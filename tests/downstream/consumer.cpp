// Downstream consumer of the installed Maintenance Coordinator package.
//
// This file is compiled by tests/downstream, an independent CMake project that
// is configured against an installed prefix and can see nothing else: no source
// tree, no build tree, no internal headers.  It therefore exercises the
// *installed* artifact, and it exercises it at run time rather than only at link
// time - it opens a real store on disk, installs a facility model, and drives a
// whole maintenance window through the public command surface.
//
// What it proves, in order:
//   * the installed headers compile and the installed library links;
//   * a store can be created from scratch in a caller-supplied directory;
//   * the lifecycle gates hold: approve needs evaluation, begin needs every
//     pre-drain and isolation obligation satisfied, verify needs completion
//     evidence, complete needs verified restoration;
//   * an acknowledgement is not an effect: a DrainAcknowledged receipt leaves
//     the drain obligation Acknowledged and begin still refused;
//   * the accepted state is durable: the completed plan is still Complete after
//     the store is closed and reopened.
//
// It uses public headers only.  Exit status is 0 only when every check passed.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "mc/engine.hpp"
#include "mc/facility.hpp"
#include "mc/ident.hpp"
#include "mc/obligation.hpp"
#include "mc/plan.hpp"
#include "mc/scope.hpp"
#include "mc/status.hpp"
#include "mc/store.hpp"
#include "mc/time.hpp"

using namespace mc;  // NOLINT(google-build-using-namespace)

namespace {

// A fixed instant, a fixed window and a clock placed inside it, so the run is
// reproducible to the nanosecond.
constexpr Timestamp kObservedAt = 1'700'000'000'000'000'000LL;
constexpr Timestamp kWindowStart = 1'700'000'600'000'000'000LL;
constexpr Timestamp kWindowEnd = kWindowStart + (8LL * kNanosPerHour);
constexpr Timestamp kNow = kWindowStart + (2LL * kNanosPerHour);
constexpr std::int64_t kApprovalValidity = 8LL * kNanosPerHour;

int g_checks = 0;
std::string g_failure;

void say(const std::string& line) { std::cout << line << '\n'; }

void milestone(int index, const std::string& name, const std::string& detail) {
    std::cout << "MILESTONE " << (index < 10 ? "0" : "") << index << ' ' << name << ": " << detail << '\n';
}

bool check(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        g_failure = what;
        say("  CHECK FAILED: " + what);
    }
    return condition;
}

// A command that must be refused, with the exact code the engine's own
// precedence rules promise.
bool check_refused(const Result<CommandResult>& result, ErrorCode expected, const std::string& what) {
    if (result.ok()) {
        return check(false, what + " [the command was accepted]");
    }
    if (result.status().code() != expected) {
        return check(false, what + " [got " + std::string(to_string(result.status().code())) + ", expected " +
                                   std::string(to_string(expected)) + "]");
    }
    return check(true, what);
}

int fail(const std::string& what, const Status& status) {
    say("FAILURE: " + what + ": " + status.render());
    return 1;
}

int finish() {
    if (!g_failure.empty()) {
        say("DOWNSTREAM CONSUMER FAILED: " + g_failure);
        return 1;
    }
    say("DOWNSTREAM CONSUMER OK: " + std::to_string(g_checks) + " checks");
    return 0;
}

template <typename Id>
Id id_of(const char* text) {
    return Id::parse(text).value();
}

TargetRef target_of(TargetKind kind, const char* name) {
    return TargetRef::make(kind, name).value();
}

MaintenanceScope rack_scope(const char* rack) {
    MaintenanceScope scope({target_of(TargetKind::Rack, rack)});
    scope.canonicalize();
    return scope;
}

// Every mutating command carries an attempt identity and the digest of the
// intent it believes it is executing.
template <typename Request>
Request stamped(Request request, const std::string& attempt) {
    request.header.attempt = id_of<AttemptId>(attempt.c_str());
    request.header.actor = "downstream-operator";
    request.header.intent_digest = intent_digest_of(request);
    return request;
}

std::size_t count_stage(const PlanRecord& record, ObligationStage stage) {
    std::size_t count = 0;
    for (const auto& obligation : record.obligations.obligations) {
        if (stage_of(obligation.kind) == stage) {
            ++count;
        }
    }
    return count;
}

const Obligation* find_kind(const PlanRecord& record, ObligationKind kind) {
    for (const auto& obligation : record.obligations.obligations) {
        if (obligation.kind == kind) {
            return &obligation;
        }
    }
    return nullptr;
}

// One site, two racks, four assets, one redundancy group, one capacity pool,
// one power domain, one cooling zone, one fabric segment.  Every generation is
// explicit and every measurement is either taken or explicitly absent.
FacilitySnapshot make_facility() {
    FacilitySnapshot facility;
    facility.revision = Revision::from_value(1);
    facility.facility_epoch = FacilityEpoch::from_value(1);
    facility.policy_generation = PolicyGeneration::from_value(1);
    facility.dependency_generation = DependencyGeneration::from_value(1);
    facility.capacity_generation = CapacityGeneration::from_value(1);
    facility.topology_generation = TopologyGeneration::from_value(1);
    facility.maintenance_generation = MaintenanceGeneration::from_value(1);
    facility.control_epoch = ControlEpoch::from_value(1);
    facility.observed_at = kObservedAt;

    facility.policy.id = id_of<PolicyId>("policy-downstream");
    facility.policy.revision = Revision::from_value(1);
    facility.policy.generation = PolicyGeneration::from_value(1);
    facility.policy.min_redundancy_margin_units = 0;
    facility.policy.min_spare_capacity_units = 0;
    facility.policy.min_power_headroom_milliwatts = 1;
    facility.policy.min_cooling_headroom_units = 1;
    facility.policy.approval_validity_nanos = kApprovalValidity;
    facility.policy.max_window_duration_nanos = 7LL * kNanosPerDay;
    facility.policy.max_concurrent_windows_per_rack = 1;
    facility.policy.require_personnel_evidence = true;
    facility.policy.require_dual_approval = false;
    facility.policy.require_work_stop_evidence = true;
    facility.policy.require_restoration_evidence = true;
    facility.policy.finalize();

    const SiteId site = id_of<SiteId>("site-a");
    const RackId rack_one = id_of<RackId>("rack-01");
    const RackId rack_two = id_of<RackId>("rack-02");
    const char* const asset_names[4] = {"asset-01", "asset-02", "asset-03", "asset-04"};
    for (int index = 0; index < 4; ++index) {
        AssetRecord asset;
        asset.id = id_of<AssetId>(asset_names[index]);
        asset.rack = index < 2 ? rack_one : rack_two;
        asset.site = site;
        asset.lifecycle = LifecycleState::InService;
        asset.service_class = ServiceClass::BusinessCritical;
        asset.lifecycle_generation = LifecycleGeneration::from_value(1);
        asset.hardware_generation = HardwareGeneration::from_value(1);
        asset.firmware_generation = FirmwareGeneration::from_value(1);
        asset.capacity_units = 1;
        asset.isolatable = true;
        asset.drainable = true;
        asset.healthy = true;
        facility.assets.push_back(asset);
    }

    RedundancyGroup group;
    group.id = id_of<RedundancyGroupId>("rg-a");
    group.site = site;
    group.mode = RedundancyMode::NPlusTwo;
    group.required_units = 2;
    group.observed_available_units = 4;
    group.members = {id_of<AssetId>("asset-01"), id_of<AssetId>("asset-02"), id_of<AssetId>("asset-03"),
                     id_of<AssetId>("asset-04")};
    facility.redundancy_groups.push_back(group);

    CapacityPool pool;
    pool.id = id_of<CapacityPoolId>("pool-a");
    pool.site = site;
    pool.units_total = 10;
    pool.units_available = 8;
    pool.units_protected = 2;
    facility.capacity_pools.push_back(pool);

    PowerDomain power;
    power.id = id_of<PowerDomainId>("pd-a");
    power.site = site;
    power.headroom.measured = true;
    power.headroom.available_units = 20;
    power.headroom.required_units = 5;
    facility.power_domains.push_back(power);

    CoolingZone cooling;
    cooling.id = id_of<CoolingZoneId>("cz-a");
    cooling.site = site;
    cooling.headroom.measured = true;
    cooling.headroom.available_units = 20;
    cooling.headroom.required_units = 5;
    facility.cooling_zones.push_back(cooling);

    FabricSegment segment;
    segment.id = id_of<FabricSegmentId>("fs-a");
    segment.site = site;
    segment.available_paths = 4;
    segment.required_paths = 2;
    segment.drainable = true;
    facility.fabric_segments.push_back(segment);

    facility.finalize();
    return facility;
}

// Ingests the satisfying observation for every obligation of one stage.  Only
// public API: the kind of evidence that satisfies an obligation is asked of the
// library, never guessed here.
Status satisfy_stage(Coordinator& coordinator, std::uint64_t& sequence, const PlanRecord& record,
                     ObligationStage stage, const std::string& prefix) {
    for (const auto& obligation : record.obligations.obligations) {
        if (stage_of(obligation.kind) != stage) {
            continue;
        }
        IngestReceiptRequest ingest;
        ingest.plan = record.plan.id;
        ingest.revision = record.plan.revision;
        ingest.receipt_id = id_of<ReceiptId>(("receipt-" + prefix + "-" + obligation.id.name()).c_str());
        ingest.obligation = obligation.id;
        ingest.kind = satisfying_receipt(obligation.kind);
        ingest.issuer_authority = authority_of(obligation.kind);
        ingest.issuer = std::string(to_string(authority_of(obligation.kind))) + "-authority";
        ingest.observation_sequence = ObservationSequence::from_value(++sequence);
        ingest.evidence = "downstream evidence for " + obligation.id.name();
        ingest.observed_at = kNow;
        auto result = coordinator.ingest_receipt(stamped(ingest, "ingest-" + prefix + "-" + obligation.id.name()));
        if (!result.ok()) {
            return result.status();
        }
    }
    return Status::success();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        say("usage: mc_downstream_consumer <store-directory>");
        return 1;
    }

    std::error_code ec;
    const std::filesystem::path store_dir = std::filesystem::absolute(std::filesystem::path(argv[1]), ec);
    if (ec) {
        say("FAILURE: cannot resolve the store directory: " + ec.message());
        return 1;
    }
    std::filesystem::remove_all(store_dir, ec);
    if (ec) {
        say("FAILURE: cannot clear the store directory: " + ec.message());
        return 1;
    }
    std::filesystem::create_directories(store_dir, ec);
    if (ec) {
        say("FAILURE: cannot create the store directory: " + ec.message());
        return 1;
    }

    StoreOptions options;
    options.directory = store_dir;
    options.create_if_missing = true;
    options.fsync = true;

    auto opened = Store::open(options);
    if (!opened.ok()) {
        return fail("open the store", opened.status());
    }
    std::unique_ptr<Store> store = std::move(opened).value();
    milestone(1, "store-directory", store_dir.string() + " created and cleared");

    const FacilitySnapshot facility = make_facility();

    std::uint64_t sequence = 0;
    PlanId plan;
    Revision revision;
    std::size_t expected_receipts = 0;

    {
        ManualClock clock(kNow);
        Coordinator coordinator(*store, clock);

        InstallFacilityRequest install;
        install.facility = facility;
        auto installed = coordinator.install_facility(stamped(install, "downstream-install"));
        if (!installed.ok()) {
            return fail("install the facility snapshot", installed.status());
        }
        milestone(2, "install-facility",
                  "site-a, 2 racks, 4 assets, 1 redundancy group, generations=1 observed_at=" +
                      std::to_string(facility.observed_at));

        ProposeRequest proposal;
        proposal.reason = "downstream packaging validation";
        proposal.requested_by = "downstream-operator";
        proposal.activity = MaintenanceActivity::HardwareRepair;
        proposal.priority = PlanPriority::Elevated;
        proposal.risk = ServiceRiskClass::High;
        proposal.scope = rack_scope("rack-01");
        proposal.window.start = kWindowStart;
        proposal.window.end = kWindowEnd;
        proposal.window.flexible = false;

        auto proposed = coordinator.propose(stamped(proposal, "downstream-propose"));
        if (!proposed.ok()) {
            return fail("propose", proposed.status());
        }
        plan = proposed.value().plan.plan.id;
        revision = proposed.value().plan.plan.revision;
        check(proposed.value().plan.phase == PlanPhase::Proposed, "propose leaves the plan in Proposed");
        check(revision.value() == 1U, "the first revision of the plan is 1");
        milestone(3, "propose", "phase=Proposed revision=1 scope=rack-01");

        EvaluateRequest evaluate;
        evaluate.plan = plan;
        evaluate.revision = revision;
        auto evaluated = coordinator.evaluate(stamped(evaluate, "downstream-evaluate"));
        if (!evaluated.ok()) {
            return fail("evaluate", evaluated.status());
        }
        const bool evaluated_clean = evaluated.value().plan.evaluation.has_value() &&
                                     evaluated.value().plan.evaluation->satisfied;
        check(evaluated.value().plan.phase == PlanPhase::Evaluating, "evaluate leaves the plan in Evaluating");
        check(evaluated_clean, "every precondition holds on the installed facility model");
        milestone(4, "evaluate", "phase=Evaluating conditions=" +
                                      std::to_string(evaluated.value().plan.evaluation->conditions.size()));

        ApproveRequest approve;
        approve.plan = plan;
        approve.revision = revision;
        approve.approved_by = "downstream-approver";
        approve.approval_evidence = "change-record-downstream-1";
        auto approved = coordinator.approve(stamped(approve, "downstream-approve"));
        if (!approved.ok()) {
            return fail("approve", approved.status());
        }
        check(approved.value().plan.phase == PlanPhase::Approved, "approve leaves the plan in Approved");
        const bool has_approval = approved.value().plan.approval.has_value();
        check(has_approval, "an approval record is bound to the plan");
        if (has_approval) {
            const ApprovalRecord& approval = *approved.value().plan.approval;
            check(approval.expires_at - approval.approved_at == kApprovalValidity,
                  "the policy approval validity of 8h is applied to the approval");
        }
        milestone(5, "approve", "phase=Approved approved_by=downstream-approver");

        DeriveObligationsRequest derive;
        derive.plan = plan;
        derive.revision = revision;
        auto derived = coordinator.derive_obligations(stamped(derive, "downstream-derive"));
        if (!derived.ok()) {
            return fail("derive obligations", derived.status());
        }
        const PlanRecord record = derived.value().plan;
        const std::size_t pre_drain = count_stage(record, ObligationStage::PreDrain);
        const std::size_t isolation = count_stage(record, ObligationStage::Isolation);
        const std::size_t completion = count_stage(record, ObligationStage::Completion);
        expected_receipts = pre_drain + isolation + completion + 1U;
        check(record.phase == PlanPhase::PreDrain, "derive-obligations leaves the plan in PreDrain");
        check(pre_drain > 0 && isolation > 0 && completion > 0,
              "pre-drain, isolation and completion obligations are all derived");
        milestone(6, "derive-obligations", "phase=PreDrain pre-drain=" + std::to_string(pre_drain) +
                                               " isolation=" + std::to_string(isolation) +
                                               " completion=" + std::to_string(completion));

        const Obligation* drain = find_kind(record, ObligationKind::AsiDrain);
        if (drain == nullptr) {
            say("FAILURE: no AsiDrain obligation was derived");
            return 1;
        }

        // An acknowledgement is not an effect.  Ingesting it must not satisfy
        // the obligation it answers, and it must not let the work begin.
        IngestReceiptRequest acknowledgement;
        acknowledgement.plan = plan;
        acknowledgement.revision = revision;
        acknowledgement.receipt_id = id_of<ReceiptId>("receipt-acknowledged-1");
        acknowledgement.obligation = drain->id;
        acknowledgement.kind = ReceiptKind::DrainAcknowledged;
        acknowledgement.issuer_authority = ObligationAuthority::Asi;
        acknowledgement.issuer = "asi-authority";
        acknowledgement.observation_sequence = ObservationSequence::from_value(++sequence);
        acknowledgement.evidence = "ASI accepted the drain request";
        acknowledgement.observed_at = kNow;
        auto acknowledged = coordinator.ingest_receipt(stamped(acknowledgement, "downstream-acknowledge"));
        if (!acknowledged.ok()) {
            return fail("ingest the drain acknowledgement", acknowledged.status());
        }
        const ObligationStatus* ack_status = acknowledged.value().obligations.has_value()
                                                 ? acknowledged.value().obligations->find(drain->id)
                                                 : nullptr;
        check(ack_status != nullptr && ack_status->disposition == Disposition::Acknowledged,
              "DrainAcknowledged leaves the drain obligation Acknowledged, not Satisfied");
        check(acknowledged.value().plan.phase == PlanPhase::PreDrain, "an acknowledgement does not advance the plan");
        milestone(7, "acknowledgement-is-not-an-effect",
                  "obligation=" + drain->id.name() + " disposition=" +
                      std::string(ack_status != nullptr ? to_string(ack_status->disposition) : "unknown"));

        BeginRequest begin;
        begin.plan = plan;
        begin.revision = revision;
        check_refused(coordinator.begin(stamped(begin, "downstream-begin-too-early")),
                      ErrorCode::ObligationsOutstanding,
                      "begin is refused while the drain obligation is only acknowledged");
        milestone(8, "begin-refused", "code=ObligationsOutstanding (obligations outstanding)");

        Status satisfied = satisfy_stage(coordinator, sequence, record, ObligationStage::PreDrain, "pre-drain");
        if (!satisfied.ok()) {
            return fail("ingest the pre-drain observations", satisfied);
        }
        ShowRequest show;
        show.plan = plan;
        auto shown = coordinator.show(show);
        if (!shown.ok()) {
            return fail("show after the pre-drain stage", shown.status());
        }
        check(shown.value().plan.phase == PlanPhase::PreDrain,
              "the plan stays in PreDrain while isolation is still outstanding");
        check_refused(coordinator.begin(stamped(begin, "downstream-begin-pre-drain-only")),
                      ErrorCode::ObligationsOutstanding,
                      "begin is still refused with only the pre-drain stage satisfied");

        satisfied = satisfy_stage(coordinator, sequence, record, ObligationStage::Isolation, "isolation");
        if (!satisfied.ok()) {
            return fail("ingest the isolation observations", satisfied);
        }
        shown = coordinator.show(show);
        if (!shown.ok()) {
            return fail("show after the isolation stage", shown.status());
        }
        check(shown.value().plan.phase == PlanPhase::Ready, "the plan is Ready once isolation is satisfied");
        milestone(9, "obligations-satisfied", "pre-drain=" + std::to_string(pre_drain) +
                                                  " isolation=" + std::to_string(isolation) + " phase=Ready");

        auto begun = coordinator.begin(stamped(begin, "downstream-begin"));
        if (!begun.ok()) {
            return fail("begin", begun.status());
        }
        check(begun.value().plan.phase == PlanPhase::InProgress, "begin leaves the plan in InProgress");
        milestone(10, "begin", "phase=InProgress at " + std::to_string(kNow));

        CompleteRequest complete;
        complete.plan = plan;
        complete.revision = revision;
        complete.note = "premature completion attempt";
        check_refused(coordinator.complete(stamped(complete, "downstream-complete-early")), ErrorCode::IllegalPhase,
                      "complete is refused while no work, evidence or restoration exists");
        milestone(11, "complete-refused", "code=IllegalPhase");

        RecordProgressRequest progress;
        progress.plan = plan;
        progress.revision = revision;
        progress.kind = ProgressKind::WorkCompleted;
        progress.note = "downstream work finished";
        auto progressed = coordinator.record_progress(stamped(progress, "downstream-progress"));
        if (!progressed.ok()) {
            return fail("record progress", progressed.status());
        }
        check(progressed.value().plan.phase == PlanPhase::Verification,
              "work-completed moves the plan to Verification");

        VerifyRequest verify;
        verify.plan = plan;
        verify.revision = revision;
        check_refused(coordinator.verify_restoration(stamped(verify, "downstream-verify-early")),
                      ErrorCode::CompletionEvidenceMissing,
                      "verify-restoration is refused before the completion evidence exists");
        milestone(12, "work-completed", "phase=Verification verify refused with CompletionEvidenceMissing");

        satisfied = satisfy_stage(coordinator, sequence, record, ObligationStage::Completion, "completion");
        if (!satisfied.ok()) {
            return fail("ingest the completion observations", satisfied);
        }
        milestone(13, "completion-evidence", "completion receipts=" + std::to_string(completion));

        auto verified = coordinator.verify_restoration(stamped(verify, "downstream-verify"));
        if (!verified.ok()) {
            return fail("verify restoration", verified.status());
        }
        check(verified.value().plan.phase == PlanPhase::Restore, "verify-restoration leaves the plan in Restore");
        check(verified.value().restoration.has_value() && verified.value().restoration->restored,
              "the restoration report is present and reports the facility restored");
        milestone(14, "verify-restoration", "phase=Restore restored=true");

        auto completed = coordinator.complete(stamped(complete, "downstream-complete"));
        if (!completed.ok()) {
            return fail("complete", completed.status());
        }
        check(completed.value().plan.phase == PlanPhase::Complete, "complete leaves the plan in Complete");
        check(completed.value().completion.has_value() && completed.value().completion->restoration_verified,
              "the completion report records verified restoration");
        check(completed.value().completion.has_value() &&
                  completed.value().completion->protected_obligations_satisfied,
              "the completion report records satisfied protected obligations");
        milestone(15, "complete", "phase=Complete");
    }

    // The installed library must have made the state durable, not merely
    // remembered it: drop the writer, reopen the store and read the plan back.
    store.reset();
    auto reopened = Store::open(options);
    if (!reopened.ok()) {
        return fail("reopen the store", reopened.status());
    }
    std::unique_ptr<Store> reopened_store = std::move(reopened).value();
    ManualClock reopen_clock(kNow);
    Coordinator reader(*reopened_store, reopen_clock);
    ShowRequest show;
    show.plan = plan;
    auto persisted = reader.show(show);
    if (!persisted.ok()) {
        return fail("show after reopening the store", persisted.status());
    }
    check(persisted.value().plan.phase == PlanPhase::Complete,
          "the completed plan is still Complete after the store is closed and reopened");
    check(persisted.value().plan.receipts.size() >= expected_receipts,
          "every ingested receipt survived the reopen");
    milestone(16, "durable-state", "reopened store reports phase=" +
                                       std::string(to_string(persisted.value().plan.phase)) + " receipts=" +
                                       std::to_string(persisted.value().plan.receipts.size()));

    return finish();
}

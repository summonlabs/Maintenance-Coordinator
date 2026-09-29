// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Model tests: canonical scope handling, enum spellings, the facility snapshot
// and its scope resolution, plan / record / approval / exception semantics, and
// white box round trips through the internal canonical codec.
//
// Every case is deterministic: no wall clock, no environment, no network, no
// machine specific path and no test timeout.  Where the subject under test is a
// whole snapshot or record, the helpers below build one canonical value and the
// cases mutate a copy of it, so a check never depends on incidental ordering.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The public headers come first: mc_test.hpp adds stream inserters for Digest
// and PlanPhase, so both types must already be declared when it is read.
#include "mc/approval.hpp"
#include "mc/digest.hpp"
#include "mc/facility.hpp"
#include "mc/ident.hpp"
#include "mc/obligation.hpp"
#include "mc/plan.hpp"
#include "mc/scope.hpp"
#include "mc/time.hpp"

#include "../mc_test.hpp"

// The internal canonical codec, for white box round trips only.
#include "serialize.hpp"

using namespace mc;

namespace {

// 2023-11-14T22:13:20Z, so no test depends on when it runs.
constexpr Timestamp kT0 = 1700000000LL * kNanosPerSecond;

// ---------------------------------------------------------------------------
// Small builders
// ---------------------------------------------------------------------------

[[nodiscard]] TargetRef target_of(TargetKind kind, std::string_view name) {
    auto made = TargetRef::make(kind, name);
    MC_REQUIRE(made.ok());
    return made.value();
}

template <typename Id>
[[nodiscard]] Id id_of(std::string_view name) {
    auto parsed = Id::parse(name);
    MC_REQUIRE(parsed.ok());
    return parsed.value();
}

template <typename Tag>
[[nodiscard]] std::vector<std::string> names_of(const std::vector<Ident<Tag>>& ids) {
    std::vector<std::string> result;
    result.reserve(ids.size());
    for (const auto& id : ids) {
        result.push_back(id.name());
    }
    return result;
}

template <typename T>
[[nodiscard]] T* find_by_id(std::vector<T>& items, std::string_view name) {
    for (auto& item : items) {
        if (item.id.name() == name) {
            return &item;
        }
    }
    return nullptr;
}

// Every enumerator survives to_string()/parse() and no two share a spelling.
template <typename Enum, std::size_t N>
void check_enum_round_trip(const std::array<Enum, N>& values, bool (*parse)(std::string_view, Enum&)) {
    for (std::size_t index = 0; index < N; ++index) {
        const std::string_view name = to_string(values[index]);
        MC_CHECK_MSG(!name.empty(), "an enumerator has no textual spelling");
        Enum parsed = values[index];
        MC_CHECK_MSG(parse(name, parsed), "could not parse " + std::string(name));
        MC_CHECK(parsed == values[index]);
        for (std::size_t other = index + 1U; other < N; ++other) {
            MC_CHECK_MSG(to_string(values[other]) != name, "two enumerators share one spelling");
        }
    }
    // An unknown spelling is rejected and the output is left untouched.
    Enum untouched = values[0];
    MC_CHECK(!parse("not-a-real-enumerator", untouched));
    MC_CHECK(!parse("", untouched));
    MC_CHECK(untouched == values[0]);
}

// ---------------------------------------------------------------------------
// A complete, valid facility snapshot
// ---------------------------------------------------------------------------

[[nodiscard]] AssetRecord make_asset(std::string_view id, std::string_view rack, std::string_view site,
                                     std::uint32_t capacity_units,
                                     LifecycleState lifecycle = LifecycleState::InService) {
    AssetRecord record;
    record.id = id_of<AssetId>(id);
    record.rack = id_of<RackId>(rack);
    record.site = id_of<SiteId>(site);
    record.lifecycle = lifecycle;
    record.service_class = ServiceClass::BusinessCritical;
    record.lifecycle_generation = LifecycleGeneration::from_value(3U);
    record.hardware_generation = HardwareGeneration::from_value(5U);
    record.firmware_generation = FirmwareGeneration::from_value(2U);
    record.capacity_units = capacity_units;
    record.isolatable = true;
    record.drainable = true;
    record.healthy = true;
    return record;
}

// asset-a1 / asset-a2 sit in rack-a1 of site-a, asset-a3 in rack-a2 of site-a,
// asset-b1 in rack-b1 of site-b.  rg-a and po-a protect asset-a1 / asset-a2.
[[nodiscard]] FacilitySnapshot make_facility() {
    FacilitySnapshot snapshot;
    snapshot.revision = Revision::from_value(11U);
    snapshot.facility_epoch = FacilityEpoch::from_value(2U);
    snapshot.policy_generation = PolicyGeneration::from_value(4U);
    snapshot.dependency_generation = DependencyGeneration::from_value(6U);
    snapshot.capacity_generation = CapacityGeneration::from_value(1U);
    snapshot.topology_generation = TopologyGeneration::from_value(9U);
    snapshot.maintenance_generation = MaintenanceGeneration::from_value(3U);
    snapshot.control_epoch = ControlEpoch::from_value(7U);
    snapshot.observed_at = kT0;

    snapshot.policy.id = id_of<PolicyId>("policy-1");
    snapshot.policy.revision = Revision::from_value(2U);
    snapshot.policy.generation = PolicyGeneration::from_value(4U);
    snapshot.policy.min_redundancy_margin_units = 1U;
    snapshot.policy.min_spare_capacity_units = 2U;
    snapshot.policy.min_power_headroom_milliwatts = 100;
    snapshot.policy.min_cooling_headroom_units = 50;
    snapshot.policy.service_class_ceiling_note = "business-critical";
    snapshot.policy.finalize();

    snapshot.assets = {make_asset("asset-a1", "rack-a1", "site-a", 10U),
                       make_asset("asset-a2", "rack-a1", "site-a", 10U),
                       make_asset("asset-a3", "rack-a2", "site-a", 5U),
                       make_asset("asset-b1", "rack-b1", "site-b", 20U)};

    RedundancyGroup group_a;
    group_a.id = id_of<RedundancyGroupId>("rg-a");
    group_a.site = id_of<SiteId>("site-a");
    group_a.mode = RedundancyMode::NPlusOne;
    group_a.required_units = 1U;
    group_a.observed_available_units = 2U;
    group_a.members = {id_of<AssetId>("asset-a2"), id_of<AssetId>("asset-a1")};
    RedundancyGroup group_b;
    group_b.id = id_of<RedundancyGroupId>("rg-b");
    group_b.site = id_of<SiteId>("site-b");
    group_b.mode = RedundancyMode::N;
    group_b.required_units = 1U;
    group_b.observed_available_units = 1U;
    group_b.members = {id_of<AssetId>("asset-b1")};
    snapshot.redundancy_groups = {group_b, group_a};

    CapacityPool pool_a;
    pool_a.id = id_of<CapacityPoolId>("pool-a");
    pool_a.site = id_of<SiteId>("site-a");
    pool_a.units_total = 40U;
    pool_a.units_available = 25U;
    pool_a.units_protected = 5U;
    CapacityPool pool_b;
    pool_b.id = id_of<CapacityPoolId>("pool-b");
    pool_b.site = id_of<SiteId>("site-b");
    pool_b.units_total = 20U;
    pool_b.units_available = 20U;
    pool_b.units_protected = 0U;
    snapshot.capacity_pools = {pool_b, pool_a};

    PowerDomain domain_a;
    domain_a.id = id_of<PowerDomainId>("pd-a");
    domain_a.site = id_of<SiteId>("site-a");
    domain_a.headroom.measured = true;
    domain_a.headroom.available_units = 1000;
    domain_a.headroom.required_units = 200;
    domain_a.interlocked = false;
    PowerDomain domain_b;
    domain_b.id = id_of<PowerDomainId>("pd-b");
    domain_b.site = id_of<SiteId>("site-b");
    domain_b.headroom.measured = true;
    domain_b.headroom.available_units = 500;
    domain_b.headroom.required_units = 100;
    domain_b.interlocked = false;
    snapshot.power_domains = {domain_b, domain_a};

    CoolingZone zone_a;
    zone_a.id = id_of<CoolingZoneId>("cz-a");
    zone_a.site = id_of<SiteId>("site-a");
    zone_a.headroom.measured = true;
    zone_a.headroom.available_units = 400;
    zone_a.headroom.required_units = 100;
    zone_a.interlocked = false;
    snapshot.cooling_zones = {zone_a};

    FabricSegment segment_a;
    segment_a.id = id_of<FabricSegmentId>("fs-a");
    segment_a.site = id_of<SiteId>("site-a");
    segment_a.available_paths = 4U;
    segment_a.required_paths = 2U;
    segment_a.drainable = true;
    snapshot.fabric_segments = {segment_a};

    BlackoutPeriod blackout_rack;
    blackout_rack.id = id_of<BlackoutId>("bo-rack-a1");
    blackout_rack.targets = {target_of(TargetKind::Rack, "rack-a1")};
    blackout_rack.start = kT0;
    blackout_rack.end = kT0 + kNanosPerHour;
    blackout_rack.hard = true;
    blackout_rack.reason = "scheduled plant work";
    BlackoutPeriod blackout_facility;
    blackout_facility.id = id_of<BlackoutId>("bo-facility");
    blackout_facility.start = kT0 + 2LL * kNanosPerHour;
    blackout_facility.end = kT0 + 3LL * kNanosPerHour;
    blackout_facility.hard = true;
    blackout_facility.reason = "facility wide test";
    snapshot.blackouts = {blackout_facility, blackout_rack};

    IncidentRecord incident_active;
    incident_active.id = id_of<IncidentId>("inc-active-a1");
    incident_active.targets = {target_of(TargetKind::Asset, "asset-a1")};
    incident_active.severity = Severity::High;
    incident_active.active = true;
    incident_active.hard_block = true;
    incident_active.summary = "psu failure";
    IncidentRecord incident_quiet;
    incident_quiet.id = id_of<IncidentId>("inc-quiet-b1");
    incident_quiet.targets = {target_of(TargetKind::Rack, "rack-b1")};
    incident_quiet.severity = Severity::Low;
    incident_quiet.active = false;
    incident_quiet.hard_block = false;
    incident_quiet.summary = "closed incident";
    snapshot.incidents = {incident_quiet, incident_active};

    ProtectedObligation protection_a;
    protection_a.id = id_of<ProtectionId>("po-a");
    protection_a.targets = {target_of(TargetKind::Asset, "asset-a2"), target_of(TargetKind::Asset, "asset-a1")};
    protection_a.required_mode = RedundancyMode::NPlusOne;
    protection_a.required_units = 1U;
    protection_a.tolerance_units = 0U;
    protection_a.hard_interlock = true;
    protection_a.description = "keep one unit serving";
    ProtectedObligation protection_b;
    protection_b.id = id_of<ProtectionId>("po-b");
    protection_b.targets = {target_of(TargetKind::Site, "site-b")};
    protection_b.required_mode = RedundancyMode::N;
    protection_b.required_units = 1U;
    protection_b.tolerance_units = 0U;
    protection_b.hard_interlock = true;
    protection_b.description = "site-b keeps one unit serving";
    snapshot.protected_obligations = {protection_b, protection_a};

    snapshot.finalize();
    return snapshot;
}

// Reverses every vector, including the ones nested inside records.
[[nodiscard]] FacilitySnapshot permuted(FacilitySnapshot snapshot) {
    const auto flip = [](auto& items) { std::reverse(items.begin(), items.end()); };
    flip(snapshot.assets);
    flip(snapshot.redundancy_groups);
    flip(snapshot.capacity_pools);
    flip(snapshot.power_domains);
    flip(snapshot.cooling_zones);
    flip(snapshot.fabric_segments);
    flip(snapshot.blackouts);
    flip(snapshot.incidents);
    flip(snapshot.protected_obligations);
    for (auto& group : snapshot.redundancy_groups) {
        flip(group.members);
    }
    for (auto& blackout : snapshot.blackouts) {
        flip(blackout.targets);
    }
    for (auto& incident : snapshot.incidents) {
        flip(incident.targets);
    }
    for (auto& protection : snapshot.protected_obligations) {
        flip(protection.targets);
    }
    return snapshot;
}

void check_snapshot_rejected(FacilitySnapshot snapshot, ErrorCode expected, const char* what) {
    snapshot.finalize();
    const Status status = snapshot.validate();
    MC_CHECK_MSG(status.code() == expected,
                 std::string(what) + ": expected " + std::string(to_string(expected)) + ", got " +
                     std::string(to_string(status.code())) + " -- " + status.render());
}

// ---------------------------------------------------------------------------
// A complete, valid plan and plan record
// ---------------------------------------------------------------------------

[[nodiscard]] MaintenancePlan make_plan() {
    MaintenancePlan plan;
    plan.id = id_of<PlanId>("plan-1");
    plan.revision = Revision::from_value(7U);
    plan.reason = "replace fan tray in rack-a1";
    plan.requested_by = "operator-1";
    plan.activity = MaintenanceActivity::HardwareRepair;
    plan.priority = PlanPriority::Elevated;
    plan.risk = ServiceRiskClass::High;
    plan.scope = MaintenanceScope({target_of(TargetKind::Rack, "rack-a1")});
    plan.window = WindowSpec{kT0, kT0 + 2LL * kNanosPerHour, false};
    plan.facility_epoch = FacilityEpoch::from_value(2U);
    plan.control_epoch = ControlEpoch::from_value(7U);
    plan.policy_generation = PolicyGeneration::from_value(4U);
    plan.dependency_generation = DependencyGeneration::from_value(6U);
    plan.capacity_generation = CapacityGeneration::from_value(1U);
    plan.topology_generation = TopologyGeneration::from_value(9U);
    plan.maintenance_generation = MaintenanceGeneration::from_value(3U);
    plan.policy_digest = digest_of(purpose::kPolicy, std::string_view("policy-body"));
    plan.dependency_digest = digest_of(purpose::kDependencySnapshot, std::string_view("dependency-body"));
    plan.finalize();
    return plan;
}

[[nodiscard]] Obligation make_obligation(std::string_view id, ObligationKind kind, const TargetRef& target) {
    Obligation obligation;
    obligation.id = id_of<ObligationId>(id);
    obligation.kind = kind;
    obligation.authority = authority_of(kind);
    obligation.target = target;
    obligation.requirement = std::string("prove ") + std::string(to_string(kind));
    obligation.mandatory = true;
    obligation.requires_observation = true;
    obligation.guarded_by = ErrorCode::Ok;
    return obligation;
}

[[nodiscard]] Receipt make_receipt(std::string_view id, std::string_view obligation, ReceiptKind kind,
                                   std::uint64_t sequence, Revision revision) {
    Receipt receipt;
    receipt.id = id_of<ReceiptId>(id);
    receipt.obligation = id_of<ObligationId>(obligation);
    receipt.kind = kind;
    receipt.issuer_authority = ObligationAuthority::Asi;
    receipt.issuer = "asi-1";
    receipt.observation_sequence = ObservationSequence::from_value(sequence);
    receipt.plan_revision = revision;
    receipt.obligation_digest = digest_of(purpose::kObligationSet, std::string_view("obligation-set"));
    receipt.evidence_digest = digest_of(purpose::kEvidenceDigest, std::string_view(id));
    receipt.evidence = "ticket-" + std::string(id);
    receipt.observed_at = kT0 + static_cast<Timestamp>(sequence) * kNanosPerSecond;
    receipt.ingested_at = receipt.observed_at + kNanosPerSecond;
    return receipt;
}

[[nodiscard]] PreconditionReport make_evaluation(const MaintenancePlan& plan) {
    PreconditionReport report;
    report.plan_revision = plan.revision;
    report.plan_digest = plan.digest;
    report.evaluated_at = kT0 + 10LL * kNanosPerSecond;
    report.facility_epoch = FacilityEpoch::from_value(2U);
    report.control_epoch = ControlEpoch::from_value(7U);
    report.policy_generation = PolicyGeneration::from_value(4U);
    report.dependency_generation = DependencyGeneration::from_value(6U);
    report.capacity_generation = CapacityGeneration::from_value(1U);
    report.topology_generation = TopologyGeneration::from_value(9U);
    report.maintenance_generation = MaintenanceGeneration::from_value(3U);
    report.facility_digest = digest_of(purpose::kFacilitySnapshot, std::string_view("facility"));
    report.policy_digest = plan.policy_digest;
    report.dependency_digest = plan.dependency_digest;

    ConditionResult redundancy;
    redundancy.condition = ErrorCode::RedundancyInsufficient;
    redundancy.satisfied = false;
    redundancy.hard = false;
    redundancy.waivable = true;
    redundancy.measured = true;
    redundancy.subject = "rg-a";
    redundancy.detail = "1 of 1 units survive";
    redundancy.observed = 1;
    redundancy.required = 1;

    ConditionResult headroom;
    headroom.condition = ErrorCode::PowerHeadroomInsufficient;
    headroom.satisfied = true;
    headroom.hard = false;
    headroom.waivable = true;
    headroom.measured = true;
    headroom.subject = "pd-a";
    headroom.detail = "800 milliwatts of margin";
    headroom.observed = 800;
    headroom.required = 100;

    ConditionResult interlock;
    interlock.condition = ErrorCode::HardInterlock;
    interlock.satisfied = false;
    interlock.hard = true;
    interlock.waivable = false;
    interlock.measured = true;
    interlock.subject = "rack-a1";
    interlock.detail = "plant interlock is closed";
    interlock.observed = 1;
    interlock.required = 0;

    report.conditions = {interlock, headroom, redundancy};  // deliberately unsorted
    report.satisfied = false;
    report.satisfied_without_exceptions = false;
    report.finalize();
    return report;
}

[[nodiscard]] ApprovalRecord make_approval(const MaintenancePlan& plan) {
    ApprovalRecord approval;
    approval.id = id_of<ApprovalId>("approval-1");
    approval.plan = plan.id;
    approval.plan_revision = plan.revision;
    approval.approved_by = "approver-1";
    approval.approval_evidence = "change-record-4711";
    approval.witness = "witness-1";
    approval.approved_at = kT0 + 20LL * kNanosPerSecond;
    approval.expires_at = approval.approved_at + 8LL * kNanosPerHour;
    approval.plan_digest = plan.digest;
    approval.facility_digest = digest_of(purpose::kFacilitySnapshot, std::string_view("facility"));
    approval.policy_digest = plan.policy_digest;
    approval.dependency_digest = plan.dependency_digest;
    approval.evaluation_digest = digest_of(purpose::kPreconditionReport, std::string_view("evaluation"));
    approval.obligation_set_digest = digest_of(purpose::kObligationSet, std::string_view("obligation-set"));
    approval.facility_epoch = FacilityEpoch::from_value(2U);
    approval.control_epoch = ControlEpoch::from_value(7U);
    approval.policy_generation = PolicyGeneration::from_value(4U);
    approval.dependency_generation = DependencyGeneration::from_value(6U);
    approval.capacity_generation = CapacityGeneration::from_value(1U);
    approval.topology_generation = TopologyGeneration::from_value(9U);
    approval.maintenance_generation = MaintenanceGeneration::from_value(3U);
    approval.exceptions = {id_of<ExceptionId>("exc-1")};
    approval.finalize();
    return approval;
}

[[nodiscard]] ExceptionGrant make_grant() {
    ExceptionGrant grant;
    grant.id = id_of<ExceptionId>("exc-1");
    grant.plan = id_of<PlanId>("plan-1");
    grant.waived = {ErrorCode::RedundancyInsufficient, ErrorCode::PowerHeadroomInsufficient};
    grant.targets = {target_of(TargetKind::Rack, "rack-a1"), target_of(TargetKind::Asset, "asset-a1")};
    grant.granted_by = "operator-1";
    grant.justification = "redundancy temporarily below the policy margin";
    grant.granted_at = kT0;
    grant.expires_at = kT0 + kNanosPerHour;
    grant.finalize();
    return grant;
}

[[nodiscard]] ProgressEvent make_progress(ProgressKind kind, std::string note, Timestamp at) {
    ProgressEvent event;
    event.kind = kind;
    event.note = std::move(note);
    event.actor = "operator-1";
    event.at = at;
    return event;
}

[[nodiscard]] PhaseTransition make_transition(PlanPhase from, PlanPhase to, Timestamp at, std::uint64_t sequence) {
    PhaseTransition transition;
    transition.from = from;
    transition.to = to;
    transition.at = at;
    transition.actor = "operator-1";
    transition.reason = std::string(to_string(from)) + " to " + std::string(to_string(to));
    transition.cause = ErrorCode::Ok;
    transition.sequence = CommitSequence::from_value(sequence);
    return transition;
}

[[nodiscard]] PlanRecord make_record() {
    PlanRecord record;
    record.plan = make_plan();
    record.phase = PlanPhase::Approved;
    record.created_at = kT0 - kNanosPerDay;
    record.updated_at = kT0 + 3LL * kNanosPerSecond;
    record.sequence = CommitSequence::from_value(12U);
    record.obligations.obligations = {
        make_obligation("ob-1", ObligationKind::AsiDrain, target_of(TargetKind::Asset, "asset-a1")),
        make_obligation("ob-2", ObligationKind::PowerIsolate, target_of(TargetKind::Rack, "rack-a1"))};
    record.transitions = {make_transition(PlanPhase::Proposed, PlanPhase::Evaluating, kT0, 1U),
                          make_transition(PlanPhase::Evaluating, PlanPhase::Approved, kT0 + kNanosPerSecond, 2U)};
    record.receipts = {make_receipt("rc-2", "ob-2", ReceiptKind::PowerIsolationObserved, 2U, record.plan.revision),
                       make_receipt("rc-1", "ob-1", ReceiptKind::DrainObserved, 1U, record.plan.revision)};
    record.progress = {make_progress(ProgressKind::StepCompleted, "note-c", kT0 + 300LL * kNanosPerSecond),
                       make_progress(ProgressKind::Started, "note-a", kT0 + 100LL * kNanosPerSecond),
                       make_progress(ProgressKind::IssueObserved, "note-b", kT0 + 200LL * kNanosPerSecond)};
    ExceptionGrant late = make_grant();
    late.id = id_of<ExceptionId>("exc-z");
    ExceptionGrant early = make_grant();
    early.id = id_of<ExceptionId>("exc-a");
    record.exceptions = {late, early};
    ConditionResult blocker;
    blocker.condition = ErrorCode::ActiveIncident;
    blocker.satisfied = false;
    blocker.hard = true;
    blocker.waivable = false;
    blocker.measured = true;
    blocker.subject = "rack-a1";
    blocker.detail = "active incident blocks the window";
    record.blockers = {blocker};
    record.last_observation_sequence = ObservationSequence::from_value(5U);
    record.evaluation = make_evaluation(record.plan);
    record.approval = make_approval(record.plan);
    record.finalize();
    return record;
}

void check_record_rejected(PlanRecord record, ErrorCode expected, const char* what) {
    record.finalize();
    const Status status = record.validate();
    MC_CHECK_MSG(status.code() == expected,
                 std::string(what) + ": expected " + std::string(to_string(expected)) + ", got " +
                     std::string(to_string(status.code())) + " -- " + status.render());
}

void check_plan_rejected(MaintenancePlan plan, ErrorCode expected, const char* what) {
    plan.finalize();
    const Status status = plan.validate();
    MC_CHECK_MSG(status.code() == expected,
                 std::string(what) + ": expected " + std::string(to_string(expected)) + ", got " +
                     std::string(to_string(status.code())) + " -- " + status.render());
}

}  // namespace

// ---------------------------------------------------------------------------
// Scope and target references
// ---------------------------------------------------------------------------

MC_TEST(maintenance_scope_canonicalization_and_set_operations) {
    const TargetRef rack_a1 = target_of(TargetKind::Rack, "rack-a1");
    const TargetRef rack_a2 = target_of(TargetKind::Rack, "rack-a2");
    const TargetRef asset_a1 = target_of(TargetKind::Asset, "asset-a1");
    const TargetRef site_a = target_of(TargetKind::Site, "site-a");
    const TargetRef site_b = target_of(TargetKind::Site, "site-b");

    MC_CHECK(MaintenanceScope{}.empty());
    MC_CHECK_EQ(MaintenanceScope{}.size(), 0U);

    MaintenanceScope scope({rack_a2, asset_a1, rack_a1, rack_a1, asset_a1});
    MC_CHECK_EQ(scope.size(), 5U);
    scope.canonicalize();
    MC_CHECK_EQ(scope.size(), 3U);
    MC_REQUIRE(scope.targets().size() == 3U);
    MC_CHECK(scope.targets()[0] == rack_a1);
    MC_CHECK(scope.targets()[1] == rack_a2);
    MC_CHECK(scope.targets()[2] == asset_a1);
    MC_CHECK(scope.contains(rack_a1));
    MC_CHECK(scope.contains(rack_a2));
    MC_CHECK(scope.contains(asset_a1));
    MC_CHECK(!scope.contains(site_a));
    MC_CHECK(!scope.contains(target_of(TargetKind::Rack, "rack-a3")));

    // canonicalize() is idempotent.
    scope.canonicalize();
    MC_CHECK_EQ(scope.size(), 3U);
    MaintenanceScope again({asset_a1, rack_a1, rack_a2});
    again.canonicalize();
    MC_CHECK(again == scope);

    // Intersection.
    const MaintenanceScope identical({rack_a1, rack_a2, asset_a1});
    const MaintenanceScope overlapping({asset_a1, site_b});
    const MaintenanceScope disjoint({site_b});
    const MaintenanceScope empty_scope;
    MC_CHECK(scope.intersects(identical));
    MC_CHECK(identical.intersects(scope));
    MC_CHECK(scope.intersects(overlapping));
    MC_CHECK(overlapping.intersects(scope));
    MC_CHECK(!scope.intersects(disjoint));
    MC_CHECK(!disjoint.intersects(scope));
    MC_CHECK(!scope.intersects(empty_scope));
    MC_CHECK(!empty_scope.intersects(scope));
    MC_CHECK(!empty_scope.intersects(empty_scope));
    MC_CHECK(identical.intersects(identical));

    // A scope built from unsorted input still intersects correctly after
    // canonicalization, and two canonical scopes compare equal by value.
    MaintenanceScope unsorted({site_b, asset_a1});
    MaintenanceScope sorted({asset_a1, site_b});
    unsorted.canonicalize();
    sorted.canonicalize();
    MC_CHECK(unsorted == sorted);
    MC_CHECK(unsorted.intersects(overlapping));
}

MC_TEST(target_ref_parse_and_str_round_trip) {
    auto no_separator = TargetRef::parse("rack");
    MC_CHECK(!no_separator.ok());
    MC_CHECK_CODE(no_separator.status(), ErrorCode::InvalidArgument);
    auto unknown_kind = TargetRef::parse("blimp:x");
    MC_CHECK(!unknown_kind.ok());
    MC_CHECK_CODE(unknown_kind.status(), ErrorCode::InvalidArgument);
    auto empty_name = TargetRef::parse("rack:");
    MC_CHECK(!empty_name.ok());
    MC_CHECK_CODE(empty_name.status(), ErrorCode::InvalidArgument);
    auto bad_name = TargetRef::parse("rack:r 07");
    MC_CHECK(!bad_name.ok());
    MC_CHECK_CODE(bad_name.status(), ErrorCode::InvalidArgument);

    auto rack = TargetRef::parse("rack:r07");
    MC_REQUIRE(rack.ok());
    MC_CHECK(rack.value().kind() == TargetKind::Rack);
    MC_CHECK_EQ(rack.value().name(), std::string("r07"));
    MC_CHECK_EQ(rack.value().str(), std::string("rack:r07"));
    MC_CHECK(!rack.value().empty());
    MC_CHECK(rack.value() == target_of(TargetKind::Rack, "r07"));

    // str() and parse() are inverse for every kind, and a colon inside a name
    // survives because only the first separator is significant.
    const std::array<std::pair<TargetKind, const char*>, 6> kinds{{
        {TargetKind::Site, "site-a"},
        {TargetKind::Rack, "rack-a1"},
        {TargetKind::Asset, "asset-a1"},
        {TargetKind::PowerDomain, "pd-a"},
        {TargetKind::CoolingZone, "cz-a"},
        {TargetKind::FabricSegment, "fs-a"},
    }};
    for (const auto& entry : kinds) {
        const TargetRef made = target_of(entry.first, entry.second);
        auto reparsed = TargetRef::parse(made.str());
        MC_CHECK_MSG(reparsed.ok(), "could not parse " + made.str());
        MC_CHECK(reparsed.ok() && reparsed.value() == made);
        MC_CHECK_EQ(std::string(to_string(entry.first)), std::string(to_string(made.kind())));
    }
    auto colon_name = TargetRef::parse("rack:a:b");
    MC_REQUIRE(colon_name.ok());
    MC_CHECK_EQ(colon_name.value().name(), std::string("a:b"));
    MC_CHECK_EQ(colon_name.value().str(), std::string("rack:a:b"));

    MC_CHECK(TargetRef{}.empty());
    MC_CHECK_EQ(TargetRef{}.str(), std::string("site:"));

    // Equality is kind sensitive; ordering is (kind, name).
    MC_CHECK(target_of(TargetKind::Rack, "x") != target_of(TargetKind::Asset, "x"));
    MC_CHECK(target_of(TargetKind::Rack, "a") < target_of(TargetKind::Rack, "b"));
    MC_CHECK(target_of(TargetKind::Site, "z") < target_of(TargetKind::Rack, "a"));
    MC_CHECK(target_of(TargetKind::Asset, "a") < target_of(TargetKind::PowerDomain, "a"));
}

// ---------------------------------------------------------------------------
// Enum spellings
// ---------------------------------------------------------------------------

MC_TEST(target_kind_text_round_trip) {
    const std::array<TargetKind, 6> values{{TargetKind::Site, TargetKind::Rack, TargetKind::Asset,
                                            TargetKind::PowerDomain, TargetKind::CoolingZone,
                                            TargetKind::FabricSegment}};
    check_enum_round_trip(values, &parse_target_kind);
    MC_CHECK_EQ(std::string(to_string(TargetKind::Site)), std::string("site"));
    MC_CHECK_EQ(std::string(to_string(TargetKind::Rack)), std::string("rack"));
    MC_CHECK_EQ(std::string(to_string(TargetKind::Asset)), std::string("asset"));
    MC_CHECK_EQ(std::string(to_string(TargetKind::PowerDomain)), std::string("power-domain"));
    MC_CHECK_EQ(std::string(to_string(TargetKind::CoolingZone)), std::string("cooling-zone"));
    MC_CHECK_EQ(std::string(to_string(TargetKind::FabricSegment)), std::string("fabric-segment"));
    MC_CHECK_EQ(std::string(to_string(static_cast<TargetKind>(200U))), std::string("unknown"));
}

MC_TEST(activity_priority_and_risk_text_round_trip) {
    const std::array<MaintenanceActivity, 10> activities{{
        MaintenanceActivity::Inspection,       MaintenanceActivity::HardwareRepair,
        MaintenanceActivity::HardwareUpgrade,  MaintenanceActivity::FirmwareUpgrade,
        MaintenanceActivity::PowerWork,        MaintenanceActivity::CoolingWork,
        MaintenanceActivity::NetworkWork,      MaintenanceActivity::RackInstall,
        MaintenanceActivity::Decommission,     MaintenanceActivity::DataWipe,
    }};
    check_enum_round_trip(activities, &parse_maintenance_activity);
    MC_CHECK_EQ(std::string(to_string(MaintenanceActivity::FirmwareUpgrade)), std::string("firmware-upgrade"));
    MC_CHECK_EQ(std::string(to_string(MaintenanceActivity::DataWipe)), std::string("data-wipe"));

    const std::array<PlanPriority, 3> priorities{{PlanPriority::Routine, PlanPriority::Elevated,
                                                  PlanPriority::Emergency}};
    check_enum_round_trip(priorities, &parse_plan_priority);
    MC_CHECK_EQ(std::string(to_string(PlanPriority::Routine)), std::string("routine"));
    MC_CHECK_EQ(std::string(to_string(PlanPriority::Emergency)), std::string("emergency"));

    const std::array<ServiceRiskClass, 4> risks{{ServiceRiskClass::Low, ServiceRiskClass::Moderate,
                                                 ServiceRiskClass::High, ServiceRiskClass::Critical}};
    check_enum_round_trip(risks, &parse_service_risk_class);
    MC_CHECK_EQ(std::string(to_string(ServiceRiskClass::Critical)), std::string("critical"));
}

MC_TEST(lifecycle_state_text_round_trip_and_serviceability) {
    const std::array<LifecycleState, 9> states{{
        LifecycleState::Unknown,         LifecycleState::Commissioning, LifecycleState::InService,
        LifecycleState::Maintenance,     LifecycleState::Draining,      LifecycleState::Drained,
        LifecycleState::Decommissioning, LifecycleState::Decommissioned, LifecycleState::Failed,
    }};
    check_enum_round_trip(states, &parse_lifecycle_state);
    MC_CHECK_EQ(std::string(to_string(LifecycleState::Unknown)), std::string("unknown"));
    MC_CHECK_EQ(std::string(to_string(LifecycleState::InService)), std::string("in-service"));
    MC_CHECK_EQ(std::string(to_string(LifecycleState::Decommissioning)), std::string("decommissioning"));

    MC_CHECK(!is_lifecycle_serviceable(LifecycleState::Unknown));
    MC_CHECK(is_lifecycle_serviceable(LifecycleState::Commissioning));
    MC_CHECK(is_lifecycle_serviceable(LifecycleState::InService));
    MC_CHECK(is_lifecycle_serviceable(LifecycleState::Maintenance));
    MC_CHECK(is_lifecycle_serviceable(LifecycleState::Draining));
    MC_CHECK(is_lifecycle_serviceable(LifecycleState::Drained));
    MC_CHECK(is_lifecycle_serviceable(LifecycleState::Decommissioning));
    MC_CHECK(!is_lifecycle_serviceable(LifecycleState::Decommissioned));
    MC_CHECK(!is_lifecycle_serviceable(LifecycleState::Failed));
    MC_CHECK(!is_lifecycle_serviceable(static_cast<LifecycleState>(200U)));
    MC_CHECK_EQ(std::string(to_string(static_cast<LifecycleState>(200U))), std::string("unknown"));
}

MC_TEST(severity_service_class_and_redundancy_text_round_trip) {
    const std::array<Severity, 4> severities{{Severity::Low, Severity::Moderate, Severity::High,
                                              Severity::Critical}};
    check_enum_round_trip(severities, &parse_severity);
    MC_CHECK_EQ(std::string(to_string(Severity::Moderate)), std::string("moderate"));

    const std::array<ServiceClass, 4> classes{{ServiceClass::BestEffort, ServiceClass::Standard,
                                               ServiceClass::BusinessCritical, ServiceClass::MissionCritical}};
    check_enum_round_trip(classes, &parse_service_class);
    MC_CHECK_EQ(std::string(to_string(ServiceClass::BusinessCritical)), std::string("business-critical"));
    MC_CHECK_EQ(std::string(to_string(ServiceClass::MissionCritical)), std::string("mission-critical"));

    const std::array<RedundancyMode, 5> modes{{RedundancyMode::None, RedundancyMode::N, RedundancyMode::NPlusOne,
                                               RedundancyMode::NPlusTwo, RedundancyMode::TwoN}};
    check_enum_round_trip(modes, &parse_redundancy_mode);
    MC_CHECK_EQ(std::string(to_string(RedundancyMode::None)), std::string("none"));
    MC_CHECK_EQ(std::string(to_string(RedundancyMode::N)), std::string("n"));
    MC_CHECK_EQ(std::string(to_string(RedundancyMode::NPlusOne)), std::string("n+1"));
    MC_CHECK_EQ(std::string(to_string(RedundancyMode::NPlusTwo)), std::string("n+2"));
    MC_CHECK_EQ(std::string(to_string(RedundancyMode::TwoN)), std::string("2n"));
}

MC_TEST(obligation_enum_text_round_trips) {
    const std::array<ObligationKind, 16> kinds{{
        ObligationKind::AsiDrain,          ObligationKind::AsiQuiesce,       ObligationKind::DfiDrainTraffic,
        ObligationKind::DfiIsolatePath,    ObligationKind::PowerIsolate,     ObligationKind::CoolingAdjust,
        ObligationKind::LifecycleTransition, ObligationKind::PersonnelOnSite, ObligationKind::ApprovalWitness,
        ObligationKind::MaintenanceVerified, ObligationKind::WorkStopConfirmed, ObligationKind::DrainRelease,
        ObligationKind::PowerRestore,      ObligationKind::CoolingRestore,   ObligationKind::TrafficRestore,
        ObligationKind::RedundancyRestore,
    }};
    check_enum_round_trip(kinds, &parse_obligation_kind);
    MC_CHECK_EQ(std::string(to_string(ObligationKind::AsiDrain)), std::string("asi-drain"));
    MC_CHECK_EQ(std::string(to_string(ObligationKind::DfiIsolatePath)), std::string("dfi-isolate-path"));
    MC_CHECK_EQ(std::string(to_string(ObligationKind::WorkStopConfirmed)), std::string("work-stop-confirmed"));

    const std::array<ObligationAuthority, 5> authorities{{ObligationAuthority::Asi, ObligationAuthority::Dfi,
                                                          ObligationAuthority::DccpPlant,
                                                          ObligationAuthority::DccpInventory,
                                                          ObligationAuthority::Operator}};
    check_enum_round_trip(authorities, &parse_obligation_authority);
    MC_CHECK_EQ(std::string(to_string(ObligationAuthority::DccpPlant)), std::string("dccp-plant"));
    MC_CHECK_EQ(std::string(to_string(ObligationAuthority::DccpInventory)), std::string("dccp-inventory"));

    const std::array<ReceiptKind, 19> receipts{{
        ReceiptKind::DrainRequested,       ReceiptKind::DrainAcknowledged,  ReceiptKind::DrainObserved,
        ReceiptKind::QuiesceObserved,      ReceiptKind::TrafficDrainObserved, ReceiptKind::PathIsolationObserved,
        ReceiptKind::PowerIsolationObserved, ReceiptKind::CoolingAdjustmentObserved,
        ReceiptKind::LifecycleTransitionObserved, ReceiptKind::PersonnelOnSiteObserved,
        ReceiptKind::ApprovalWitnessObserved, ReceiptKind::MaintenanceVerifiedObserved,
        ReceiptKind::WorkStopObserved,     ReceiptKind::DrainReleaseObserved, ReceiptKind::PowerRestoreObserved,
        ReceiptKind::CoolingRestoreObserved, ReceiptKind::TrafficRestoreObserved,
        ReceiptKind::RedundancyRestoreObserved, ReceiptKind::FailureObserved,
    }};
    check_enum_round_trip(receipts, &parse_receipt_kind);
    MC_CHECK_EQ(std::string(to_string(ReceiptKind::DrainRequested)), std::string("drain-requested"));
    MC_CHECK_EQ(std::string(to_string(ReceiptKind::FailureObserved)), std::string("failure-observed"));

    // ObligationStage and Disposition have no parser, but their spellings are
    // still stable and unique.
    const std::array<ObligationStage, 3> stages{{ObligationStage::PreDrain, ObligationStage::Isolation,
                                                 ObligationStage::Completion}};
    for (std::size_t index = 0; index < stages.size(); ++index) {
        MC_CHECK(!to_string(stages[index]).empty());
        for (std::size_t other = index + 1U; other < stages.size(); ++other) {
            MC_CHECK(to_string(stages[other]) != to_string(stages[index]));
        }
    }
    MC_CHECK_EQ(std::string(to_string(ObligationStage::PreDrain)), std::string("pre-drain"));
    MC_CHECK_EQ(std::string(to_string(ObligationStage::Isolation)), std::string("isolation"));
    MC_CHECK_EQ(std::string(to_string(ObligationStage::Completion)), std::string("completion"));

    const std::array<Disposition, 6> dispositions{{
        Disposition::Outstanding, Disposition::Acknowledged, Disposition::Satisfied,
        Disposition::Failed, Disposition::Waived, Disposition::NotApplicable,
    }};
    for (std::size_t index = 0; index < dispositions.size(); ++index) {
        MC_CHECK(!to_string(dispositions[index]).empty());
        for (std::size_t other = index + 1U; other < dispositions.size(); ++other) {
            MC_CHECK(to_string(dispositions[other]) != to_string(dispositions[index]));
        }
    }
    MC_CHECK_EQ(std::string(to_string(Disposition::NotApplicable)), std::string("not-applicable"));

    // Observations, requests and acknowledgements are disjoint and cover the
    // whole receipt surface.
    for (const ReceiptKind kind : receipts) {
        MC_CHECK(!(is_observation(kind) && is_request_or_acknowledgement(kind)));
    }
    MC_CHECK(is_request_or_acknowledgement(ReceiptKind::DrainRequested));
    MC_CHECK(is_request_or_acknowledgement(ReceiptKind::DrainAcknowledged));
    MC_CHECK(!is_observation(ReceiptKind::DrainRequested));
    MC_CHECK(!is_observation(ReceiptKind::DrainAcknowledged));
    MC_CHECK(!is_observation(ReceiptKind::FailureObserved));
    MC_CHECK(!is_request_or_acknowledgement(ReceiptKind::FailureObserved));
    MC_CHECK(is_observation(ReceiptKind::DrainObserved));
    MC_CHECK(!is_request_or_acknowledgement(ReceiptKind::DrainObserved));
}

MC_TEST(plan_phase_and_progress_kind_text_round_trip) {
    const std::array<PlanPhase, 12> phases{{
        PlanPhase::Proposed,   PlanPhase::Evaluating, PlanPhase::Approved,  PlanPhase::PreDrain,
        PlanPhase::Ready,      PlanPhase::InProgress,  PlanPhase::Verification, PlanPhase::Restore,
        PlanPhase::Complete,   PlanPhase::Blocked,    PlanPhase::Cancelled, PlanPhase::Failed,
    }};
    check_enum_round_trip(phases, &parse_plan_phase);
    MC_CHECK_EQ(std::string(to_string(PlanPhase::PreDrain)), std::string("pre-drain"));
    MC_CHECK_EQ(std::string(to_string(PlanPhase::InProgress)), std::string("in-progress"));
    MC_CHECK_EQ(std::string(to_string(PlanPhase::Cancelled)), std::string("cancelled"));

    MC_CHECK(is_terminal(PlanPhase::Complete));
    MC_CHECK(is_terminal(PlanPhase::Cancelled));
    MC_CHECK(is_terminal(PlanPhase::Failed));
    MC_CHECK(!is_terminal(PlanPhase::Proposed));
    MC_CHECK(!is_terminal(PlanPhase::Restore));
    MC_CHECK(is_started(PlanPhase::InProgress));
    MC_CHECK(is_started(PlanPhase::Verification));
    MC_CHECK(is_started(PlanPhase::Restore));
    MC_CHECK(!is_started(PlanPhase::Approved));
    MC_CHECK(!is_started(PlanPhase::Complete));

    std::uint8_t previous = 0U;
    for (std::size_t index = 0; index < 9U; ++index) {
        const std::uint8_t order = phase_order(phases[index]);
        MC_CHECK_MSG(order >= previous, "phase_order must not decrease along the canonical order");
        previous = order;
    }
    MC_CHECK_EQ(static_cast<unsigned>(phase_order(PlanPhase::Proposed)), 0U);
    MC_CHECK_EQ(static_cast<unsigned>(phase_order(PlanPhase::Complete)), 8U);
    MC_CHECK_EQ(static_cast<unsigned>(phase_order(PlanPhase::Blocked)), 9U);
    MC_CHECK_EQ(static_cast<unsigned>(phase_order(PlanPhase::Cancelled)), 9U);
    MC_CHECK_EQ(static_cast<unsigned>(phase_order(PlanPhase::Failed)), 9U);

    const std::array<ProgressKind, 5> progress{{ProgressKind::Started, ProgressKind::StepCompleted,
                                                ProgressKind::WorkCompleted, ProgressKind::WorkStopped,
                                                ProgressKind::IssueObserved}};
    check_enum_round_trip(progress, &parse_progress_kind);
    MC_CHECK_EQ(std::string(to_string(ProgressKind::StepCompleted)), std::string("step-completed"));
    MC_CHECK_EQ(std::string(to_string(ProgressKind::WorkStopped)), std::string("work-stopped"));
    MC_CHECK_EQ(std::string(to_string(ProgressKind::IssueObserved)), std::string("issue-observed"));
}

MC_TEST(obligation_mapping_tables) {
    const std::array<ObligationKind, 16> kinds{{
        ObligationKind::AsiDrain,          ObligationKind::AsiQuiesce,       ObligationKind::DfiDrainTraffic,
        ObligationKind::DfiIsolatePath,    ObligationKind::PowerIsolate,     ObligationKind::CoolingAdjust,
        ObligationKind::LifecycleTransition, ObligationKind::PersonnelOnSite, ObligationKind::ApprovalWitness,
        ObligationKind::MaintenanceVerified, ObligationKind::WorkStopConfirmed, ObligationKind::DrainRelease,
        ObligationKind::PowerRestore,      ObligationKind::CoolingRestore,   ObligationKind::TrafficRestore,
        ObligationKind::RedundancyRestore,
    }};

    std::size_t index = 0U;
    for (const ObligationKind kind : kinds) {
        // The authority the derived obligation is placed on is authority_of(),
        // and Obligation::validate() enforces exactly that pair.
        const Obligation obligation =
            make_obligation("ob-" + std::to_string(index), kind, target_of(TargetKind::Rack, "rack-a1"));
        MC_CHECK(obligation.authority == authority_of(kind));
        MC_CHECK_MSG(obligation.validate().ok(), "derived obligation for " + std::string(to_string(kind)) +
                                                     " did not validate: " + obligation.validate().render());

        // The stage mapping is a pure function of the kind.
        MC_CHECK(stage_of(kind) == stage_of(kind));

        // Satisfying evidence is always an observation of effect, never a
        // request or an acknowledgement.
        const ReceiptKind receipt = satisfying_receipt(kind);
        MC_CHECK_MSG(is_observation(receipt), std::string(to_string(kind)) + " is satisfied by a non observation");
        MC_CHECK_MSG(!is_request_or_acknowledgement(receipt),
                     std::string(to_string(kind)) + " is satisfied by a request or acknowledgement");
        ++index;
    }

    // Spot check the table itself, so a silent reordering is caught.
    MC_CHECK(authority_of(ObligationKind::AsiDrain) == ObligationAuthority::Asi);
    MC_CHECK(authority_of(ObligationKind::AsiQuiesce) == ObligationAuthority::Asi);
    MC_CHECK(authority_of(ObligationKind::DfiDrainTraffic) == ObligationAuthority::Dfi);
    MC_CHECK(authority_of(ObligationKind::DfiIsolatePath) == ObligationAuthority::Dfi);
    MC_CHECK(authority_of(ObligationKind::PowerIsolate) == ObligationAuthority::DccpPlant);
    MC_CHECK(authority_of(ObligationKind::CoolingAdjust) == ObligationAuthority::DccpPlant);
    MC_CHECK(authority_of(ObligationKind::LifecycleTransition) == ObligationAuthority::DccpInventory);
    MC_CHECK(authority_of(ObligationKind::PersonnelOnSite) == ObligationAuthority::Operator);
    MC_CHECK(authority_of(ObligationKind::ApprovalWitness) == ObligationAuthority::Operator);
    MC_CHECK(authority_of(ObligationKind::RedundancyRestore) == ObligationAuthority::DccpPlant);
    MC_CHECK(stage_of(ObligationKind::AsiDrain) == ObligationStage::PreDrain);
    MC_CHECK(stage_of(ObligationKind::ApprovalWitness) == ObligationStage::PreDrain);
    MC_CHECK(stage_of(ObligationKind::PowerIsolate) == ObligationStage::Isolation);
    MC_CHECK(stage_of(ObligationKind::MaintenanceVerified) == ObligationStage::Completion);
    MC_CHECK(satisfying_receipt(ObligationKind::AsiDrain) == ReceiptKind::DrainObserved);
    MC_CHECK(satisfying_receipt(ObligationKind::RedundancyRestore) == ReceiptKind::RedundancyRestoreObserved);

    // Satisfying receipts are distinct across kinds.
    for (std::size_t left = 0; left < kinds.size(); ++left) {
        for (std::size_t right = left + 1U; right < kinds.size(); ++right) {
            MC_CHECK(satisfying_receipt(kinds[left]) != satisfying_receipt(kinds[right]));
        }
    }

    // A set holding every kind canonicalizes group by stage: the canonical
    // order is (stage, kind, target, id), so stage must never decrease.
    ObligationSet set;
    for (const ObligationKind kind : kinds) {
        set.obligations.push_back(make_obligation(std::string("ob-") + std::string(to_string(kind)), kind,
                                                  target_of(TargetKind::Rack, "rack-a1")));
    }
    set.canonicalize();
    MC_CHECK_EQ(set.obligations.size(), 16U);
    for (std::size_t position = 1; position < set.obligations.size(); ++position) {
        MC_CHECK(static_cast<std::uint8_t>(stage_of(set.obligations[position - 1U].kind)) <=
                 static_cast<std::uint8_t>(stage_of(set.obligations[position].kind)));
    }
    set.finalize();
    MC_REQUIRE_OK(set.validate());
    MC_CHECK(!set.digest.is_zero());
    MC_CHECK(set.find(id_of<ObligationId>("ob-asi-drain")) != nullptr);
    MC_CHECK(set.find(id_of<ObligationId>("ob-not-a-kind")) == nullptr);
    MC_CHECK(!set.empty());

    // An obligation placed on the wrong authority is an identity mismatch.
    Obligation wrong = make_obligation("ob-wrong", ObligationKind::AsiDrain, target_of(TargetKind::Rack, "rack-a1"));
    wrong.authority = ObligationAuthority::Dfi;
    MC_CHECK_CODE(wrong.validate(), ErrorCode::IdentityMismatch);
    Obligation no_target = make_obligation("ob-no-target", ObligationKind::AsiDrain, TargetRef{});
    MC_CHECK_CODE(no_target.validate(), ErrorCode::InvalidArgument);
    Obligation no_requirement = make_obligation("ob-no-requirement", ObligationKind::AsiDrain,
                                                target_of(TargetKind::Rack, "rack-a1"));
    no_requirement.requirement.clear();
    MC_CHECK_CODE(no_requirement.validate(), ErrorCode::InvalidArgument);
    MC_CHECK(wrong.compute_digest() != no_target.compute_digest());
}

// ---------------------------------------------------------------------------
// Facility snapshot
// ---------------------------------------------------------------------------

MC_TEST(facility_snapshot_validates_and_digest_is_order_independent) {
    const FacilitySnapshot canonical = make_facility();
    MC_REQUIRE_OK(canonical.validate());
    MC_CHECK(!canonical.digest.is_zero());
    MC_CHECK(!canonical.dependency_digest.is_zero());
    MC_CHECK(canonical.digest != canonical.dependency_digest);
    MC_CHECK_EQ(canonical.assets.size(), 4U);
    MC_CHECK_EQ(canonical.redundancy_groups.size(), 2U);
    MC_CHECK_EQ(names_of(canonical.sites()), (std::vector<std::string>{"site-a", "site-b"}));

    // Every vector reversed, then canonicalized: byte identical digests.
    FacilitySnapshot shuffled = permuted(canonical);
    shuffled.finalize();
    MC_REQUIRE_OK(shuffled.validate());
    MC_CHECK(shuffled.digest == canonical.digest);
    MC_CHECK(shuffled.dependency_digest == canonical.dependency_digest);
    MC_CHECK_EQ(names_of(shuffled.sites()), names_of(canonical.sites()));

    // finalize() is idempotent, and the digests are recomputed from the content.
    shuffled.finalize();
    MC_CHECK(shuffled.digest == canonical.digest);
    MC_REQUIRE_OK(canonical.validate());

    // Lookups work on the canonical order.
    MC_REQUIRE(canonical.find_asset(id_of<AssetId>("asset-a1")) != nullptr);
    MC_CHECK_EQ(canonical.find_asset(id_of<AssetId>("asset-a1"))->rack.name(), std::string("rack-a1"));
    MC_CHECK(canonical.find_asset(id_of<AssetId>("asset-nope")) == nullptr);
    MC_REQUIRE(canonical.find_redundancy_group(id_of<RedundancyGroupId>("rg-a")) != nullptr);
    MC_CHECK(canonical.find_redundancy_group(id_of<RedundancyGroupId>("rg-z")) == nullptr);
    MC_REQUIRE(canonical.find_capacity_pool(id_of<CapacityPoolId>("pool-a")) != nullptr);
    MC_CHECK(canonical.find_capacity_pool(id_of<CapacityPoolId>("pool-z")) == nullptr);
    MC_REQUIRE(canonical.find_power_domain(id_of<PowerDomainId>("pd-a")) != nullptr);
    MC_CHECK(canonical.find_power_domain(id_of<PowerDomainId>("pd-z")) == nullptr);
    MC_REQUIRE(canonical.find_cooling_zone(id_of<CoolingZoneId>("cz-a")) != nullptr);
    MC_CHECK(canonical.find_cooling_zone(id_of<CoolingZoneId>("cz-z")) == nullptr);
    MC_REQUIRE(canonical.find_fabric_segment(id_of<FabricSegmentId>("fs-a")) != nullptr);
    MC_CHECK(canonical.find_fabric_segment(id_of<FabricSegmentId>("fs-z")) == nullptr);

    // knows_target() resolves every kind through the model.
    MC_CHECK(canonical.knows_target(target_of(TargetKind::Asset, "asset-a1")));
    MC_CHECK(!canonical.knows_target(target_of(TargetKind::Asset, "asset-nope")));
    MC_CHECK(canonical.knows_target(target_of(TargetKind::Rack, "rack-a1")));
    MC_CHECK(!canonical.knows_target(target_of(TargetKind::Rack, "rack-nope")));
    MC_CHECK(canonical.knows_target(target_of(TargetKind::Site, "site-a")));
    MC_CHECK(!canonical.knows_target(target_of(TargetKind::Site, "site-nope")));
    MC_CHECK(canonical.knows_target(target_of(TargetKind::PowerDomain, "pd-a")));
    MC_CHECK(!canonical.knows_target(target_of(TargetKind::PowerDomain, "pd-nope")));
    MC_CHECK(canonical.knows_target(target_of(TargetKind::CoolingZone, "cz-a")));
    MC_CHECK(canonical.knows_target(target_of(TargetKind::FabricSegment, "fs-a")));

    // The whole digest covers the whole snapshot; the dependency digest covers
    // the dependency projection (assets, groups, pools, fabric, protections,
    // incidents, blackouts) and not the power or cooling plant.
    FacilitySnapshot changed = canonical;
    changed.assets[0].healthy = false;
    changed.finalize();
    MC_CHECK(changed.digest != canonical.digest);
    MC_CHECK(changed.dependency_digest != canonical.dependency_digest);

    FacilitySnapshot cooled = canonical;
    MC_REQUIRE(!cooled.cooling_zones.empty());
    cooled.cooling_zones[0].headroom.available_units += 5;
    cooled.finalize();
    MC_CHECK(cooled.digest != canonical.digest);
    MC_CHECK(cooled.dependency_digest == canonical.dependency_digest);

    FacilitySnapshot regened = canonical;
    regened.capacity_generation = CapacityGeneration::from_value(99U);
    regened.finalize();
    MC_CHECK(regened.digest != canonical.digest);
    MC_CHECK(regened.dependency_digest != canonical.dependency_digest);

    // A snapshot with no generation is never silently generation zero.
    FacilitySnapshot unset = canonical;
    unset.maintenance_generation = MaintenanceGeneration{};
    MC_CHECK_CODE(unset.validate_generations(), ErrorCode::MissingGeneration);
    MC_CHECK_CODE(unset.validate(), ErrorCode::MissingGeneration);
}

MC_TEST(facility_snapshot_validate_rejects_structural_faults) {
    {
        FacilitySnapshot snapshot = make_facility();
        snapshot.assets.push_back(snapshot.assets.front());
        check_snapshot_rejected(snapshot, ErrorCode::DuplicateIdentity, "duplicate asset identity");
    }
    {
        FacilitySnapshot snapshot = make_facility();
        snapshot.redundancy_groups.push_back(snapshot.redundancy_groups.front());
        check_snapshot_rejected(snapshot, ErrorCode::DuplicateIdentity, "duplicate redundancy group identity");
    }
    {
        FacilitySnapshot snapshot = make_facility();
        snapshot.incidents.push_back(snapshot.incidents.front());
        check_snapshot_rejected(snapshot, ErrorCode::DuplicateIdentity, "duplicate incident identity");
    }
    {
        FacilitySnapshot snapshot = make_facility();
        snapshot.assets.push_back(make_asset("asset-zz", "rack-a1", "site-b", 1U));
        check_snapshot_rejected(snapshot, ErrorCode::IdentityMismatch, "one rack in two different sites");
    }
    {
        FacilitySnapshot snapshot = make_facility();
        RedundancyGroup* group = find_by_id(snapshot.redundancy_groups, "rg-a");
        MC_REQUIRE(group != nullptr);
        group->members.push_back(id_of<AssetId>("asset-missing"));
        check_snapshot_rejected(snapshot, ErrorCode::UnknownAsset, "redundancy group with an unknown member");
    }
    {
        IncidentRecord incident;
        incident.id = id_of<IncidentId>("inc-unknown");
        incident.targets = {target_of(TargetKind::Asset, "asset-nope")};
        incident.severity = Severity::High;
        incident.active = true;
        incident.summary = "unknown target";
        FacilitySnapshot snapshot = make_facility();
        snapshot.incidents.push_back(incident);
        check_snapshot_rejected(snapshot, ErrorCode::UnknownTarget, "incident with an unknown target");
    }
    {
        BlackoutPeriod blackout;
        blackout.id = id_of<BlackoutId>("bo-unknown");
        blackout.targets = {target_of(TargetKind::Rack, "rack-nope")};
        blackout.start = kT0;
        blackout.end = kT0 + kNanosPerHour;
        FacilitySnapshot snapshot = make_facility();
        snapshot.blackouts.push_back(blackout);
        check_snapshot_rejected(snapshot, ErrorCode::UnknownTarget, "blackout with an unknown target");
    }
    {
        FacilitySnapshot snapshot = make_facility();
        snapshot.facility_epoch = FacilityEpoch{};
        check_snapshot_rejected(snapshot, ErrorCode::MissingGeneration, "snapshot without a facility epoch");
    }
    {
        FacilitySnapshot snapshot = make_facility();
        MC_REQUIRE(!snapshot.assets.empty());
        snapshot.assets[0].lifecycle_generation = LifecycleGeneration{};
        check_snapshot_rejected(snapshot, ErrorCode::MissingGeneration, "asset without a lifecycle generation");
    }
    {
        FacilitySnapshot snapshot = make_facility();
        snapshot.observed_at = kNoTimestamp;
        check_snapshot_rejected(snapshot, ErrorCode::InvalidArgument, "snapshot without an observation time");
    }
    {
        // Checked without finalize(), which would recompute the digest.
        FacilitySnapshot snapshot = make_facility();
        snapshot.digest = Digest{};
        MC_CHECK_CODE(snapshot.validate(), ErrorCode::InvalidArgument);
        FacilitySnapshot dependency = make_facility();
        dependency.dependency_digest = Digest{};
        MC_CHECK_CODE(dependency.validate(), ErrorCode::InvalidArgument);
    }
    {
        BlackoutPeriod blackout;
        blackout.id = id_of<BlackoutId>("bo-backwards");
        blackout.start = kT0 + kNanosPerHour;
        blackout.end = kT0;
        FacilitySnapshot snapshot = make_facility();
        snapshot.blackouts.push_back(blackout);
        check_snapshot_rejected(snapshot, ErrorCode::InvalidArgument, "blackout that ends before it starts");
    }
    {
        BlackoutPeriod blackout;
        blackout.id = id_of<BlackoutId>("bo-empty");
        blackout.start = kT0;
        blackout.end = kT0;
        FacilitySnapshot snapshot = make_facility();
        snapshot.blackouts.push_back(blackout);
        check_snapshot_rejected(snapshot, ErrorCode::InvalidArgument, "blackout with a zero length window");
    }
    {
        FacilitySnapshot snapshot = make_facility();
        ProtectedObligation* protection = find_by_id(snapshot.protected_obligations, "po-a");
        MC_REQUIRE(protection != nullptr);
        protection->required_units = 0U;
        check_snapshot_rejected(snapshot, ErrorCode::InvalidArgument, "protection that protects nothing");
    }
    {
        FacilitySnapshot snapshot = make_facility();
        ProtectedObligation* protection = find_by_id(snapshot.protected_obligations, "po-a");
        MC_REQUIRE(protection != nullptr);
        protection->targets.clear();
        check_snapshot_rejected(snapshot, ErrorCode::InvalidArgument, "protection with no target");
    }
    {
        FacilitySnapshot snapshot = make_facility();
        IncidentRecord incident;
        incident.id = id_of<IncidentId>("inc-empty");
        incident.summary = "no target";
        snapshot.incidents.push_back(incident);
        check_snapshot_rejected(snapshot, ErrorCode::InvalidArgument, "incident with no target");
    }
    {
        FacilitySnapshot snapshot = make_facility();
        snapshot.power_domains[0].interlocked = true;
        check_snapshot_rejected(snapshot, ErrorCode::InvalidArgument, "power interlock with no reason");
    }
    {
        FacilitySnapshot snapshot = make_facility();
        snapshot.power_domains[0].interlocked = true;
        snapshot.power_domains[0].interlock_reason = "arc flash risk";
        snapshot.finalize();
        MC_REQUIRE_OK(snapshot.validate());
    }
    {
        FacilitySnapshot snapshot = make_facility();
        snapshot.policy.generation = PolicyGeneration{};
        check_snapshot_rejected(snapshot, ErrorCode::MissingGeneration, "policy without a generation");
    }
    {
        FacilitySnapshot snapshot = make_facility();
        snapshot.assets.push_back(make_asset("asset-empty-site", "rack-a1", "site-a", 1U));
        MC_REQUIRE(snapshot.assets.size() == 5U);
        snapshot.assets.back().site = SiteId{};
        check_snapshot_rejected(snapshot, ErrorCode::UnknownTarget, "asset with no parent");
    }
}

MC_TEST(facility_snapshot_validate_reports_the_lowest_code) {
    // IdentityMismatch (204) outranks UnknownAsset (207) and MissingGeneration (311).
    {
        FacilitySnapshot snapshot = make_facility();
        snapshot.assets.push_back(make_asset("asset-zz", "rack-a1", "site-b", 1U));
        RedundancyGroup* group = find_by_id(snapshot.redundancy_groups, "rg-a");
        MC_REQUIRE(group != nullptr);
        group->members.push_back(id_of<AssetId>("asset-missing"));
        snapshot.topology_generation = TopologyGeneration{};
        check_snapshot_rejected(snapshot, ErrorCode::IdentityMismatch, "mismatch with unknown member and generation");
    }
    // DuplicateIdentity (203) outranks IdentityMismatch (204) and UnknownAsset (207).
    {
        FacilitySnapshot snapshot = make_facility();
        snapshot.assets.push_back(snapshot.assets.front());
        snapshot.assets.push_back(make_asset("asset-zz", "rack-a1", "site-b", 1U));
        RedundancyGroup* group = find_by_id(snapshot.redundancy_groups, "rg-a");
        MC_REQUIRE(group != nullptr);
        group->members.push_back(id_of<AssetId>("asset-missing"));
        check_snapshot_rejected(snapshot, ErrorCode::DuplicateIdentity, "duplicate with mismatch and unknown member");
    }
    // InvalidArgument (103) outranks MissingGeneration (311) and UnknownAsset (207).
    {
        FacilitySnapshot snapshot = make_facility();
        BlackoutPeriod blackout;
        blackout.id = id_of<BlackoutId>("bo-backwards");
        blackout.start = kT0 + kNanosPerHour;
        blackout.end = kT0;
        snapshot.blackouts.push_back(blackout);
        snapshot.policy_generation = PolicyGeneration{};
        RedundancyGroup* group = find_by_id(snapshot.redundancy_groups, "rg-a");
        MC_REQUIRE(group != nullptr);
        group->members.push_back(id_of<AssetId>("asset-missing"));
        check_snapshot_rejected(snapshot, ErrorCode::InvalidArgument, "invalid argument outranks the rest");
    }
    // UnknownTarget (201) outranks DuplicateIdentity (203) and MissingGeneration (311).
    {
        FacilitySnapshot snapshot = make_facility();
        IncidentRecord incident;
        incident.id = id_of<IncidentId>("inc-unknown");
        incident.targets = {target_of(TargetKind::Asset, "asset-nope")};
        incident.summary = "unknown target";
        snapshot.incidents.push_back(incident);
        snapshot.assets.push_back(snapshot.assets.front());
        snapshot.policy_generation = PolicyGeneration{};
        check_snapshot_rejected(snapshot, ErrorCode::UnknownTarget, "unknown target outranks duplicates");

        // The outranked faults are still recorded for evidence.
        snapshot.finalize();
        const Status status = snapshot.validate();
        MC_CHECK_EQ(status.code(), ErrorCode::UnknownTarget);
        MC_CHECK(status.suppressed().size() >= 2U);
        bool saw_duplicate = false;
        bool saw_generation = false;
        for (const Status& extra : status.suppressed()) {
            saw_duplicate = saw_duplicate || extra.code() == ErrorCode::DuplicateIdentity;
            saw_generation = saw_generation || extra.code() == ErrorCode::MissingGeneration;
        }
        MC_CHECK(saw_duplicate);
        MC_CHECK(saw_generation);
    }
}

// ---------------------------------------------------------------------------
// Scope resolution against the snapshot
// ---------------------------------------------------------------------------

MC_TEST(scope_resolution_assets_groups_and_conflicts) {
    const FacilitySnapshot snapshot = make_facility();

    const MaintenanceScope asset_scope({target_of(TargetKind::Asset, "asset-a1")});
    const MaintenanceScope rack_scope({target_of(TargetKind::Rack, "rack-a1")});
    const MaintenanceScope other_rack_scope({target_of(TargetKind::Rack, "rack-a2")});
    const MaintenanceScope site_scope({target_of(TargetKind::Site, "site-a")});
    const MaintenanceScope site_b_scope({target_of(TargetKind::Site, "site-b")});
    const MaintenanceScope domain_scope({target_of(TargetKind::PowerDomain, "pd-a")});
    const MaintenanceScope empty_scope;

    MC_CHECK_EQ(names_of(snapshot.assets_in_scope(asset_scope)), (std::vector<std::string>{"asset-a1"}));
    MC_CHECK_EQ(names_of(snapshot.assets_in_scope(rack_scope)), (std::vector<std::string>{"asset-a1", "asset-a2"}));
    MC_CHECK_EQ(names_of(snapshot.assets_in_scope(site_scope)),
                (std::vector<std::string>{"asset-a1", "asset-a2", "asset-a3"}));
    MC_CHECK_EQ(names_of(snapshot.assets_in_scope(site_b_scope)), (std::vector<std::string>{"asset-b1"}));
    MC_CHECK_EQ(names_of(snapshot.assets_in_scope(other_rack_scope)), (std::vector<std::string>{"asset-a3"}));
    MC_CHECK(snapshot.assets_in_scope(domain_scope).empty());
    MC_CHECK(snapshot.assets_in_scope(empty_scope).empty());
    // A rack name that does not exist selects nothing rather than everything.
    MC_CHECK(snapshot.assets_in_scope(MaintenanceScope({target_of(TargetKind::Rack, "rack-nope")})).empty());

    MC_CHECK_EQ(names_of(snapshot.redundancy_groups_in_scope(asset_scope)),
                (std::vector<std::string>{"rg-a"}));
    MC_CHECK_EQ(names_of(snapshot.redundancy_groups_in_scope(site_scope)),
                (std::vector<std::string>{"rg-a"}));
    MC_CHECK_EQ(names_of(snapshot.redundancy_groups_in_scope(site_b_scope)),
                (std::vector<std::string>{"rg-b"}));
    MC_CHECK(snapshot.redundancy_groups_in_scope(other_rack_scope).empty());
    MC_CHECK(snapshot.redundancy_groups_in_scope(empty_scope).empty());

    // The site of a scoped asset is itself in scope, so the pools, domains,
    // zones and segments that belong to that site are touched by the scope.
    MC_CHECK_EQ(names_of(snapshot.sites_in_scope(asset_scope)), (std::vector<std::string>{"site-a"}));
    MC_CHECK_EQ(names_of(snapshot.sites_in_scope(rack_scope)), (std::vector<std::string>{"site-a"}));
    MC_CHECK_EQ(names_of(snapshot.sites_in_scope(site_scope)), (std::vector<std::string>{"site-a"}));
    MC_CHECK_EQ(names_of(snapshot.sites_in_scope(site_b_scope)), (std::vector<std::string>{"site-b"}));
    MC_CHECK(snapshot.sites_in_scope(other_rack_scope).size() == 1U);
    MC_CHECK(snapshot.sites_in_scope(empty_scope).empty());
    MC_CHECK(snapshot.sites_in_scope(MaintenanceScope({target_of(TargetKind::Rack, "rack-nope")})).empty());

    MC_CHECK_EQ(names_of(snapshot.capacity_pools_in_scope(site_scope)),
                (std::vector<std::string>{"pool-a"}));
    MC_CHECK_EQ(names_of(snapshot.capacity_pools_in_scope(asset_scope)),
                (std::vector<std::string>{"pool-a"}));
    MC_CHECK_EQ(names_of(snapshot.capacity_pools_in_scope(site_b_scope)),
                (std::vector<std::string>{"pool-b"}));
    MC_CHECK(snapshot.capacity_pools_in_scope(empty_scope).empty());
    MC_CHECK_EQ(names_of(snapshot.power_domains_in_scope(site_scope)), (std::vector<std::string>{"pd-a"}));
    MC_CHECK_EQ(names_of(snapshot.power_domains_in_scope(asset_scope)), (std::vector<std::string>{"pd-a"}));
    MC_CHECK_EQ(names_of(snapshot.power_domains_in_scope(domain_scope)), (std::vector<std::string>{"pd-a"}));
    MC_CHECK_EQ(names_of(snapshot.power_domains_in_scope(site_b_scope)), (std::vector<std::string>{"pd-b"}));
    MC_CHECK(snapshot.power_domains_in_scope(empty_scope).empty());
    MC_CHECK_EQ(names_of(snapshot.cooling_zones_in_scope(site_scope)), (std::vector<std::string>{"cz-a"}));
    MC_CHECK_EQ(names_of(snapshot.cooling_zones_in_scope(asset_scope)), (std::vector<std::string>{"cz-a"}));
    MC_CHECK(snapshot.cooling_zones_in_scope(site_b_scope).empty());
    MC_CHECK(snapshot.cooling_zones_in_scope(empty_scope).empty());
    MC_CHECK_EQ(names_of(snapshot.fabric_segments_in_scope(site_scope)), (std::vector<std::string>{"fs-a"}));
    MC_CHECK_EQ(names_of(snapshot.fabric_segments_in_scope(asset_scope)), (std::vector<std::string>{"fs-a"}));
    MC_CHECK(snapshot.fabric_segments_in_scope(site_b_scope).empty());
    MC_CHECK(snapshot.fabric_segments_in_scope(empty_scope).empty());

    // Conflict: sharing a target, or one scope containing the other.
    MC_CHECK(snapshot.scope_conflicts(rack_scope, asset_scope));
    MC_CHECK(snapshot.scope_conflicts(asset_scope, rack_scope));
    MC_CHECK(snapshot.scope_conflicts(asset_scope, asset_scope));
    MC_CHECK(snapshot.scope_conflicts(site_scope, rack_scope));
    MC_CHECK(snapshot.scope_conflicts(site_scope, asset_scope));
    MC_CHECK(snapshot.scope_conflicts(site_scope, site_scope));
    MC_CHECK(!snapshot.scope_conflicts(rack_scope, other_rack_scope));
    MC_CHECK(!snapshot.scope_conflicts(site_scope, site_b_scope));
    MC_CHECK(!snapshot.scope_conflicts(asset_scope, empty_scope));
    MC_CHECK(!snapshot.scope_conflicts(site_b_scope, asset_scope));
    MC_CHECK(!snapshot.scope_conflicts(domain_scope, site_scope));

    // Two disjoint sites never conflict, but a mixed scope that touches one of
    // them does.
    const MaintenanceScope mixed({target_of(TargetKind::Asset, "asset-b1"), target_of(TargetKind::Rack, "rack-a2")});
    MC_CHECK(!snapshot.scope_conflicts(site_scope, MaintenanceScope({target_of(TargetKind::Site, "site-b")})));
    MC_CHECK(snapshot.scope_conflicts(site_scope, mixed));
    MC_CHECK(snapshot.scope_conflicts(site_b_scope, mixed));
}

MC_TEST(scope_resolution_blackouts_incidents_and_protections) {
    const FacilitySnapshot snapshot = make_facility();
    const MaintenanceScope asset_scope({target_of(TargetKind::Asset, "asset-a1")});
    const MaintenanceScope asset_b_scope({target_of(TargetKind::Asset, "asset-b1")});
    const MaintenanceScope site_scope({target_of(TargetKind::Site, "site-a")});
    const MaintenanceScope rack_b_scope({target_of(TargetKind::Rack, "rack-b1")});

    const WindowSpec inside_rack_blackout{kT0 + 10LL * kNanosPerMinute, kT0 + 20LL * kNanosPerMinute};
    const WindowSpec inside_facility_blackout{kT0 + 2LL * kNanosPerHour + kNanosPerMinute,
                                              kT0 + 2LL * kNanosPerHour + 2LL * kNanosPerMinute};
    const WindowSpec outside_everything{kT0 + 10LL * kNanosPerDay, kT0 + 10LL * kNanosPerDay + kNanosPerHour};
    const WindowSpec touching_edge{kT0 + kNanosPerHour, kT0 + kNanosPerHour + kNanosPerMinute};

    // The rack blackout covers an asset inside that rack...
    MC_CHECK_EQ(names_of(snapshot.blackouts_covering(asset_scope, inside_rack_blackout)),
                (std::vector<std::string>{"bo-rack-a1"}));
    MC_CHECK_EQ(names_of(snapshot.blackouts_covering(asset_scope, inside_facility_blackout)),
                (std::vector<std::string>{"bo-facility"}));
    // ... including exactly at the end instant, because overlap is inclusive.
    MC_CHECK_EQ(names_of(snapshot.blackouts_covering(asset_scope, touching_edge)),
                (std::vector<std::string>{"bo-rack-a1"}));
    // A non covering scope is not matched by the rack blackout, and a window
    // outside every blackout window matches nothing.
    MC_CHECK(snapshot.blackouts_covering(asset_b_scope, inside_rack_blackout).empty());
    MC_CHECK_EQ(names_of(snapshot.blackouts_covering(asset_b_scope, inside_facility_blackout)),
                (std::vector<std::string>{"bo-facility"}));
    MC_CHECK(snapshot.blackouts_covering(site_scope, outside_everything).empty());
    MC_CHECK(snapshot.blackouts_covering(asset_scope, WindowSpec{}).empty());
    MC_CHECK(snapshot.blackouts_covering(asset_scope, WindowSpec{kT0, kNoTimestamp}).empty());

    // Incidents, filtered and unfiltered.
    MC_CHECK_EQ(names_of(snapshot.incidents_covering(asset_scope, true)), (std::vector<std::string>{"inc-active-a1"}));
    MC_CHECK_EQ(names_of(snapshot.incidents_covering(asset_scope, false)),
                (std::vector<std::string>{"inc-active-a1"}));
    MC_CHECK(snapshot.incidents_covering(rack_b_scope, true).empty());
    MC_CHECK_EQ(names_of(snapshot.incidents_covering(rack_b_scope, false)),
                (std::vector<std::string>{"inc-quiet-b1"}));
    MC_CHECK_EQ(names_of(snapshot.incidents_covering(site_scope, false)),
                (std::vector<std::string>{"inc-active-a1"}));
    MC_CHECK(snapshot.incidents_covering(MaintenanceScope{}, false).empty());

    // Protected obligations.
    MC_CHECK_EQ(names_of(snapshot.protections_covering(asset_scope)), (std::vector<std::string>{"po-a"}));
    MC_CHECK_EQ(names_of(snapshot.protections_covering(site_scope)), (std::vector<std::string>{"po-a"}));
    MC_CHECK_EQ(names_of(snapshot.protections_covering(MaintenanceScope({target_of(TargetKind::Site, "site-b")}))),
                (std::vector<std::string>{"po-b"}));
    MC_CHECK(snapshot.protections_covering(MaintenanceScope({target_of(TargetKind::Asset, "asset-a3")})).empty());
    MC_CHECK(snapshot.protections_covering(MaintenanceScope{}).empty());

    // WindowSpec arithmetic used by the callers above.
    const WindowSpec window{kT0, kT0 + kNanosPerHour};
    MC_CHECK(window.contains(kT0));
    MC_CHECK(window.contains(kT0 + kNanosPerHour));
    MC_CHECK(!window.contains(kT0 - 1LL));
    MC_CHECK(window.overlaps(kT0 - kNanosPerMinute, kT0));
    MC_CHECK(!window.overlaps(kT0 + kNanosPerHour + 1LL, kT0 + 2LL * kNanosPerHour));
    MC_CHECK_EQ(window.duration_nanos(), kNanosPerHour);
    MC_CHECK_EQ(WindowSpec{}.duration_nanos(), 0LL);
    MC_CHECK(!WindowSpec{}.contains(kT0));
    MC_CHECK(!WindowSpec{}.overlaps(kT0, kT0 + 1LL));
}

// ---------------------------------------------------------------------------
// Plans and plan records
// ---------------------------------------------------------------------------

MC_TEST(maintenance_plan_validation_and_digest_sensitivity) {
    const MaintenancePlan plan = make_plan();
    MC_REQUIRE_OK(plan.validate());
    MC_CHECK(!plan.digest.is_zero());
    MC_CHECK_EQ(plan.scope.size(), 1U);
    MC_CHECK(plan.scope.contains(target_of(TargetKind::Rack, "rack-a1")));
    MC_CHECK(plan.window.contains(kT0 + kNanosPerHour));
    MC_CHECK(!plan.window.contains(kT0 - 1LL));
    MC_CHECK_EQ(plan.window.duration_nanos(), 2LL * kNanosPerHour);

    // finalize() is idempotent.
    MaintenancePlan repeated = plan;
    repeated.finalize();
    MC_CHECK(repeated.digest == plan.digest);
    repeated.finalize();
    MC_CHECK(repeated.digest == plan.digest);
    MC_CHECK(repeated == plan);

    MaintenancePlan empty_scope = plan;
    empty_scope.scope = MaintenanceScope{};
    check_plan_rejected(empty_scope, ErrorCode::InvalidArgument, "plan without a target");

    MaintenancePlan zero_length = plan;
    zero_length.window.end = zero_length.window.start;
    check_plan_rejected(zero_length, ErrorCode::InvalidArgument, "plan window that ends where it starts");

    MaintenancePlan backwards = plan;
    backwards.window.start = plan.window.end + 1LL;
    check_plan_rejected(backwards, ErrorCode::InvalidArgument, "plan window that ends before it starts");

    MaintenancePlan open_window = plan;
    open_window.window.end = kNoTimestamp;
    check_plan_rejected(open_window, ErrorCode::InvalidArgument, "plan window without an end");

    MaintenancePlan missing_generation = plan;
    missing_generation.maintenance_generation = MaintenanceGeneration{};
    check_plan_rejected(missing_generation, ErrorCode::MissingGeneration, "plan without a maintenance generation");

    MaintenancePlan missing_digest = plan;
    missing_digest.policy_digest = Digest{};
    check_plan_rejected(missing_digest, ErrorCode::InvalidArgument, "plan without a policy digest");

    MaintenancePlan bad_requester = plan;
    bad_requester.requested_by = "operator 1";
    check_plan_rejected(bad_requester, ErrorCode::InvalidArgument, "plan with an invalid requester identity");

    MaintenancePlan no_reason = plan;
    no_reason.reason.clear();
    check_plan_rejected(no_reason, ErrorCode::InvalidArgument, "plan without a reason");

    MaintenancePlan no_identity = plan;
    no_identity.id = PlanId{};
    check_plan_rejected(no_identity, ErrorCode::InvalidArgument, "plan without an identity");

    // Digest sensitivity: scope, window, risk and every bound digest.
    const Digest base = plan.digest;
    MaintenancePlan wider = plan;
    wider.scope =
        MaintenanceScope({target_of(TargetKind::Rack, "rack-a1"), target_of(TargetKind::Asset, "asset-a1")});
    wider.finalize();
    MC_CHECK(wider.digest != base);

    MaintenancePlan shifted = plan;
    shifted.window.start += kNanosPerMinute;
    shifted.finalize();
    MC_CHECK(shifted.digest != base);

    MaintenancePlan riskier = plan;
    riskier.risk = ServiceRiskClass::Critical;
    riskier.finalize();
    MC_CHECK(riskier.digest != base);

    MaintenancePlan other_policy = plan;
    other_policy.policy_digest = digest_of(purpose::kPolicy, std::string_view("another-policy"));
    other_policy.finalize();
    MC_CHECK(other_policy.digest != base);

    MaintenancePlan other_dependencies = plan;
    other_dependencies.dependency_digest = digest_of(purpose::kDependencySnapshot, std::string_view("other-deps"));
    other_dependencies.finalize();
    MC_CHECK(other_dependencies.digest != base);

    MaintenancePlan next_revision = plan;
    next_revision.revision = Revision::from_value(8U);
    next_revision.finalize();
    MC_CHECK(next_revision.digest != base);

    MaintenancePlan other_activity = plan;
    other_activity.activity = MaintenanceActivity::DataWipe;
    other_activity.finalize();
    MC_CHECK(other_activity.digest != base);

    // The scope is canonical, so the input order of its targets is irrelevant.
    MaintenancePlan ordered_a = plan;
    ordered_a.scope =
        MaintenanceScope({target_of(TargetKind::Asset, "asset-a1"), target_of(TargetKind::Rack, "rack-a1")});
    ordered_a.finalize();
    MaintenancePlan ordered_b = plan;
    ordered_b.scope =
        MaintenanceScope({target_of(TargetKind::Rack, "rack-a1"), target_of(TargetKind::Asset, "asset-a1")});
    ordered_b.finalize();
    MC_CHECK(ordered_a.digest == ordered_b.digest);
    MC_CHECK(ordered_a.digest != base);
}

MC_TEST(plan_record_canonicalize_orders_collections) {
    const PlanRecord canonical = make_record();
    MC_REQUIRE_OK(canonical.validate());
    MC_CHECK(!canonical.digest.is_zero());

    PlanRecord shuffled = make_record();
    std::reverse(shuffled.receipts.begin(), shuffled.receipts.end());
    std::reverse(shuffled.progress.begin(), shuffled.progress.end());
    std::reverse(shuffled.exceptions.begin(), shuffled.exceptions.end());
    std::reverse(shuffled.transitions.begin(), shuffled.transitions.end());
    shuffled.canonicalize();

    // Receipts: (observation_sequence, id).
    MC_CHECK_EQ(shuffled.receipts.size(), 2U);
    MC_REQUIRE(shuffled.receipts.size() == 2U);
    MC_CHECK_EQ(shuffled.receipts[0].id.name(), std::string("rc-1"));
    MC_CHECK_EQ(shuffled.receipts[0].observation_sequence.value(), 1U);
    MC_CHECK_EQ(shuffled.receipts[1].id.name(), std::string("rc-2"));
    MC_CHECK_EQ(shuffled.receipts[1].observation_sequence.value(), 2U);
    MC_CHECK(!shuffled.receipts[0].digest.is_zero());
    MC_CHECK(shuffled.receipts[0].digest == canonical.find_receipt(id_of<ReceiptId>("rc-1"))->digest);

    // Progress: by time.
    MC_CHECK_EQ(shuffled.progress.size(), 3U);
    MC_REQUIRE(shuffled.progress.size() == 3U);
    MC_CHECK_EQ(shuffled.progress[0].note, std::string("note-a"));
    MC_CHECK_EQ(shuffled.progress[1].note, std::string("note-b"));
    MC_CHECK_EQ(shuffled.progress[2].note, std::string("note-c"));
    MC_CHECK(shuffled.progress[0].at < shuffled.progress[1].at);
    MC_CHECK(shuffled.progress[1].at < shuffled.progress[2].at);
    MC_CHECK(!shuffled.progress[0].digest.is_zero());

    // Exceptions: by identity.
    MC_CHECK_EQ(shuffled.exceptions.size(), 2U);
    MC_REQUIRE(shuffled.exceptions.size() == 2U);
    MC_CHECK_EQ(shuffled.exceptions[0].id.name(), std::string("exc-a"));
    MC_CHECK_EQ(shuffled.exceptions[1].id.name(), std::string("exc-z"));

    // Transitions: by (at, sequence), and still a chain.
    MC_CHECK_EQ(shuffled.transitions.size(), 2U);
    MC_REQUIRE(shuffled.transitions.size() == 2U);
    MC_CHECK(shuffled.transitions[0].from == PlanPhase::Proposed);
    MC_CHECK(shuffled.transitions[0].to == PlanPhase::Evaluating);
    MC_CHECK(shuffled.transitions[1].from == PlanPhase::Evaluating);
    MC_CHECK(shuffled.transitions[1].to == PlanPhase::Approved);
    MC_CHECK(shuffled.transitions[0].sequence.value() == 1U);
    MC_CHECK(shuffled.transitions[1].sequence.value() == 2U);
    MC_CHECK(!shuffled.transitions[0].digest.is_zero());

    // Canonicalization is order independent: the digest is identical.
    shuffled.finalize();
    MC_CHECK(shuffled.digest == canonical.digest);
    MC_CHECK(shuffled == canonical);
    MC_REQUIRE_OK(shuffled.validate());

    // Two receipts sharing a sequence are ordered by identity.
    PlanRecord tied = make_record();
    tied.receipts.push_back(make_receipt("rc-1b", "ob-1", ReceiptKind::DrainObserved, 2U, tied.plan.revision));
    tied.finalize();
    MC_CHECK_EQ(tied.receipts.size(), 3U);
    MC_REQUIRE(tied.receipts.size() == 3U);
    MC_CHECK_EQ(tied.receipts[0].id.name(), std::string("rc-1"));
    MC_CHECK_EQ(tied.receipts[1].id.name(), std::string("rc-1b"));
    MC_CHECK_EQ(tied.receipts[2].id.name(), std::string("rc-2"));
    MC_REQUIRE_OK(tied.validate());

    // Lookups and derived helpers.
    MC_CHECK(canonical.find_receipt(id_of<ReceiptId>("rc-1")) != nullptr);
    MC_CHECK(canonical.find_receipt(id_of<ReceiptId>("rc-nope")) == nullptr);
    MC_CHECK(canonical.find_exception(id_of<ExceptionId>("exc-a")) != nullptr);
    MC_CHECK(canonical.find_exception(id_of<ExceptionId>("exc-nope")) == nullptr);
    MC_CHECK_EQ(canonical.receipts_for(id_of<ObligationId>("ob-1")).size(), 1U);
    MC_CHECK(canonical.receipts_for(id_of<ObligationId>("ob-2")).size() == 1U);
    MC_CHECK(canonical.receipts_for(id_of<ObligationId>("ob-nope")).empty());
    MC_CHECK(!canonical.work_stopped());
    MC_CHECK(!canonical.work_completed());
    MC_CHECK(canonical.evaluation.has_value());
    MC_CHECK(canonical.approval.has_value());
    MC_CHECK_EQ(canonical.obligations.obligations.size(), 2U);
}

MC_TEST(plan_record_validate_rejects_bad_bindings) {
    // A receipt bound to another revision of the plan.
    {
        PlanRecord record = make_record();
        MC_CHECK_EQ(record.receipts.size(), 2U);
        MC_REQUIRE(record.receipts.size() == 2U);
        record.receipts[0].plan_revision = Revision::from_value(8U);
        check_record_rejected(record, ErrorCode::StaleRevision, "receipt for another plan revision");
    }
    // Two receipts with one identity.
    {
        PlanRecord record = make_record();
        record.receipts[1].observation_sequence = ObservationSequence::from_value(3U);
        Receipt duplicate = record.receipts[0];
        duplicate.observation_sequence = ObservationSequence::from_value(2U);
        record.receipts.push_back(duplicate);
        check_record_rejected(record, ErrorCode::DuplicateIdentity, "two receipts with one identity");
    }
    // An approval bound to a different plan.
    {
        PlanRecord record = make_record();
        MC_REQUIRE(record.approval.has_value());
        record.approval->plan = id_of<PlanId>("plan-2");
        check_record_rejected(record, ErrorCode::IdentityMismatch, "approval for a different plan");
    }
    // An approval bound to a different revision.
    {
        PlanRecord record = make_record();
        MC_REQUIRE(record.approval.has_value());
        record.approval->plan_revision = Revision::from_value(9U);
        check_record_rejected(record, ErrorCode::StaleRevision, "approval for a different revision");
    }
    // An evaluation whose plan digest is not the plan digest.
    {
        PlanRecord record = make_record();
        MC_REQUIRE(record.evaluation.has_value());
        record.evaluation->plan_digest = digest_of(purpose::kPlanDigest, std::string_view("another-plan"));
        check_record_rejected(record, ErrorCode::StalePlanDigest, "evaluation for another plan digest");
    }
    // An evaluation bound to a different revision.
    {
        PlanRecord record = make_record();
        MC_REQUIRE(record.evaluation.has_value());
        record.evaluation->plan_revision = Revision::from_value(4U);
        check_record_rejected(record, ErrorCode::StaleRevision, "evaluation for another revision");
    }
    // A receipt without provenance is an invalid argument, not a stale binding.
    {
        PlanRecord record = make_record();
        record.receipts[0].evidence.clear();
        check_record_rejected(record, ErrorCode::InvalidArgument, "receipt without provenance");
    }
    // A receipt without an observation sequence.
    {
        PlanRecord record = make_record();
        record.receipts[0].observation_sequence = ObservationSequence{};
        check_record_rejected(record, ErrorCode::MissingGeneration, "receipt without an observation sequence");
    }
    // An obligation set that is not canonical.  This is checked without
    // finalize(), because finalize() would canonicalize the set again.
    {
        PlanRecord record = make_record();
        std::reverse(record.obligations.obligations.begin(), record.obligations.obligations.end());
        MC_CHECK_CODE(record.validate(), ErrorCode::LayoutInvalid);
    }
    // A record whose digest was never computed (also checked without finalize).
    {
        PlanRecord record = make_record();
        record.digest = Digest{};
        MC_CHECK_CODE(record.validate(), ErrorCode::InvalidArgument);
    }
    // The unmodified record is valid, so each rejection above is specific.
    const PlanRecord valid = make_record();
    MC_REQUIRE_OK(valid.validate());
}

MC_TEST(plan_record_validate_rejects_unlinked_chains_and_stale_water_marks) {
    // Two transitions that do not link up.
    {
        PlanRecord record = make_record();
        MC_CHECK_EQ(record.transitions.size(), 2U);
        MC_REQUIRE(record.transitions.size() == 2U);
        record.transitions[0].to = PlanPhase::Ready;
        record.phase = PlanPhase::Approved;
        check_record_rejected(record, ErrorCode::LayoutInvalid, "transition chain that does not link");
    }
    // A recorded phase that does not match the last transition.
    {
        PlanRecord record = make_record();
        record.phase = PlanPhase::Ready;
        check_record_rejected(record, ErrorCode::LayoutInvalid, "recorded phase that is not the chain end");
    }
    // A high water mark below a stored receipt sequence.
    {
        PlanRecord record = make_record();
        record.last_observation_sequence = ObservationSequence::from_value(1U);
        check_record_rejected(record, ErrorCode::LayoutInvalid, "water mark below a stored receipt");
    }
    // An unset high water mark is accepted; the mark is only checked when set.
    {
        PlanRecord record = make_record();
        record.last_observation_sequence = ObservationSequence{};
        record.finalize();
        MC_REQUIRE_OK(record.validate());
    }
    // A mark above every receipt is accepted.
    {
        PlanRecord record = make_record();
        record.last_observation_sequence = ObservationSequence::from_value(99U);
        record.finalize();
        MC_REQUIRE_OK(record.validate());
    }
}

// ---------------------------------------------------------------------------
// Exceptions, approvals and evaluations
// ---------------------------------------------------------------------------

MC_TEST(exception_grant_validation_and_covers) {
    const ExceptionGrant grant = make_grant();
    MC_REQUIRE_OK(grant.validate());
    MC_CHECK(!grant.digest.is_zero());
    MC_CHECK(grant.active_at(kT0));
    MC_CHECK(grant.active_at(kT0 + kNanosPerHour));
    MC_CHECK(!grant.active_at(kT0 - 1LL));
    MC_CHECK(!grant.active_at(kT0 + kNanosPerHour + 1LL));

    const TargetRef asset_a1 = target_of(TargetKind::Asset, "asset-a1");
    const TargetRef asset_a2 = target_of(TargetKind::Asset, "asset-a2");

    // covers() is true only for a waivable code, an in scope target and an
    // instant inside the validity window.
    MC_CHECK(grant.covers(ErrorCode::RedundancyInsufficient, asset_a1, kT0));
    MC_CHECK(grant.covers(ErrorCode::RedundancyInsufficient, asset_a1, kT0 + kNanosPerMinute));
    MC_CHECK(grant.covers(ErrorCode::RedundancyInsufficient, asset_a1, kT0 + kNanosPerHour));
    MC_CHECK(!grant.covers(ErrorCode::RedundancyInsufficient, asset_a1, kT0 - 1LL));
    MC_CHECK(!grant.covers(ErrorCode::RedundancyInsufficient, asset_a1, kT0 + kNanosPerHour + 1LL));
    MC_CHECK(!grant.covers(ErrorCode::ProtectedWindowActive, asset_a1, kT0));
    MC_CHECK(!grant.covers(ErrorCode::HardInterlock, asset_a1, kT0));
    MC_CHECK(!grant.covers(ErrorCode::RedundancyInsufficient, asset_a2, kT0));
    MC_CHECK(grant.covers(ErrorCode::PowerHeadroomInsufficient, target_of(TargetKind::Rack, "rack-a1"), kT0));
    MC_CHECK(!grant.covers(ErrorCode::PowerHeadroomInsufficient, target_of(TargetKind::Rack, "rack-a2"), kT0));

    // An empty justification is invalid.
    {
        ExceptionGrant broken = make_grant();
        broken.justification.clear();
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    // An expiry that is not after the grant time is invalid.
    {
        ExceptionGrant broken = make_grant();
        broken.expires_at = broken.granted_at;
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    {
        ExceptionGrant broken = make_grant();
        broken.expires_at = broken.granted_at - 1LL;
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    {
        ExceptionGrant broken = make_grant();
        broken.expires_at = kNoTimestamp;
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    // No grantor, no plan, no target, no waived condition, no digest.
    {
        ExceptionGrant broken = make_grant();
        broken.granted_by.clear();
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
        broken = make_grant();
        broken.plan = PlanId{};
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
        broken = make_grant();
        broken.targets.clear();
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
        broken = make_grant();
        broken.waived.clear();
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
        broken = make_grant();
        broken.digest = Digest{};
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
        broken = make_grant();
        broken.id = ExceptionId{};
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    // A hard interlock is never waivable, and neither are the other hard
    // conditions an exception might be pointed at.
    {
        ExceptionGrant broken = make_grant();
        broken.waived = {ErrorCode::HardInterlock};
        MC_CHECK_CODE(broken.validate(), ErrorCode::HardInterlock);
    }
    {
        ExceptionGrant broken = make_grant();
        broken.waived = {ErrorCode::ProtectedObligationViolated};
        MC_CHECK_CODE(broken.validate(), ErrorCode::HardInterlock);
    }
    {
        ExceptionGrant broken = make_grant();
        broken.waived = {ErrorCode::LifecycleStateInvalid};
        MC_CHECK_CODE(broken.validate(), ErrorCode::HardInterlock);
    }
    {
        ExceptionGrant broken = make_grant();
        broken.waived = {ErrorCode::RedundancyInsufficient, ErrorCode::HardInterlock};
        MC_CHECK_CODE(broken.validate(), ErrorCode::HardInterlock);
    }
    // A wholly waivable set still validates.
    {
        ExceptionGrant good = make_grant();
        good.waived = {ErrorCode::ProtectedWindowActive, ErrorCode::ActiveIncident};
        good.finalize();
        MC_REQUIRE_OK(good.validate());
        MC_CHECK(good.covers(ErrorCode::ActiveIncident, asset_a1, kT0));
        MC_CHECK(!good.covers(ErrorCode::RedundancyInsufficient, asset_a1, kT0));
    }
    // canonicalize() sorts and de-duplicates both lists, which is what makes
    // covers() a binary search.
    {
        ExceptionGrant unsorted = make_grant();
        unsorted.waived = {ErrorCode::ActiveIncident, ErrorCode::RedundancyInsufficient, ErrorCode::ActiveIncident};
        unsorted.targets = {target_of(TargetKind::Rack, "rack-a1"), target_of(TargetKind::Asset, "asset-a1"),
                            target_of(TargetKind::Asset, "asset-a1")};
        unsorted.canonicalize();
        MC_CHECK_EQ(unsorted.waived.size(), 2U);
        MC_REQUIRE(unsorted.waived.size() == 2U);
        MC_CHECK(unsorted.waived[0] == ErrorCode::RedundancyInsufficient);
        MC_CHECK(unsorted.waived[1] == ErrorCode::ActiveIncident);
        MC_CHECK_EQ(unsorted.targets.size(), 2U);
        MC_REQUIRE(unsorted.targets.size() == 2U);
        MC_CHECK(unsorted.targets[0].kind() == TargetKind::Rack);
        MC_CHECK(unsorted.targets[1].kind() == TargetKind::Asset);
        unsorted.finalize();
        MC_CHECK(unsorted.covers(ErrorCode::ActiveIncident, asset_a1, kT0));
    }
}

MC_TEST(approval_record_validation) {
    const MaintenancePlan plan = make_plan();
    const ApprovalRecord approval = make_approval(plan);
    MC_REQUIRE_OK(approval.validate());
    MC_CHECK(!approval.digest.is_zero());
    MC_CHECK_EQ(approval.plan.name(), std::string("plan-1"));
    MC_CHECK(!approval.expired_at(approval.approved_at));
    MC_CHECK(!approval.expired_at(approval.expires_at));
    MC_CHECK(approval.expired_at(approval.expires_at + 1LL));

    {
        ApprovalRecord broken = approval;
        broken.approved_by.clear();
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    {
        ApprovalRecord broken = approval;
        broken.approved_by = "approver 1";  // not a valid identifier
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    {
        ApprovalRecord broken = approval;
        broken.approval_evidence.clear();
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    {
        ApprovalRecord broken = approval;
        broken.facility_epoch = FacilityEpoch{};
        MC_CHECK_CODE(broken.validate(), ErrorCode::MissingGeneration);
    }
    {
        ApprovalRecord broken = approval;
        broken.policy_generation = PolicyGeneration{};
        MC_CHECK_CODE(broken.validate(), ErrorCode::MissingGeneration);
    }
    {
        ApprovalRecord broken = approval;
        broken.plan_revision = Revision{};
        MC_CHECK_CODE(broken.validate(), ErrorCode::MissingGeneration);
    }
    {
        ApprovalRecord broken = approval;
        broken.expires_at = broken.approved_at;
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    {
        ApprovalRecord broken = approval;
        broken.expires_at = broken.approved_at - 1LL;
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    {
        // An unset expiry is expired, and invalid.
        ApprovalRecord broken = approval;
        broken.expires_at = kNoTimestamp;
        MC_CHECK(broken.expired_at(kT0));
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    {
        ApprovalRecord broken = approval;
        broken.evaluation_digest = Digest{};
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    {
        ApprovalRecord broken = approval;
        broken.plan = PlanId{};
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    {
        ApprovalRecord broken = approval;
        broken.id = ApprovalId{};
        MC_CHECK_CODE(broken.validate(), ErrorCode::InvalidArgument);
    }
    // canonicalize() sorts and de-duplicates the exception list.
    {
        ApprovalRecord exceptions = approval;
        exceptions.exceptions = {id_of<ExceptionId>("exc-z"), id_of<ExceptionId>("exc-a"),
                                 id_of<ExceptionId>("exc-z")};
        exceptions.canonicalize();
        MC_CHECK_EQ(exceptions.exceptions.size(), 2U);
        MC_REQUIRE(exceptions.exceptions.size() == 2U);
        MC_CHECK_EQ(exceptions.exceptions[0].name(), std::string("exc-a"));
        MC_CHECK_EQ(exceptions.exceptions[1].name(), std::string("exc-z"));
        exceptions.finalize();
        MC_CHECK(exceptions.digest != approval.digest);
    }
}

MC_TEST(precondition_report_partitions_and_canonical_order) {
    const MaintenancePlan plan = make_plan();
    const PreconditionReport report = make_evaluation(plan);
    MC_REQUIRE_OK(report.validate());
    MC_CHECK(!report.digest.is_zero());
    MC_CHECK_EQ(report.plan_revision.value(), plan.revision.value());

    // Canonical order is (condition, subject, detail).
    MC_CHECK_EQ(report.conditions.size(), 3U);
    MC_REQUIRE(report.conditions.size() == 3U);
    MC_CHECK(report.conditions[0].condition == ErrorCode::RedundancyInsufficient);
    MC_CHECK(report.conditions[1].condition == ErrorCode::PowerHeadroomInsufficient);
    MC_CHECK(report.conditions[2].condition == ErrorCode::HardInterlock);
    for (std::size_t index = 1; index < report.conditions.size(); ++index) {
        MC_CHECK(report.conditions[index - 1U] < report.conditions[index]);
    }

    // blocking() and blocking(true) partition the unsatisfied conditions.
    const std::vector<ConditionResult> blocking = report.blocking();
    const std::vector<ConditionResult> hard = report.blocking(true);
    MC_CHECK_EQ(blocking.size(), 2U);
    MC_CHECK_EQ(hard.size(), 1U);
    MC_REQUIRE(hard.size() == 1U);
    MC_CHECK(hard[0].condition == ErrorCode::HardInterlock);
    MC_CHECK(hard[0].hard);
    for (const ConditionResult& condition : hard) {
        MC_CHECK(condition.hard);
        MC_CHECK(!condition.satisfied);
        bool present = false;
        for (const ConditionResult& candidate : blocking) {
            present = present || candidate.condition == condition.condition;
        }
        MC_CHECK_MSG(present, "a hard blocker must also be a blocker");
    }
    for (const ConditionResult& condition : blocking) {
        MC_CHECK(!condition.satisfied);
    }
    MC_CHECK_EQ(blocking.size(), hard.size() + 1U);  // one unsatisfied soft condition

    // find()/has() see every condition, satisfied or not.
    MC_CHECK(report.has(ErrorCode::HardInterlock));
    MC_CHECK(report.has(ErrorCode::PowerHeadroomInsufficient));
    MC_CHECK(report.has(ErrorCode::RedundancyInsufficient));
    MC_CHECK(!report.has(ErrorCode::ProtectedWindowActive));
    MC_REQUIRE(report.find(ErrorCode::HardInterlock) != nullptr);
    MC_CHECK_EQ(report.find(ErrorCode::HardInterlock)->subject, std::string("rack-a1"));
    MC_CHECK(report.find(ErrorCode::HardInterlock)->hard);
    MC_CHECK(!report.find(ErrorCode::HardInterlock)->waivable);
    MC_REQUIRE(report.find(ErrorCode::RedundancyInsufficient) != nullptr);
    MC_CHECK(report.find(ErrorCode::RedundancyInsufficient)->waivable);
    MC_CHECK(!report.find(ErrorCode::RedundancyInsufficient)->satisfied);
    MC_CHECK(report.find(ErrorCode::ProtectedWindowActive) == nullptr);

    // The canonical order is enforced by validate().
    {
        PreconditionReport unsorted = report;
        std::swap(unsorted.conditions[0], unsorted.conditions[2]);
        MC_CHECK_CODE(unsorted.validate(), ErrorCode::LayoutInvalid);
    }
    // Two conditions that compare equal are not a strictly increasing order.
    {
        PreconditionReport duplicated = report;
        duplicated.conditions.push_back(duplicated.conditions[0]);
        MC_CHECK_CODE(duplicated.validate(), ErrorCode::LayoutInvalid);
    }
    // A report with no condition at all is invalid.
    {
        PreconditionReport empty = report;
        empty.conditions.clear();
        MC_CHECK_CODE(empty.validate(), ErrorCode::InvalidArgument);
    }
    // A report that is not bound to a full generation set is invalid.
    {
        PreconditionReport unbound = report;
        unbound.topology_generation = TopologyGeneration{};
        MC_CHECK_CODE(unbound.validate(), ErrorCode::MissingGeneration);
    }
    {
        PreconditionReport unbound = report;
        unbound.evaluated_at = kNoTimestamp;
        MC_CHECK_CODE(unbound.validate(), ErrorCode::InvalidArgument);
    }
    {
        PreconditionReport unbound = report;
        unbound.plan_digest = Digest{};
        MC_CHECK_CODE(unbound.validate(), ErrorCode::InvalidArgument);
    }
}

// ---------------------------------------------------------------------------
// White box codec round trips
// ---------------------------------------------------------------------------

MC_TEST(detail_codec_round_trips_facility_and_plan_record) {
    const FacilitySnapshot snapshot = make_facility();

    Writer snapshot_writer;
    detail::encode(snapshot_writer, snapshot);
    MC_CHECK(snapshot_writer.size() > 0U);
    Reader snapshot_reader(snapshot_writer.data());
    FacilitySnapshot decoded_snapshot;
    MC_REQUIRE_OK(detail::decode(snapshot_reader, decoded_snapshot));
    MC_CHECK(snapshot_reader.fully_consumed());
    MC_CHECK(decoded_snapshot.digest == snapshot.digest);
    MC_CHECK(decoded_snapshot.dependency_digest == snapshot.dependency_digest);
    MC_REQUIRE_OK(decoded_snapshot.validate());
    MC_CHECK_EQ(names_of(decoded_snapshot.sites()), names_of(snapshot.sites()));
    MC_CHECK_EQ(decoded_snapshot.assets.size(), snapshot.assets.size());
    MC_CHECK_EQ(decoded_snapshot.protected_obligations.size(), snapshot.protected_obligations.size());
    MC_REQUIRE(decoded_snapshot.find_asset(id_of<AssetId>("asset-a1")) != nullptr);
    MC_CHECK_EQ(decoded_snapshot.find_asset(id_of<AssetId>("asset-a1"))->capacity_units, 10U);
    MC_CHECK(decoded_snapshot.find_asset(id_of<AssetId>("asset-a1"))->isolatable);
    MC_REQUIRE(decoded_snapshot.find_redundancy_group(id_of<RedundancyGroupId>("rg-a")) != nullptr);
    MC_CHECK_EQ(decoded_snapshot.find_redundancy_group(id_of<RedundancyGroupId>("rg-a"))->members.size(), 2U);

    // A truncated payload is a truncated record, not a silent partial decode.
    {
        std::vector<std::uint8_t> truncated = snapshot_writer.data();
        truncated.resize(truncated.size() / 2U);
        Reader reader(truncated);
        FacilitySnapshot partial;
        MC_CHECK_CODE(detail::decode(reader, partial), ErrorCode::RecordTruncated);
    }
    // Trailing bytes are visible through fully_consumed().
    {
        std::vector<std::uint8_t> extended = snapshot_writer.data();
        extended.push_back(0x00U);
        Reader reader(extended);
        FacilitySnapshot decoded;
        MC_REQUIRE_OK(detail::decode(reader, decoded));
        MC_CHECK(!reader.fully_consumed());
        MC_CHECK_EQ(reader.remaining(), 1U);
    }

    const PlanRecord record = make_record();
    Writer record_writer;
    detail::encode(record_writer, record);
    MC_CHECK(record_writer.size() > 0U);
    Reader record_reader(record_writer.data());
    PlanRecord decoded_record;
    MC_REQUIRE_OK(detail::decode(record_reader, decoded_record));
    MC_CHECK(record_reader.fully_consumed());
    MC_CHECK(decoded_record.digest == record.digest);
    MC_REQUIRE_OK(decoded_record.validate());
    MC_CHECK(decoded_record == record);
    MC_CHECK(decoded_record.plan.digest == record.plan.digest);
    MC_CHECK(decoded_record.evaluation.has_value());
    MC_CHECK(decoded_record.approval.has_value());
    MC_CHECK(decoded_record.evaluation->digest == record.evaluation->digest);
    MC_CHECK(decoded_record.approval->digest == record.approval->digest);
    MC_CHECK_EQ(decoded_record.receipts.size(), 2U);
    MC_REQUIRE(decoded_record.find_receipt(id_of<ReceiptId>("rc-1")) != nullptr);
    MC_CHECK(decoded_record.find_receipt(id_of<ReceiptId>("rc-1"))->digest ==
             record.find_receipt(id_of<ReceiptId>("rc-1"))->digest);
    MC_CHECK_EQ(decoded_record.receipts_for(id_of<ObligationId>("ob-1")).size(), 1U);
    MC_CHECK_EQ(decoded_record.progress.size(), 3U);
    MC_CHECK_EQ(decoded_record.obligations.obligations.size(), 2U);
    MC_CHECK(decoded_record.obligations.digest == record.obligations.digest);
    MC_CHECK_EQ(decoded_record.blockers.size(), 1U);
    MC_CHECK(decoded_record.blockers[0].condition == ErrorCode::ActiveIncident);
    MC_CHECK_EQ(decoded_record.transitions.size(), 2U);
    MC_CHECK(decoded_record.transitions[1].digest == record.transitions[1].digest);
}

MC_TEST(detail_codec_round_trips_present_and_absent_idents) {
    // An identity that is legitimately absent carries an explicit presence
    // flag, so "no identity" and "an identity whose name is empty" are never
    // the same byte string, and a default constructed Ident survives a round
    // trip without being confused with a parsed empty name.
    ConditionResult condition;
    condition.condition = ErrorCode::RedundancyInsufficient;
    condition.satisfied = true;
    condition.hard = false;
    condition.waivable = true;
    condition.measured = true;
    condition.subject = "rg-a";
    condition.detail = "1 unit of margin";
    condition.observed = 2;
    condition.required = 1;
    // condition.waiver is deliberately left unset.
    MC_CHECK(condition.waiver.empty());

    Writer absent_writer;
    detail::encode(absent_writer, condition);
    Reader absent_reader(absent_writer.data());
    ConditionResult absent_decoded;
    MC_REQUIRE_OK(detail::decode(absent_reader, absent_decoded));
    MC_CHECK(absent_reader.fully_consumed());
    MC_CHECK(absent_decoded == condition);
    MC_CHECK(absent_decoded.waiver.empty());

    // The same value with a stated waiver also round trips, and the two
    // encodings differ, so presence is part of the canonical bytes.
    ConditionResult stated = condition;
    stated.waiver = id_of<ExceptionId>("exc-1");
    Writer stated_writer;
    detail::encode(stated_writer, stated);
    MC_CHECK(stated_writer.size() != absent_writer.size());
    Reader stated_reader(stated_writer.data());
    ConditionResult stated_decoded;
    MC_REQUIRE_OK(detail::decode(stated_reader, stated_decoded));
    MC_CHECK(stated_reader.fully_consumed());
    MC_CHECK(stated_decoded == stated);
    MC_CHECK_EQ(stated_decoded.waiver.name(), std::string("exc-1"));
    MC_CHECK(!(stated_decoded == condition));
}

MC_TEST_MAIN()

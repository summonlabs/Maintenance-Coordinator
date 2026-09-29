// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// End-to-end lifecycle, fencing, replay and recovery proofs at the engine
// boundary.  The plant model is synthetic; the store, the lock, the records and
// the processes are real.

#include "../fixture.hpp"
#include "../harness.hpp"
#include "../mc_test.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mc;        // NOLINT(google-build-using-namespace)
using namespace mc::test;  // NOLINT(google-build-using-namespace)

namespace {

ProposeRequest base_proposal(const char* rack = "rack-01") {
    ProposeRequest request;
    request.reason = "replace failed accelerator";
    request.requested_by = "operator-1";
    request.activity = MaintenanceActivity::HardwareRepair;
    request.priority = PlanPriority::Elevated;
    request.risk = ServiceRiskClass::High;
    request.scope = rack_scope(rack);
    request.window = fixture_window();
    return request;
}

// Brings a plan to the PreDrain phase: propose, evaluate, approve, derive.
Result<CommandResult> reach_pre_drain(Harness& harness, const char* attempt_prefix = "a") {
    auto proposed = harness.coordinator().propose(harness.with_attempt(base_proposal(), std::string(attempt_prefix) + "-propose"));
    if (!proposed.ok()) {
        return proposed.status();
    }
    const PlanId id = proposed.value().plan.plan.id;
    const Revision revision = proposed.value().plan.plan.revision;

    EvaluateRequest evaluate;
    evaluate.plan = id;
    evaluate.revision = revision;
    auto evaluated = harness.coordinator().evaluate(harness.with_attempt(evaluate, std::string(attempt_prefix) + "-evaluate"));
    if (!evaluated.ok()) {
        return evaluated.status();
    }
    ApproveRequest approve;
    approve.plan = id;
    approve.revision = revision;
    approve.approved_by = "approver-1";
    approve.approval_evidence = "change-record-4711";
    auto approved = harness.coordinator().approve(harness.with_attempt(approve, std::string(attempt_prefix) + "-approve"));
    if (!approved.ok()) {
        return approved.status();
    }
    DeriveObligationsRequest derive;
    derive.plan = id;
    derive.revision = revision;
    return harness.coordinator().derive_obligations(harness.with_attempt(derive, std::string(attempt_prefix) + "-derive"));
}

// Ingests the satisfying observation for every obligation of one stage.
Status satisfy_stage(Harness& harness, const PlanRecord& record, ObligationStage stage, std::uint64_t& sequence,
                     const std::string& prefix) {
    for (const auto& obligation : record.obligations.obligations) {
        if (stage_of(obligation.kind) != stage) {
            continue;
        }
        IngestReceiptRequest ingest;
        ingest.plan = record.plan.id;
        ingest.revision = record.plan.revision;
        ingest.receipt_id = receipt_id(("receipt-" + prefix + "-" + obligation.id.name()).c_str());
        ingest.obligation = obligation.id;
        ingest.kind = satisfying_receipt(obligation.kind);
        ingest.issuer_authority = authority_of(obligation.kind);
        ingest.issuer = std::string(to_string(obligation.authority)) + "-authority";
        ingest.observation_sequence = ObservationSequence::from_value(++sequence);
        ingest.evidence = "evidence for " + obligation.id.name();
        ingest.observed_at = mid_window();
        auto result = harness.coordinator().ingest_receipt(
            harness.with_attempt(ingest, "ingest-" + prefix + "-" + obligation.id.name()));
        if (!result.ok()) {
            return result.status();
        }
        if (result.value().plan.phase == PlanPhase::Ready) {
            return Status::success();
        }
    }
    return Status::success();
}

}  // namespace

MC_TEST(propose_records_intent_without_approval) {
    Harness harness("engine-propose", make_facility());
    auto result = harness.coordinator().propose(harness.with_attempt(base_proposal(), "p1"));
    MC_REQUIRE_OK(result);
    MC_CHECK_EQ(static_cast<int>(result.value().plan.phase), static_cast<int>(PlanPhase::Proposed));
    MC_CHECK(result.value().plan.approval.has_value() == false);
    MC_CHECK(result.value().plan.obligations.obligations.empty());
    MC_CHECK_EQ(result.value().plan.plan.revision.value(), 1ULL);
    MC_CHECK(result.value().mutation.replayed == false);
}

MC_TEST(proposal_validation_is_deterministic) {
    Harness harness("engine-propose-invalid", make_facility());
    {
        ProposeRequest request = base_proposal();
        request.scope = MaintenanceScope(std::vector<TargetRef>{});
        auto result = harness.coordinator().propose(harness.with_attempt(request, "bad-1"));
        MC_CHECK_CODE(result.status(), ErrorCode::InvalidArgument);
    }
    {
        ProposeRequest request = base_proposal();
        request.window.end = request.window.start;
        auto result = harness.coordinator().propose(harness.with_attempt(request, "bad-2"));
        MC_CHECK_CODE(result.status(), ErrorCode::InvalidArgument);
    }
    {
        ProposeRequest request = base_proposal("rack-99");
        auto result = harness.coordinator().propose(harness.with_attempt(request, "bad-3"));
        MC_CHECK_CODE(result.status(), ErrorCode::UnknownTarget);
    }
    {
        ProposeRequest request = base_proposal();
        request.requested_by = "not a valid identity";
        auto result = harness.coordinator().propose(harness.with_attempt(request, "bad-4"));
        MC_CHECK_CODE(result.status(), ErrorCode::InvalidArgument);
    }
}

MC_TEST(happy_path_completes_only_with_evidence) {
    Harness harness("engine-happy", make_facility());
    auto reached = reach_pre_drain(harness);
    MC_REQUIRE_OK(reached);
    const PlanRecord after_derive = reached.value().plan;
    MC_CHECK_EQ(static_cast<int>(after_derive.phase), static_cast<int>(PlanPhase::PreDrain));
    MC_CHECK(after_derive.obligations.obligations.size() > 0);

    // Beginning now is refused: nothing has been drained.
    {
        BeginRequest begin;
        begin.plan = after_derive.plan.id;
        begin.revision = after_derive.plan.revision;
        auto result = harness.coordinator().begin(harness.with_attempt(begin, "begin-too-early"));
        MC_CHECK_CODE(result.status(), ErrorCode::ObligationsOutstanding);
    }

    std::uint64_t sequence = 0;
    MC_REQUIRE_OK(satisfy_stage(harness, after_derive, ObligationStage::PreDrain, sequence, "pre"));
    MC_REQUIRE_OK(satisfy_stage(harness, after_derive, ObligationStage::Isolation, sequence, "iso"));

    ShowRequest show;
    show.plan = after_derive.plan.id;
    auto shown = harness.coordinator().show(show);
    MC_REQUIRE_OK(shown);
    MC_CHECK_EQ(static_cast<int>(shown.value().plan.phase), static_cast<int>(PlanPhase::Ready));

    // Work cannot begin outside the window.
    harness.clock().set(kWindowStart - kNanosPerHour);
    {
        BeginRequest begin;
        begin.plan = after_derive.plan.id;
        begin.revision = after_derive.plan.revision;
        auto result = harness.coordinator().begin(harness.with_attempt(begin, "begin-closed"));
        MC_CHECK_CODE(result.status(), ErrorCode::WindowNotOpen);
    }
    harness.clock().set(mid_window());
    {
        BeginRequest begin;
        begin.plan = after_derive.plan.id;
        begin.revision = after_derive.plan.revision;
        auto result = harness.coordinator().begin(harness.with_attempt(begin, "begin"));
        MC_REQUIRE_OK(result);
        MC_CHECK_EQ(static_cast<int>(result.value().plan.phase), static_cast<int>(PlanPhase::InProgress));
    }

    // Completion is refused while the work is still in progress.
    {
        CompleteRequest complete;
        complete.plan = after_derive.plan.id;
        complete.revision = after_derive.plan.revision;
        auto result = harness.coordinator().complete(harness.with_attempt(complete, "complete-early"));
        MC_CHECK_CODE(result.status(), ErrorCode::IllegalPhase);
    }

    {
        RecordProgressRequest progress;
        progress.plan = after_derive.plan.id;
        progress.revision = after_derive.plan.revision;
        progress.kind = ProgressKind::WorkCompleted;
        progress.note = "replacement installed";
        auto result = harness.coordinator().record_progress(harness.with_attempt(progress, "progress-done"));
        MC_REQUIRE_OK(result);
        MC_CHECK_EQ(static_cast<int>(result.value().plan.phase), static_cast<int>(PlanPhase::Verification));
    }

    // Verification without the maintenance-verified observation is refused.
    {
        VerifyRequest verify;
        verify.plan = after_derive.plan.id;
        verify.revision = after_derive.plan.revision;
        auto result = harness.coordinator().verify_restoration(harness.with_attempt(verify, "verify-early"));
        MC_CHECK_CODE(result.status(), ErrorCode::CompletionEvidenceMissing);
    }

    MC_REQUIRE_OK(satisfy_stage(harness, after_derive, ObligationStage::Completion, sequence, "done"));

    {
        VerifyRequest verify;
        verify.plan = after_derive.plan.id;
        verify.revision = after_derive.plan.revision;
        auto result = harness.coordinator().verify_restoration(harness.with_attempt(verify, "verify"));
        MC_REQUIRE_OK(result);
        MC_CHECK_EQ(static_cast<int>(result.value().plan.phase), static_cast<int>(PlanPhase::Restore));
        MC_CHECK(result.value().restoration.has_value());
        MC_CHECK(result.value().restoration->restored);
    }
    {
        CompleteRequest complete;
        complete.plan = after_derive.plan.id;
        complete.revision = after_derive.plan.revision;
        complete.note = "window closed";
        auto result = harness.coordinator().complete(harness.with_attempt(complete, "complete"));
        MC_REQUIRE_OK(result);
        MC_CHECK_EQ(static_cast<int>(result.value().plan.phase), static_cast<int>(PlanPhase::Complete));
        MC_CHECK(result.value().completion.has_value());
        MC_CHECK(result.value().completion->restoration_verified);
        MC_CHECK(result.value().completion->protected_obligations_satisfied);
    }
    // A completed plan is terminal.
    {
        CancelRequest cancel;
        cancel.plan = after_derive.plan.id;
        cancel.revision = after_derive.plan.revision;
        cancel.reason = "too late";
        auto result = harness.coordinator().cancel(harness.with_attempt(cancel, "cancel-late"));
        MC_CHECK_CODE(result.status(), ErrorCode::AlreadyTerminal);
    }
    MC_CHECK(harness.events().size() >= 8);
}

MC_TEST(acknowledgement_is_not_an_effect) {
    Harness harness("engine-ack", make_facility());
    auto reached = reach_pre_drain(harness);
    MC_REQUIRE_OK(reached);
    const PlanRecord record = reached.value().plan;

    const Obligation* drain = nullptr;
    for (const auto& obligation : record.obligations.obligations) {
        if (obligation.kind == ObligationKind::AsiDrain) {
            drain = &obligation;
            break;
        }
    }
    MC_REQUIRE(drain != nullptr);

    IngestReceiptRequest acknowledged;
    acknowledged.plan = record.plan.id;
    acknowledged.revision = record.plan.revision;
    acknowledged.receipt_id = receipt_id("receipt-ack-1");
    acknowledged.obligation = drain->id;
    acknowledged.kind = ReceiptKind::DrainAcknowledged;
    acknowledged.issuer_authority = ObligationAuthority::Asi;
    acknowledged.issuer = "asi-authority";
    acknowledged.observation_sequence = ObservationSequence::from_value(1U);
    acknowledged.evidence = "asi accepted the drain request";
    acknowledged.observed_at = mid_window();
    auto ingested = harness.coordinator().ingest_receipt(harness.with_attempt(acknowledged, "ack-1"));
    MC_REQUIRE_OK(ingested);
    MC_REQUIRE(ingested.value().obligations.has_value());
    const ObligationStatus* status = ingested.value().obligations->find(drain->id);
    MC_REQUIRE(status != nullptr);
    MC_CHECK_EQ(static_cast<int>(status->disposition), static_cast<int>(Disposition::Acknowledged));
    MC_CHECK(ingested.value().plan.phase == PlanPhase::PreDrain);
}

MC_TEST(a_generation_change_fences_an_approval) {
    Harness harness("engine-fence", make_facility());
    auto reached = reach_pre_drain(harness);
    MC_REQUIRE_OK(reached);
    const PlanRecord record = reached.value().plan;

    std::uint64_t sequence = 0;
    MC_REQUIRE_OK(satisfy_stage(harness, record, ObligationStage::PreDrain, sequence, "pre"));
    MC_REQUIRE_OK(satisfy_stage(harness, record, ObligationStage::Isolation, sequence, "iso"));

    // A later facility model with a new dependency generation.
    auto changed = make_facility(2U, 2U, 1U, 1U, 1U, 1U, 1U);
    InstallFacilityRequest install;
    install.facility = changed;
    MC_REQUIRE_OK(harness.install(install, "install-2"));

    BeginRequest begin;
    begin.plan = record.plan.id;
    begin.revision = record.plan.revision;
    auto result = harness.coordinator().begin(harness.with_attempt(begin, "begin-fenced"));
    MC_CHECK_CODE(result.status(), ErrorCode::StaleDependencyGeneration);

    ExplainRequest explain;
    explain.plan = record.plan.id;
    auto explanation = harness.coordinator().explain(explain);
    MC_REQUIRE_OK(explanation);
    MC_REQUIRE(explanation.value().explanation.has_value());
    MC_CHECK(explanation.value().explanation->can_begin == false);
}

MC_TEST(soft_conditions_can_be_waived_and_hard_interlocks_cannot) {
    {
        FacilitySnapshot facility = make_facility();
        BlackoutPeriod blackout;
        blackout.id = BlackoutId::parse("blackout-1").value();
        blackout.targets = {target(TargetKind::Rack, "rack-01")};
        blackout.start = kWindowStart - kNanosPerHour;
        blackout.end = kWindowEnd + kNanosPerHour;
        blackout.hard = false;
        blackout.reason = "tenant change freeze";
        facility.blackouts.push_back(blackout);
        facility.finalize();

        Harness harness("engine-waive-soft", facility);
        auto proposed = harness.coordinator().propose(harness.with_attempt(base_proposal(), "s-propose"));
        MC_REQUIRE_OK(proposed);
        const PlanId id = proposed.value().plan.plan.id;
        const Revision revision = proposed.value().plan.plan.revision;

        EvaluateRequest evaluate;
        evaluate.plan = id;
        evaluate.revision = revision;
        auto blocked = harness.coordinator().evaluate(harness.with_attempt(evaluate, "s-evaluate"));
        MC_REQUIRE_OK(blocked);
        MC_CHECK_EQ(static_cast<int>(blocked.value().plan.phase), static_cast<int>(PlanPhase::Blocked));

        GrantExceptionRequest grant;
        grant.plan = id;
        grant.revision = revision;
        grant.waived = {ErrorCode::BlackoutPeriodActive};
        grant.targets = {target(TargetKind::Rack, "rack-01")};
        grant.granted_by = "facility-director";
        grant.justification = "emergency replacement approved during freeze";
        grant.validity_nanos = kNanosPerHour;
        auto granted = harness.coordinator().grant_exception(harness.with_attempt(grant, "s-grant"));
        MC_REQUIRE_OK(granted);

        auto re_evaluated = harness.coordinator().evaluate(harness.with_attempt(evaluate, "s-evaluate-2"));
        MC_REQUIRE_OK(re_evaluated);
        MC_CHECK_EQ(static_cast<int>(re_evaluated.value().plan.phase), static_cast<int>(PlanPhase::Evaluating));
        MC_REQUIRE(re_evaluated.value().evaluation.has_value());
        MC_CHECK(re_evaluated.value().evaluation->satisfied);
        MC_CHECK(re_evaluated.value().evaluation->satisfied_without_exceptions == false);
        MC_CHECK_EQ(re_evaluated.value().evaluation->applied_exceptions.size(), static_cast<std::size_t>(1));

        ApproveRequest approve;
        approve.plan = id;
        approve.revision = revision;
        approve.approved_by = "approver-1";
        approve.approval_evidence = "change-record-99";
        auto approved = harness.coordinator().approve(harness.with_attempt(approve, "s-approve"));
        MC_REQUIRE_OK(approved);
        MC_REQUIRE(approved.value().approval.has_value());
        MC_CHECK_EQ(approved.value().approval->exceptions.size(), static_cast<std::size_t>(1));
    }
    {
        // A hard interlock is refused on every path that could set it aside.
        FacilitySnapshot facility = make_facility();
        facility.power_domains.front().interlocked = true;
        facility.power_domains.front().interlock_reason = "arc-flash boundary active";
        facility.finalize();

        Harness harness("engine-hard-interlock", facility);
        auto proposed = harness.coordinator().propose(harness.with_attempt(base_proposal(), "h-propose"));
        MC_REQUIRE_OK(proposed);
        const PlanId id = proposed.value().plan.plan.id;
        const Revision revision = proposed.value().plan.plan.revision;

        EvaluateRequest evaluate;
        evaluate.plan = id;
        evaluate.revision = revision;
        auto blocked = harness.coordinator().evaluate(harness.with_attempt(evaluate, "h-evaluate"));
        MC_REQUIRE_OK(blocked);
        MC_CHECK_EQ(static_cast<int>(blocked.value().plan.phase), static_cast<int>(PlanPhase::Blocked));
        MC_REQUIRE(blocked.value().evaluation.has_value());
        MC_CHECK(blocked.value().evaluation->has(ErrorCode::HardInterlock));

        GrantExceptionRequest grant;
        grant.plan = id;
        grant.revision = revision;
        grant.waived = {ErrorCode::HardInterlock};
        grant.targets = {target(TargetKind::PowerDomain, "pd-a")};
        grant.granted_by = "facility-director";
        grant.justification = "attempt to bypass the interlock";
        grant.validity_nanos = kNanosPerHour;
        auto granted = harness.coordinator().grant_exception(harness.with_attempt(grant, "h-grant"));
        MC_CHECK_CODE(granted.status(), ErrorCode::HardInterlock);

        ApproveRequest approve;
        approve.plan = id;
        approve.revision = revision;
        approve.approved_by = "approver-1";
        approve.approval_evidence = "change-record-100";
        auto approved = harness.coordinator().approve(harness.with_attempt(approve, "h-approve"));
        MC_CHECK_CODE(approved.status(), ErrorCode::HardInterlock);
    }
}

MC_TEST(protected_obligations_block_maintenance) {
    FacilitySnapshot facility = make_facility();
    facility.redundancy_groups.front().observed_available_units = 2;
    ProtectedObligation protection;
    protection.id = protection_id("protect-tenant-a");
    protection.targets = {target(TargetKind::Site, "site-a")};
    protection.required_mode = RedundancyMode::NPlusOne;
    protection.required_units = 2;
    protection.hard_interlock = true;
    protection.description = "tenant A requires N+1 throughout its contract";
    facility.protected_obligations.push_back(protection);
    facility.finalize();

    Harness harness("engine-protection", facility);
    auto proposed = harness.coordinator().propose(harness.with_attempt(base_proposal(), "p-propose"));
    MC_REQUIRE_OK(proposed);
    EvaluateRequest evaluate;
    evaluate.plan = proposed.value().plan.plan.id;
    evaluate.revision = proposed.value().plan.plan.revision;
    auto blocked = harness.coordinator().evaluate(harness.with_attempt(evaluate, "p-evaluate"));
    MC_REQUIRE_OK(blocked);
    MC_REQUIRE(blocked.value().evaluation.has_value());
    MC_CHECK(blocked.value().evaluation->has(ErrorCode::ProtectedObligationViolated));

    GrantExceptionRequest grant;
    grant.plan = proposed.value().plan.plan.id;
    grant.revision = proposed.value().plan.plan.revision;
    grant.waived = {ErrorCode::ProtectedObligationViolated};
    grant.targets = {target(TargetKind::Site, "site-a")};
    grant.granted_by = "facility-director";
    grant.justification = "attempt to waive a protection";
    grant.validity_nanos = kNanosPerHour;
    auto granted = harness.coordinator().grant_exception(harness.with_attempt(grant, "p-grant"));
    MC_CHECK_CODE(granted.status(), ErrorCode::HardInterlock);
}

MC_TEST(replaying_an_attempt_never_double_applies) {
    Harness harness("engine-replay", make_facility());
    ProposeRequest request = harness.with_attempt(base_proposal(), "replay-propose");
    auto first = harness.coordinator().propose(request);
    MC_REQUIRE_OK(first);
    auto second = harness.coordinator().propose(request);
    MC_REQUIRE_OK(second);
    MC_CHECK(second.value().mutation.replayed);
    MC_CHECK(first.value().mutation.sequence == second.value().mutation.sequence);
    MC_CHECK(first.value().plan.plan.digest == second.value().plan.plan.digest);

    // The same attempt id with a different intent is refused.
    ProposeRequest changed = request;
    changed.reason = "a different reason";
    changed.header.intent_digest = intent_digest_of(changed);
    auto mismatched = harness.coordinator().propose(changed);
    MC_CHECK_CODE(mismatched.status(), ErrorCode::ReplayIntentMismatch);

    // A tampered intent digest is refused before anything else happens.
    ProposeRequest tampered = harness.with_attempt(base_proposal(), "replay-propose-2");
    tampered.header.intent_digest = Digest{};
    auto refused = harness.coordinator().propose(tampered);
    MC_CHECK_CODE(refused.status(), ErrorCode::ReplayIntentMismatch);
}

MC_TEST(cancellation_rules_follow_effect) {
    Harness harness("engine-cancel", make_facility());
    auto reached = reach_pre_drain(harness);
    MC_REQUIRE_OK(reached);
    const PlanRecord record = reached.value().plan;

    CancelRequest cancel;
    cancel.plan = record.plan.id;
    cancel.revision = record.plan.revision;
    cancel.reason = "parts unavailable";
    auto cancelled = harness.coordinator().cancel(harness.with_attempt(cancel, "cancel-before"));
    MC_REQUIRE_OK(cancelled);
    MC_CHECK_EQ(static_cast<int>(cancelled.value().plan.phase), static_cast<int>(PlanPhase::Cancelled));

    // A second plan that has started cannot be cancelled outright.
    Harness second("engine-cancel-started", make_facility());
    auto started = reach_pre_drain(second, "b");
    MC_REQUIRE_OK(started);
    const PlanRecord in_flight = started.value().plan;
    std::uint64_t sequence = 0;
    MC_REQUIRE_OK(satisfy_stage(second, in_flight, ObligationStage::PreDrain, sequence, "pre"));
    MC_REQUIRE_OK(satisfy_stage(second, in_flight, ObligationStage::Isolation, sequence, "iso"));
    {
        BeginRequest begin;
        begin.plan = in_flight.plan.id;
        begin.revision = in_flight.plan.revision;
        MC_REQUIRE_OK(second.coordinator().begin(second.with_attempt(begin, "b-begin")));
    }
    {
        CancelRequest in_flight_cancel;
        in_flight_cancel.plan = in_flight.plan.id;
        in_flight_cancel.revision = in_flight.plan.revision;
        in_flight_cancel.reason = "stop";
        auto result = second.coordinator().cancel(second.with_attempt(in_flight_cancel, "b-cancel"));
        MC_CHECK_CODE(result.status(), ErrorCode::IllegalPhase);
    }
}

MC_TEST(interrupted_windows_require_recovery_and_fresh_evidence) {
    const std::filesystem::path directory = make_temp_dir("engine-recovery");
    PlanRecord before_crash;
    {
        Harness harness(directory.string(), make_facility(), true);
        auto reached = reach_pre_drain(harness);
        MC_REQUIRE_OK(reached);
        const PlanRecord record = reached.value().plan;
        std::uint64_t sequence = 0;
        MC_REQUIRE_OK(satisfy_stage(harness, record, ObligationStage::PreDrain, sequence, "pre"));
        MC_REQUIRE_OK(satisfy_stage(harness, record, ObligationStage::Isolation, sequence, "iso"));
        BeginRequest begin;
        begin.plan = record.plan.id;
        begin.revision = record.plan.revision;
        MC_REQUIRE_OK(harness.coordinator().begin(harness.with_attempt(begin, "begin")));
        ShowRequest show;
        show.plan = record.plan.id;
        auto shown = harness.coordinator().show(show);
        MC_REQUIRE_OK(shown);
        before_crash = shown.value().plan;
        MC_CHECK_EQ(static_cast<int>(before_crash.phase), static_cast<int>(PlanPhase::InProgress));
    }

    // The process that owned the store is gone; a new one recovers the ledger.
    ManualClock clock(mid_window());
    StoreOptions options;
    options.directory = directory;
    auto reopened = Store::open(options);
    MC_REQUIRE_OK(reopened);
    auto store = std::move(reopened).value();
    MC_CHECK(store->recovery().plans_recovered >= 1);
    Coordinator coordinator(*store, clock, nullptr);

    RecoverRequest recover;
    recover.plan = before_crash.plan.id;
    recover.header.attempt = attempt_id("recover-1");
    recover.header.intent_digest = intent_digest_of(recover);
    auto recovered = coordinator.recover(recover);
    MC_REQUIRE_OK(recovered);
    MC_CHECK_EQ(static_cast<int>(recovered.value().plan.phase), static_cast<int>(PlanPhase::Restore));
    MC_CHECK(recovered.value().plan.recovery_required);

    CompleteRequest complete;
    complete.plan = before_crash.plan.id;
    complete.revision = before_crash.plan.revision;
    complete.note = "attempt after crash";
    complete.header.attempt = attempt_id("complete-after-crash");
    complete.header.intent_digest = intent_digest_of(complete);
    auto refused = coordinator.complete(complete);
    MC_CHECK_CODE(refused.status(), ErrorCode::RecoveryRequired);

    // Fresh stop evidence, newer than the recovery mark, clears the obligation.
    const Obligation* work_stop = nullptr;
    for (const auto& obligation : before_crash.obligations.obligations) {
        if (obligation.kind == ObligationKind::WorkStopConfirmed) {
            work_stop = &obligation;
            break;
        }
    }
    MC_REQUIRE(work_stop != nullptr);

    IngestReceiptRequest stop;
    stop.plan = before_crash.plan.id;
    stop.revision = before_crash.plan.revision;
    stop.receipt_id = receipt_id("receipt-stop-fresh");
    stop.obligation = work_stop->id;
    stop.kind = ReceiptKind::WorkStopObserved;
    stop.issuer_authority = ObligationAuthority::DccpPlant;
    stop.issuer = "plant-controller";
    stop.observation_sequence = ObservationSequence::from_value(1000U);
    stop.evidence = "walkdown confirms no work in flight";
    stop.observed_at = mid_window();
    stop.header.attempt = attempt_id("stop-1");
    stop.header.intent_digest = intent_digest_of(stop);
    auto ingested = coordinator.ingest_receipt(stop);
    MC_REQUIRE_OK(ingested);
    MC_CHECK(ingested.value().plan.recovery_required == false);

    // Loopback: the store still rejects a stale observation sequence.
    stop.receipt_id = receipt_id("receipt-stop-stale");
    stop.observation_sequence = ObservationSequence::from_value(999U);
    stop.header.attempt = attempt_id("stop-2");
    stop.header.intent_digest = intent_digest_of(stop);
    auto stale = coordinator.ingest_receipt(stop);
    MC_CHECK_CODE(stale.status(), ErrorCode::StaleObservation);
}

MC_TEST(concurrent_windows_on_one_rack_conflict) {
    Harness harness("engine-concurrent", make_facility());
    auto first = reach_pre_drain(harness, "one");
    MC_REQUIRE_OK(first);
    const PlanRecord first_record = first.value().plan;
    std::uint64_t sequence = 0;
    MC_REQUIRE_OK(satisfy_stage(harness, first_record, ObligationStage::PreDrain, sequence, "pre"));
    MC_REQUIRE_OK(satisfy_stage(harness, first_record, ObligationStage::Isolation, sequence, "iso"));
    ShowRequest show;
    show.plan = first_record.plan.id;
    auto shown = harness.coordinator().show(show);
    MC_REQUIRE_OK(shown);
    MC_REQUIRE(shown.value().plan.phase == PlanPhase::Ready);

    ProposeRequest second = base_proposal();
    second.reason = "second window on the same rack";
    auto proposed = harness.coordinator().propose(harness.with_attempt(second, "two-propose"));
    MC_REQUIRE_OK(proposed);
    EvaluateRequest evaluate;
    evaluate.plan = proposed.value().plan.plan.id;
    evaluate.revision = proposed.value().plan.plan.revision;
    auto evaluated = harness.coordinator().evaluate(harness.with_attempt(evaluate, "two-evaluate"));
    MC_REQUIRE_OK(evaluated);
    MC_REQUIRE(evaluated.value().evaluation.has_value());
    MC_CHECK(evaluated.value().evaluation->has(ErrorCode::ConcurrentMaintenanceConflict));
    MC_CHECK_EQ(static_cast<int>(evaluated.value().plan.phase), static_cast<int>(PlanPhase::Blocked));
}

MC_TEST(identical_inputs_produce_identical_state) {
    Harness one("engine-determinism-1", make_facility());
    Harness two("engine-determinism-2", make_facility());
    auto first = reach_pre_drain(one, "d");
    auto second = reach_pre_drain(two, "d");
    MC_REQUIRE_OK(first);
    MC_REQUIRE_OK(second);
    MC_CHECK(first.value().plan.plan.digest == second.value().plan.plan.digest);
    MC_CHECK(first.value().plan.obligations.digest == second.value().plan.obligations.digest);
    MC_CHECK(first.value().mutation.sequence == second.value().mutation.sequence);
    MC_CHECK(first.value().plan.approval->digest == second.value().plan.approval->digest);
}

MC_TEST(receipts_must_come_from_the_owning_authority) {
    Harness harness("engine-authority", make_facility());
    auto reached = reach_pre_drain(harness);
    MC_REQUIRE_OK(reached);
    const PlanRecord record = reached.value().plan;
    const Obligation* drain = nullptr;
    for (const auto& obligation : record.obligations.obligations) {
        if (obligation.kind == ObligationKind::AsiDrain) {
            drain = &obligation;
            break;
        }
    }
    MC_REQUIRE(drain != nullptr);

    IngestReceiptRequest forged;
    forged.plan = record.plan.id;
    forged.revision = record.plan.revision;
    forged.receipt_id = receipt_id("receipt-forged");
    forged.obligation = drain->id;
    forged.kind = ReceiptKind::DrainObserved;
    forged.issuer_authority = ObligationAuthority::Dfi;  // ASI owns this obligation
    forged.issuer = "dfi-authority";
    forged.observation_sequence = ObservationSequence::from_value(1U);
    forged.evidence = "not this authority's business";
    forged.observed_at = mid_window();
    auto refused = harness.coordinator().ingest_receipt(harness.with_attempt(forged, "forged-1"));
    MC_CHECK_CODE(refused.status(), ErrorCode::IdentityMismatch);

    MC_CHECK(harness.store().audit().ok());
}

MC_TEST_MAIN()

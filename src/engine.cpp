// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// The coordination engine.
//
// Locking contract, stated once and honoured everywhere in this file:
//
//   * one state mutex serialises the commands a Coordinator executes;
//   * the store has its own internal mutex, and lock order is always
//     coordinator mutex -> store mutex, never the reverse;
//   * no callback, sink, allocator hook or user code runs while the state mutex
//     is held: events are emitted after it is released;
//   * a command either commits a complete record or leaves both the durable
//     state and the in-memory ledger exactly as they were.

#include "mc/engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine_internal.hpp"
#include "mc/version.hpp"
#include "serialize.hpp"

namespace mc {
namespace {

[[nodiscard]] std::string padded(std::uint64_t value, std::size_t width) {
    std::string text = std::to_string(value);
    while (text.size() < width) {
        text.insert(text.begin(), '0');
    }
    return text;
}

template <typename Fn>
[[nodiscard]] Digest intent_digest(std::string_view command, Fn&& body) {
    Writer writer;
    writer.blob(command);
    body(writer);
    return mc::digest_of(purpose::kOperationIntent, writer.data());
}

void write_scope(Writer& writer, const MaintenanceScope& scope) {
    writer.u32(static_cast<std::uint32_t>(scope.size()));
    for (const auto& target : scope.targets()) {
        writer.blob(target.str());
    }
}

void write_targets(Writer& writer, const std::vector<TargetRef>& targets) {
    writer.u32(static_cast<std::uint32_t>(targets.size()));
    for (const auto& target : targets) {
        writer.blob(target.str());
    }
}

[[nodiscard]] bool phases_equal(PlanPhase lhs, PlanPhase rhs) noexcept {
    return static_cast<std::uint8_t>(lhs) == static_cast<std::uint8_t>(rhs);
}

}  // namespace

// ---------------------------------------------------------------------------
// Intent digests
// ---------------------------------------------------------------------------

Digest intent_digest_of(const ProposeRequest& request) {
    return intent_digest("propose", [&request](Writer& writer) {
        writer.blob(request.plan_id.name());
        writer.blob(request.reason);
        writer.blob(request.requested_by);
        writer.u8(static_cast<std::uint8_t>(request.activity));
        writer.u8(static_cast<std::uint8_t>(request.priority));
        writer.u8(static_cast<std::uint8_t>(request.risk));
        write_scope(writer, request.scope);
        writer.i64(request.window.start);
        writer.i64(request.window.end);
        writer.boolean(request.window.flexible);
    });
}

Digest intent_digest_of(const EvaluateRequest& request) {
    return intent_digest("evaluate", [&request](Writer& writer) {
        writer.blob(request.plan.name());
        writer.generation(request.revision);
    });
}

Digest intent_digest_of(const ApproveRequest& request) {
    return intent_digest("approve", [&request](Writer& writer) {
        writer.blob(request.plan.name());
        writer.generation(request.revision);
        writer.blob(request.approved_by);
        writer.blob(request.approval_evidence);
        writer.blob(request.witness);
    });
}

Digest intent_digest_of(const DeriveObligationsRequest& request) {
    return intent_digest("derive-obligations", [&request](Writer& writer) {
        writer.blob(request.plan.name());
        writer.generation(request.revision);
    });
}

Digest intent_digest_of(const IngestReceiptRequest& request) {
    return intent_digest("ingest-receipt", [&request](Writer& writer) {
        writer.blob(request.plan.name());
        writer.generation(request.revision);
        writer.blob(request.receipt_id.name());
        writer.blob(request.obligation.name());
        writer.u8(static_cast<std::uint8_t>(request.kind));
        writer.u8(static_cast<std::uint8_t>(request.issuer_authority));
        writer.blob(request.issuer);
        writer.generation(request.observation_sequence);
        writer.blob(request.evidence);
        writer.i64(request.observed_at);
    });
}

Digest intent_digest_of(const GrantExceptionRequest& request) {
    return intent_digest("grant-exception", [&request](Writer& writer) {
        writer.blob(request.plan.name());
        writer.generation(request.revision);
        writer.u32(static_cast<std::uint32_t>(request.waived.size()));
        for (const auto code : request.waived) {
            writer.u16(static_cast<std::uint16_t>(code));
        }
        write_targets(writer, request.targets);
        writer.blob(request.granted_by);
        writer.blob(request.justification);
        writer.i64(request.validity_nanos);
    });
}

Digest intent_digest_of(const BeginRequest& request) {
    return intent_digest("begin", [&request](Writer& writer) {
        writer.blob(request.plan.name());
        writer.generation(request.revision);
    });
}

Digest intent_digest_of(const RecordProgressRequest& request) {
    return intent_digest("record-progress", [&request](Writer& writer) {
        writer.blob(request.plan.name());
        writer.generation(request.revision);
        writer.u8(static_cast<std::uint8_t>(request.kind));
        writer.blob(request.note);
    });
}

Digest intent_digest_of(const VerifyRequest& request) {
    return intent_digest("verify-restoration", [&request](Writer& writer) {
        writer.blob(request.plan.name());
        writer.generation(request.revision);
    });
}

Digest intent_digest_of(const CompleteRequest& request) {
    return intent_digest("complete", [&request](Writer& writer) {
        writer.blob(request.plan.name());
        writer.generation(request.revision);
        writer.blob(request.note);
    });
}

Digest intent_digest_of(const CancelRequest& request) {
    return intent_digest("cancel", [&request](Writer& writer) {
        writer.blob(request.plan.name());
        writer.generation(request.revision);
        writer.blob(request.reason);
    });
}

Digest intent_digest_of(const RecoverRequest& request) {
    return intent_digest("recover", [&request](Writer& writer) { writer.blob(request.plan.name()); });
}

Digest intent_digest_of(const InstallFacilityRequest& request) {
    return intent_digest("install-facility", [&request](Writer& writer) {
        writer.digest(request.facility.digest);
        writer.u64(request.facility.revision.value());
    });
}

Digest intent_digest_of(const CompactRequest&) {
    return intent_digest("compact", [](Writer&) {});
}

// ---------------------------------------------------------------------------
// Coordinator implementation
// ---------------------------------------------------------------------------

class Coordinator::Impl {
public:
    Impl(Store& store, const Clock& clock, EventSink* sink) : store_(&store), clock_(&clock), sink_(sink) {}

    Store& store() noexcept { return *store_; }
    const Clock& clock() const noexcept { return *clock_; }
    std::mutex& mutex() const noexcept { return mutex_; }

    // --- shared plumbing -----------------------------------------------------

    [[nodiscard]] Timestamp now() const { return clock_->now_nanos(); }

    [[nodiscard]] Result<std::optional<CommandResult>> replay_of(const CommandHeader& header,
                                                                const Digest& expected_intent) const {
        if (header.attempt.empty()) {
            return fail(ErrorCode::InvalidArgument, "every mutating command needs an attempt identity",
                        "attempt");
        }
        if (!(header.intent_digest == expected_intent)) {
            return fail(ErrorCode::ReplayIntentMismatch,
                        "the supplied intent digest does not describe this request", "attempt",
                        "supplied " + header.intent_digest.hex() + ", derived " + expected_intent.hex());
        }
        const LedgerState& ledger = store_->cached_ledger();
        const AttemptRecord* recorded = ledger.find_attempt(header.attempt);
        if (recorded == nullptr) {
            return std::optional<CommandResult>{};
        }
        if (!(recorded->intent_digest == expected_intent)) {
            return fail(ErrorCode::ReplayIntentMismatch,
                        "this attempt identity was used for a different operation", "attempt",
                        header.attempt.name());
        }
        CommandResult result;
        result.mutation.intent_digest = expected_intent;
        result.mutation.outcome_digest = recorded->outcome_digest;
        result.mutation.sequence = recorded->sequence;
        result.mutation.applied_at = recorded->applied_at;
        result.mutation.replayed = true;
        if (!recorded->plan.empty()) {
            if (const PlanRecord* record = ledger.find_plan(recorded->plan); record != nullptr) {
                result.has_plan = true;
                result.plan = *record;
                result.obligations = detail::derive_obligation_status(*record, now());
                if (record->evaluation.has_value()) {
                    result.evaluation = record->evaluation;
                }
                if (record->approval.has_value()) {
                    result.approval = record->approval;
                }
            }
        }
        if (ledger.facility.has_value()) {
            result.facility = ledger.facility;
        }
        result.store_recovery = store_->recovery();
        result.notes.push_back("operation was already applied; the reported state is the current one");
        return std::optional<CommandResult>(std::move(result));
    }

    [[nodiscard]] Status require_plan(const LedgerState& ledger, const PlanId& id, const Revision& revision,
                                      const PlanRecord*& out) const {
        const PlanRecord* record = ledger.find_plan(id);
        if (record == nullptr) {
            return fail(ErrorCode::UnknownPlan, "no such plan", "plan", id.name());
        }
        if (!revision.is_set()) {
            return fail(ErrorCode::MissingGeneration, "the command does not name a plan revision", "plan",
                        id.name());
        }
        if (record->plan.revision != revision) {
            return fail(ErrorCode::StaleRevision, "the command names a superseded plan revision", "plan",
                        id.name());
        }
        out = record;
        return Status::success();
    }

    [[nodiscard]] std::vector<const PlanRecord*> active_plans(const LedgerState& ledger) const {
        std::vector<const PlanRecord*> result;
        for (const auto& entry : ledger.plans) {
            const PlanPhase phase = entry.second.phase;
            if (phase == PlanPhase::Ready || is_started(phase)) {
                result.push_back(&entry.second);
            }
        }
        return result;
    }

    [[nodiscard]] std::optional<FacilitySnapshot> facility_or_null(const LedgerState& ledger) const {
        return ledger.facility;
    }

    [[nodiscard]] Result<CommitSequence> commit_plan(const PlanRecord& record, const CommandHeader& header,
                                                     const Digest& expected_intent, std::string_view command,
                                                     std::uint64_t next_plan_number, Digest& outcome,
                                                     Timestamp& applied_at) {
        CommitSequence next = store_->cached_ledger().sequence.is_set()
                                  ? store_->cached_ledger().sequence.next()
                                  : CommitSequence::from_value(1U);
        outcome = detail::outcome_digest_of(command, record, next);
        applied_at = now();
        Transaction transaction;
        transaction.plan = record;
        transaction.next_plan_number = next_plan_number;
        if (!header.attempt.empty()) {
            AttemptCommit attempt;
            attempt.id = header.attempt;
            attempt.record.intent_digest = expected_intent;
            attempt.record.outcome_digest = outcome;
            attempt.record.sequence = next;
            attempt.record.plan = record.plan.id;
            attempt.record.applied_at = applied_at;
            transaction.attempt = std::move(attempt);
        }
        auto committed = store_->commit(transaction);
        if (!committed.ok()) {
            return committed.status();
        }
        return committed.value();
    }

    [[nodiscard]] Result<CommitSequence> commit_facility(const FacilitySnapshot& facility, const CommandHeader& header,
                                                         const Digest& expected_intent, std::string_view command,
                                                         Digest& outcome, Timestamp& applied_at) {
        CommitSequence next = store_->cached_ledger().sequence.is_set()
                                  ? store_->cached_ledger().sequence.next()
                                  : CommitSequence::from_value(1U);
        outcome = detail::outcome_digest_of(command, facility, next);
        applied_at = now();
        Transaction transaction;
        transaction.facility = facility;
        if (!header.attempt.empty()) {
            AttemptCommit attempt;
            attempt.id = header.attempt;
            attempt.record.intent_digest = expected_intent;
            attempt.record.outcome_digest = outcome;
            attempt.record.sequence = next;
            attempt.record.applied_at = applied_at;
            transaction.attempt = std::move(attempt);
        }
        auto committed = store_->commit(transaction);
        if (!committed.ok()) {
            return committed.status();
        }
        return committed.value();
    }

    [[nodiscard]] CommandResult base_result() const {
        CommandResult result;
        result.store_recovery = store_->recovery();
        return result;
    }

    void emit(EngineEvent event) const {
        if (sink_ != nullptr) {
            sink_->on_event(event);
        }
    }

    // Runs one locked command and emits its event only after the lock has been
    // released, so a sink can never observe or deadlock against the state lock.
    template <typename Request, typename Method>
    [[nodiscard]] Result<CommandResult> execute(const Request& request, Method method) {
        std::optional<EngineEvent> event;
        Result<CommandResult> result = [&]() -> Result<CommandResult> {
            const std::lock_guard<std::mutex> guard(mutex_);
            return (this->*method)(request, event);
        }();
        if (event.has_value()) {
            emit(*event);
        }
        return result;
    }

    // The authoritative record after a commit: the store assigns the commit
    // sequence and refreshes the record digest, so the value returned to the
    // caller is read back from the ledger rather than from the pre-commit copy.
    [[nodiscard]] PlanRecord refreshed_plan(const PlanRecord& fallback) const {
        const PlanRecord* stored = store_->cached_ledger().find_plan(fallback.plan.id);
        return stored != nullptr ? *stored : fallback;
    }

    [[nodiscard]] Status fence_against_facility(const FacilitySnapshot& facility, const MaintenancePlan& plan,
                                                const PreconditionReport& report) const {
        Status primary;
        const auto differs = [&primary](bool mismatch, ErrorCode code, std::string what) {
            if (mismatch) {
                primary.merge(fail(code, "a generation this decision was taken against has changed",
                                   std::move(what)));
            }
        };
        differs(!(facility.control_epoch == plan.control_epoch), ErrorCode::StaleAuthority, "control-epoch");
        differs(!(facility.policy_generation == plan.policy_generation), ErrorCode::StalePolicyGeneration,
                "policy-generation");
        differs(!(facility.dependency_generation == plan.dependency_generation),
                ErrorCode::StaleDependencyGeneration, "dependency-generation");
        differs(!(facility.capacity_generation == plan.capacity_generation), ErrorCode::StaleCapacityGeneration,
                "capacity-generation");
        differs(!(facility.topology_generation == plan.topology_generation), ErrorCode::StaleTopologyGeneration,
                "topology-generation");
        differs(!(facility.maintenance_generation == plan.maintenance_generation),
                ErrorCode::StaleMaintenanceGeneration, "maintenance-generation");
        differs(!(facility.facility_epoch == plan.facility_epoch), ErrorCode::StaleFacilityEpoch, "facility-epoch");
        differs(!(facility.dependency_digest == plan.dependency_digest), ErrorCode::StaleApproval,
                "dependency-digest");
        if (report.plan_revision.is_set() && report.plan_revision != plan.revision) {
            primary.merge(fail(ErrorCode::StaleRevision, "the recorded evaluation belongs to another revision",
                               "plan", plan.id.name()));
        }
        return primary;
    }

    // --- commands ------------------------------------------------------------

    [[nodiscard]] Result<CommandResult> propose(const ProposeRequest& request, std::optional<EngineEvent>& event);
    [[nodiscard]] Result<CommandResult> evaluate(const EvaluateRequest& request, std::optional<EngineEvent>& event);
    [[nodiscard]] Result<CommandResult> approve(const ApproveRequest& request, std::optional<EngineEvent>& event);
    [[nodiscard]] Result<CommandResult> derive_obligations(const DeriveObligationsRequest& request,
                                                           std::optional<EngineEvent>& event);
    [[nodiscard]] Result<CommandResult> ingest_receipt(const IngestReceiptRequest& request,
                                                       std::optional<EngineEvent>& event);
    [[nodiscard]] Result<CommandResult> grant_exception(const GrantExceptionRequest& request,
                                                        std::optional<EngineEvent>& event);
    [[nodiscard]] Result<CommandResult> begin(const BeginRequest& request, std::optional<EngineEvent>& event);
    [[nodiscard]] Result<CommandResult> record_progress(const RecordProgressRequest& request,
                                                        std::optional<EngineEvent>& event);
    [[nodiscard]] Result<CommandResult> verify_restoration(const VerifyRequest& request,
                                                           std::optional<EngineEvent>& event);
    [[nodiscard]] Result<CommandResult> complete(const CompleteRequest& request, std::optional<EngineEvent>& event);
    [[nodiscard]] Result<CommandResult> cancel(const CancelRequest& request, std::optional<EngineEvent>& event);
    [[nodiscard]] Result<CommandResult> recover(const RecoverRequest& request, std::optional<EngineEvent>& event);
    [[nodiscard]] Result<CommandResult> install_facility(const InstallFacilityRequest& request,
                                                         std::optional<EngineEvent>& event);
    [[nodiscard]] Result<CommandResult> compact(const CompactRequest& request, std::optional<EngineEvent>& event);

private:
    Store* store_;
    const Clock* clock_;
    EventSink* sink_{nullptr};
    mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// propose
// ---------------------------------------------------------------------------

Result<CommandResult> Coordinator::Impl::propose(const ProposeRequest& request,
                                                 std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    auto replay = replay_of(request.header, expected);
    if (!replay.ok()) {
        return replay.status();
    }
    if (replay.value().has_value()) {
        return std::move(*replay.value());
    }

    if (request.reason.empty()) {
        return fail(ErrorCode::InvalidArgument, "a plan must state why the maintenance is needed", "plan");
    }
    if (request.requested_by.empty() || !is_valid_identifier(request.requested_by)) {
        return fail(ErrorCode::InvalidArgument, "a plan must name an accountable requester", "plan");
    }
    if (request.scope.empty()) {
        return fail(ErrorCode::InvalidArgument, "a plan must name at least one target", "plan");
    }
    for (const auto& target : request.scope.targets()) {
        if (target.empty()) {
            return fail(ErrorCode::InvalidArgument, "a plan names an empty target reference", "plan");
        }
    }
    if (!is_set(request.window.start) || !is_set(request.window.end)) {
        return fail(ErrorCode::InvalidArgument, "a plan window must state a start and an end", "plan");
    }
    if (request.window.start >= request.window.end) {
        return fail(ErrorCode::InvalidArgument, "a plan window ends before it starts", "plan");
    }

    const LedgerState& ledger = store_->cached_ledger();
    if (!ledger.facility.has_value()) {
        return fail(ErrorCode::MissingGeneration,
                    "no facility model has been installed, so a plan cannot bind generations", "facility");
    }
    const FacilitySnapshot& facility = *ledger.facility;
    for (const auto& target : request.scope.targets()) {
        if (!facility.knows_target(target)) {
            return fail(ErrorCode::UnknownTarget, "the facility model does not contain this target", "plan",
                        target.str());
        }
    }
    if (request.window.duration_nanos() > facility.policy.max_window_duration_nanos) {
        return fail(ErrorCode::InvalidArgument, "the requested window is longer than policy allows", "plan",
                    std::to_string(request.window.duration_nanos()) + " ns");
    }

    PlanId plan_id;
    std::uint64_t next_plan_number = 0;
    if (!request.plan_id.empty()) {
        plan_id = request.plan_id;
    } else {
        auto generated = PlanId::parse("plan-" + padded(ledger.next_plan_number, 6));
        if (!generated.ok()) {
            return generated.status();
        }
        plan_id = generated.value();
        next_plan_number = ledger.next_plan_number + 1U;
    }
    if (ledger.find_plan(plan_id) != nullptr) {
        return fail(ErrorCode::DuplicateIdentity, "a plan with this identity already exists", "plan",
                    plan_id.name());
    }

    MaintenancePlan plan;
    plan.id = plan_id;
    plan.revision = Revision::from_value(1U);
    plan.reason = request.reason;
    plan.requested_by = request.requested_by;
    plan.activity = request.activity;
    plan.priority = request.priority;
    plan.risk = request.risk;
    plan.scope = request.scope;
    plan.scope.canonicalize();
    plan.window = request.window;
    plan.facility_epoch = facility.facility_epoch;
    plan.control_epoch = facility.control_epoch;
    plan.policy_generation = facility.policy_generation;
    plan.dependency_generation = facility.dependency_generation;
    plan.capacity_generation = facility.capacity_generation;
    plan.topology_generation = facility.topology_generation;
    plan.maintenance_generation = facility.maintenance_generation;
    plan.policy_digest = facility.policy.compute_digest();
    plan.dependency_digest = facility.dependency_digest;
    plan.finalize();

    PlanRecord record;
    record.plan = plan;
    record.phase = PlanPhase::Proposed;
    record.created_at = now();
    record.updated_at = record.created_at;
    record.finalize();

    if (auto status = record.validate(); !status.ok()) {
        return fail(ErrorCode::InternalError, "proposed plan record failed its own validation", "plan",
                    std::string(status.render()));
    }

    CommandResult result = base_result();
    Digest outcome;
    Timestamp applied_at = 0;
    auto committed = commit_plan(record, request.header, expected, "propose", next_plan_number, outcome, applied_at);
    if (!committed.ok()) {
        return committed.status();
    }
    result.mutation.intent_digest = expected;
    result.mutation.outcome_digest = outcome;
    result.mutation.sequence = committed.value();
    result.mutation.applied_at = applied_at;
    result.has_plan = true;
    result.plan = refreshed_plan(record);
    result.facility = facility;
    result.notes.push_back("plan recorded; nothing has been approved and no obligation exists yet");
    event = EngineEvent{"proposed", plan_id, result.plan.phase, result.mutation.sequence, outcome, applied_at,
                        request.reason};
    return result;
}

// ---------------------------------------------------------------------------
// evaluate
// ---------------------------------------------------------------------------

Result<CommandResult> Coordinator::Impl::evaluate(const EvaluateRequest& request,
                                                  std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    auto replay = replay_of(request.header, expected);
    if (!replay.ok()) {
        return replay.status();
    }
    if (replay.value().has_value()) {
        return std::move(*replay.value());
    }

    const LedgerState& ledger = store_->cached_ledger();
    const PlanRecord* existing = nullptr;
    if (auto status = require_plan(ledger, request.plan, request.revision, existing); !status.ok()) {
        return status;
    }
    if (!ledger.facility.has_value()) {
        return fail(ErrorCode::MissingGeneration, "no facility model has been installed", "facility");
    }
    const FacilitySnapshot& facility = *ledger.facility;
    if (is_terminal(existing->phase)) {
        return fail(ErrorCode::AlreadyTerminal, "this plan has already reached a terminal phase", "plan",
                    existing->plan.id.name());
    }
    if (!phases_equal(existing->phase, PlanPhase::Proposed) &&
        !phases_equal(existing->phase, PlanPhase::Evaluating) &&
        !phases_equal(existing->phase, PlanPhase::Blocked)) {
        return fail(ErrorCode::IllegalPhase, "evaluation is only meaningful before approval", "plan",
                    std::string(to_string(existing->phase)));
    }

    PlanRecord record = *existing;
    detail::GateContext context;
    context.facility = &facility;
    context.plan = &record.plan;
    context.record = &record;
    context.other_active_plans = active_plans(ledger);
    context.now = now();

    const PreconditionReport report = detail::evaluate_preconditions(context);
    record.evaluation = report;
    record.blockers = report.blocking();
    record.phase = report.satisfied ? PlanPhase::Evaluating : PlanPhase::Blocked;
    record.updated_at = context.now;
    record.finalize();

    if (auto status = record.validate(); !status.ok()) {
        return fail(ErrorCode::InternalError, "evaluated plan record failed its own validation", "plan",
                    std::string(status.render()));
    }

    Digest outcome;
    Timestamp applied_at = 0;
    auto committed = commit_plan(record, request.header, expected, "evaluate", 0, outcome, applied_at);
    if (!committed.ok()) {
        return committed.status();
    }
    CommandResult result = base_result();
    result.mutation.intent_digest = expected;
    result.mutation.outcome_digest = outcome;
    result.mutation.sequence = committed.value();
    result.mutation.applied_at = applied_at;
    result.has_plan = true;
    result.plan = refreshed_plan(record);
    result.evaluation = report;
    result.facility = facility;
    if (!report.satisfied) {
        result.notes.push_back("plan is blocked; every blocking condition is listed in the report");
    }
    event = EngineEvent{report.satisfied ? "evaluated" : "blocked", record.plan.id, record.phase,
                        result.mutation.sequence, outcome, applied_at, {}};
    return result;
}

// ---------------------------------------------------------------------------
// approve
// ---------------------------------------------------------------------------

Result<CommandResult> Coordinator::Impl::approve(const ApproveRequest& request,
                                                 std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    auto replay = replay_of(request.header, expected);
    if (!replay.ok()) {
        return replay.status();
    }
    if (replay.value().has_value()) {
        return std::move(*replay.value());
    }

    const LedgerState& ledger = store_->cached_ledger();
    const PlanRecord* existing = nullptr;
    if (auto status = require_plan(ledger, request.plan, request.revision, existing); !status.ok()) {
        return status;
    }
    if (!ledger.facility.has_value()) {
        return fail(ErrorCode::MissingGeneration, "no facility model has been installed", "facility");
    }
    const FacilitySnapshot& facility = *ledger.facility;
    if (is_terminal(existing->phase)) {
        return fail(ErrorCode::AlreadyTerminal, "this plan has already reached a terminal phase", "plan",
                    existing->plan.id.name());
    }
    if (phases_equal(existing->phase, PlanPhase::Blocked)) {
        if (existing->evaluation.has_value()) {
            Status blocked = detail::primary_blocker(*existing->evaluation);
            if (!blocked.ok()) {
                return blocked;
            }
        }
        return fail(ErrorCode::PlanBlocked, "the plan is blocked and cannot be approved", "plan",
                    existing->plan.id.name());
    }
    if (!phases_equal(existing->phase, PlanPhase::Evaluating)) {
        return fail(ErrorCode::IllegalPhase, "only an evaluated plan can be approved", "plan",
                    std::string(to_string(existing->phase)));
    }
    if (!existing->evaluation.has_value()) {
        return fail(ErrorCode::NotApproved, "the plan carries no evaluation to approve", "plan",
                    existing->plan.id.name());
    }
    if (existing->approval.has_value()) {
        return fail(ErrorCode::IllegalPhase, "this revision is already approved", "plan", existing->plan.id.name());
    }
    if (request.approved_by.empty() || !is_valid_identifier(request.approved_by)) {
        return fail(ErrorCode::InvalidArgument, "an approval must name an accountable approver", "approval");
    }
    if (request.approval_evidence.empty()) {
        return fail(ErrorCode::ApprovalEvidenceMissing, "an approval must cite the evidence it relies on",
                    "approval", request.plan.name());
    }
    if (facility.policy.require_dual_approval && request.witness.empty()) {
        return fail(ErrorCode::ApprovalEvidenceMissing, "policy requires a second approver to witness",
                    "approval", request.plan.name());
    }
    if (auto status = fence_against_facility(facility, existing->plan, *existing->evaluation); !status.ok()) {
        return status;
    }

    PlanRecord record = *existing;
    const Timestamp now = this->now();
    ApprovalRecord approval;
    approval.id = ApprovalId::parse("approval-" + record.plan.id.name() + "-r" +
                                    std::to_string(record.plan.revision.value()))
                      .value();
    approval.plan = record.plan.id;
    approval.plan_revision = record.plan.revision;
    approval.approved_by = request.approved_by;
    approval.approval_evidence = request.approval_evidence;
    approval.witness = request.witness;
    approval.approved_at = now;
    approval.expires_at = now + facility.policy.approval_validity_nanos;
    approval.plan_digest = record.plan.digest;
    approval.facility_digest = record.evaluation->facility_digest;
    approval.policy_digest = record.evaluation->policy_digest;
    approval.dependency_digest = record.evaluation->dependency_digest;
    approval.evaluation_digest = record.evaluation->digest;
    approval.facility_epoch = facility.facility_epoch;
    approval.control_epoch = facility.control_epoch;
    approval.policy_generation = facility.policy_generation;
    approval.dependency_generation = facility.dependency_generation;
    approval.capacity_generation = facility.capacity_generation;
    approval.topology_generation = facility.topology_generation;
    approval.maintenance_generation = facility.maintenance_generation;
    approval.exceptions = record.evaluation->applied_exceptions;
    approval.finalize();

    record.approval = approval;
    record.phase = PlanPhase::Approved;
    record.blockers.clear();
    record.updated_at = now;
    record.finalize();
    if (auto status = record.validate(); !status.ok()) {
        return fail(ErrorCode::InternalError, "approved plan record failed its own validation", "plan",
                    std::string(status.render()));
    }

    Digest outcome;
    Timestamp applied_at = 0;
    auto committed = commit_plan(record, request.header, expected, "approve", 0, outcome, applied_at);
    if (!committed.ok()) {
        return committed.status();
    }
    CommandResult result = base_result();
    result.mutation.intent_digest = expected;
    result.mutation.outcome_digest = outcome;
    result.mutation.sequence = committed.value();
    result.mutation.applied_at = applied_at;
    result.has_plan = true;
    result.plan = refreshed_plan(record);
    result.approval = approval;
    result.evaluation = record.evaluation;
    result.facility = facility;
    result.notes.push_back("approval is bound to this revision and generation set; any change fences it");
    event = EngineEvent{"approved", record.plan.id, record.phase, result.mutation.sequence, outcome, applied_at, {}};
    return result;
}

// ---------------------------------------------------------------------------
// derive-obligations
// ---------------------------------------------------------------------------

Result<CommandResult> Coordinator::Impl::derive_obligations(const DeriveObligationsRequest& request,
                                                            std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    auto replay = replay_of(request.header, expected);
    if (!replay.ok()) {
        return replay.status();
    }
    if (replay.value().has_value()) {
        return std::move(*replay.value());
    }

    const LedgerState& ledger = store_->cached_ledger();
    const PlanRecord* existing = nullptr;
    if (auto status = require_plan(ledger, request.plan, request.revision, existing); !status.ok()) {
        return status;
    }
    if (!ledger.facility.has_value()) {
        return fail(ErrorCode::MissingGeneration, "no facility model has been installed", "facility");
    }
    const FacilitySnapshot& facility = *ledger.facility;
    if (is_terminal(existing->phase)) {
        return fail(ErrorCode::AlreadyTerminal, "this plan has already reached a terminal phase", "plan",
                    existing->plan.id.name());
    }
    if (phases_equal(existing->phase, PlanPhase::Proposed) ||
        phases_equal(existing->phase, PlanPhase::Evaluating) ||
        phases_equal(existing->phase, PlanPhase::Blocked)) {
        return fail(ErrorCode::NotApproved, "obligations cannot be derived before the plan is approved", "plan",
                    existing->plan.id.name());
    }
    if (!phases_equal(existing->phase, PlanPhase::Approved) &&
        !phases_equal(existing->phase, PlanPhase::PreDrain)) {
        return fail(ErrorCode::IllegalPhase, "obligations are derived once, before work starts", "plan",
                    std::string(to_string(existing->phase)));
    }
    if (!existing->approval.has_value() || !existing->evaluation.has_value()) {
        return fail(ErrorCode::NotApproved, "the plan has no approval to derive obligations from", "plan",
                    existing->plan.id.name());
    }
    if (!existing->receipts.empty()) {
        return fail(ErrorCode::IllegalPhase,
                    "obligations cannot be re-derived once evidence has been accepted", "plan",
                    existing->plan.id.name());
    }
    if (auto status = detail::check_approval_fence(*existing->approval, *existing->evaluation, facility);
        !status.ok()) {
        return status;
    }

    PlanRecord record = *existing;
    detail::GateContext context;
    context.facility = &facility;
    context.plan = &record.plan;
    context.record = &record;
    context.now = this->now();
    const ObligationSet obligations = detail::derive_obligations(context);
    if (obligations.obligations.size() > 4096U) {
        return fail(ErrorCode::LimitExceeded, "the derived obligation set is larger than the accepted bound",
                    "plan", std::to_string(obligations.obligations.size()));
    }
    record.obligations = obligations;
    record.approval->obligation_set_digest = obligations.digest;
    record.approval->finalize();
    record.phase = PlanPhase::PreDrain;
    record.updated_at = context.now;
    record.finalize();
    if (auto status = record.validate(); !status.ok()) {
        return fail(ErrorCode::InternalError, "plan record with derived obligations failed its own validation",
                    "plan", std::string(status.render()));
    }

    Digest outcome;
    Timestamp applied_at = 0;
    auto committed = commit_plan(record, request.header, expected, "derive-obligations", 0, outcome, applied_at);
    if (!committed.ok()) {
        return committed.status();
    }
    CommandResult result = base_result();
    result.mutation.intent_digest = expected;
    result.mutation.outcome_digest = outcome;
    result.mutation.sequence = committed.value();
    result.mutation.applied_at = applied_at;
    result.has_plan = true;
    result.plan = refreshed_plan(record);
    result.approval = record.approval;
    result.obligations = detail::derive_obligation_status(record, context.now);
    result.facility = facility;
    result.notes.push_back("obligations are requests, not effects: each one needs observed evidence");
    event = EngineEvent{"obligations-derived", record.plan.id, record.phase, result.mutation.sequence, outcome,
                        applied_at, {}};
    return result;
}

// ---------------------------------------------------------------------------
// ingest-receipt
// ---------------------------------------------------------------------------

Result<CommandResult> Coordinator::Impl::ingest_receipt(const IngestReceiptRequest& request,
                                                        std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    auto replay = replay_of(request.header, expected);
    if (!replay.ok()) {
        return replay.status();
    }
    if (replay.value().has_value()) {
        return std::move(*replay.value());
    }

    const LedgerState& ledger = store_->cached_ledger();
    const PlanRecord* existing = nullptr;
    if (auto status = require_plan(ledger, request.plan, request.revision, existing); !status.ok()) {
        return status;
    }
    if (is_terminal(existing->phase)) {
        return fail(ErrorCode::AlreadyTerminal, "this plan has already reached a terminal phase", "plan",
                    existing->plan.id.name());
    }
    if (phases_equal(existing->phase, PlanPhase::Proposed) ||
        phases_equal(existing->phase, PlanPhase::Evaluating) ||
        phases_equal(existing->phase, PlanPhase::Blocked) ||
        phases_equal(existing->phase, PlanPhase::Approved)) {
        return fail(ErrorCode::IllegalPhase, "no obligation exists yet, so no evidence can be accepted", "plan",
                    std::string(to_string(existing->phase)));
    }
    if (request.receipt_id.empty()) {
        return fail(ErrorCode::InvalidArgument, "a receipt must have an identity", "receipt");
    }
    if (request.obligation.empty()) {
        return fail(ErrorCode::InvalidArgument, "a receipt must name the obligation it answers", "receipt",
                    request.receipt_id.name());
    }
    if (request.issuer.empty() || !is_valid_identifier(request.issuer)) {
        return fail(ErrorCode::InvalidArgument, "a receipt must name the authority that issued it", "receipt",
                    request.receipt_id.name());
    }
    if (request.evidence.empty()) {
        return fail(ErrorCode::InvalidArgument, "a receipt must carry provenance", "receipt",
                    request.receipt_id.name());
    }
    if (!is_set(request.observed_at)) {
        return fail(ErrorCode::InvalidArgument, "a receipt must state when the fact was observed", "receipt",
                    request.receipt_id.name());
    }
    if (!request.observation_sequence.is_set()) {
        return fail(ErrorCode::MissingGeneration, "a receipt must carry an observation sequence", "receipt",
                    request.receipt_id.name());
    }

    const Obligation* obligation = existing->obligations.find(request.obligation);
    if (obligation == nullptr) {
        return fail(ErrorCode::UnknownObligation, "this plan has no such obligation", "obligation",
                    request.obligation.name());
    }
    if (const Receipt* duplicate = existing->find_receipt(request.receipt_id); duplicate != nullptr) {
        const bool identical = duplicate->obligation == request.obligation && duplicate->kind == request.kind &&
                               duplicate->evidence == request.evidence &&
                               duplicate->observation_sequence == request.observation_sequence &&
                               duplicate->issuer == request.issuer;
        if (identical) {
            return fail(ErrorCode::DuplicateOperation, "this receipt has already been recorded", "receipt",
                        request.receipt_id.name());
        }
        return fail(ErrorCode::DuplicateIdentity,
                    "a different receipt already uses this identity", "receipt", request.receipt_id.name());
    }
    if (!detail::receipt_matches_obligation(obligation->kind, request.kind)) {
        return fail(ErrorCode::InvalidArgument, "the receipt kind does not answer this obligation", "receipt",
                    std::string(to_string(request.kind)) + " for " + std::string(to_string(obligation->kind)));
    }
    if (request.issuer_authority != obligation->authority) {
        return fail(ErrorCode::IdentityMismatch, "the receipt was issued by an authority that does not own this "
                                                 "obligation",
                    "receipt",
                    std::string(to_string(request.issuer_authority)) + " instead of " +
                        std::string(to_string(obligation->authority)));
    }
    if (existing->last_observation_sequence.is_set() &&
        !(existing->last_observation_sequence < request.observation_sequence)) {
        return fail(ErrorCode::StaleObservation,
                    "the observation sequence is not fresher than the evidence already recorded", "receipt",
                    "recorded " + std::to_string(existing->last_observation_sequence.value()) + ", supplied " +
                        std::to_string(request.observation_sequence.value()));
    }

    PlanRecord record = *existing;
    const Timestamp now = this->now();
    Receipt receipt;
    receipt.id = request.receipt_id;
    receipt.obligation = request.obligation;
    receipt.kind = request.kind;
    receipt.issuer_authority = request.issuer_authority;
    receipt.issuer = request.issuer;
    receipt.observation_sequence = request.observation_sequence;
    receipt.plan_revision = record.plan.revision;
    receipt.obligation_digest = record.obligations.digest;
    receipt.evidence = request.evidence;
    receipt.evidence_digest = detail::evidence_digest_of(request.evidence);
    receipt.observed_at = request.observed_at;
    receipt.ingested_at = now;
    receipt.finalize();
    if (auto status = receipt.validate(); !status.ok()) {
        return fail(ErrorCode::InternalError, "receipt failed its own validation", "receipt",
                    std::string(status.render()));
    }

    record.receipts.push_back(receipt);
    record.last_observation_sequence = request.observation_sequence;
    if (record.recovery_required && request.kind == ReceiptKind::WorkStopObserved && record.recovery_mark.is_set() &&
        record.recovery_mark < request.observation_sequence) {
        // Fresh evidence, newer than the state that was recovered, is the only
        // thing that clears a recovery obligation.
        record.recovery_required = false;
    }
    record.updated_at = now;
    record.finalize();

    const ObligationStatusReport statuses = detail::derive_obligation_status(record, now);
    if (phases_equal(record.phase, PlanPhase::PreDrain) && statuses.ready_for_isolation) {
        record.phase = PlanPhase::Ready;
    }
    record.finalize();
    if (auto status = record.validate(); !status.ok()) {
        return fail(ErrorCode::InternalError, "plan record with the new receipt failed its own validation", "plan",
                    std::string(status.render()));
    }

    Digest outcome;
    Timestamp applied_at = 0;
    auto committed = commit_plan(record, request.header, expected, "ingest-receipt", 0, outcome, applied_at);
    if (!committed.ok()) {
        return committed.status();
    }
    CommandResult result = base_result();
    result.mutation.intent_digest = expected;
    result.mutation.outcome_digest = outcome;
    result.mutation.sequence = committed.value();
    result.mutation.applied_at = applied_at;
    result.has_plan = true;
    result.plan = refreshed_plan(record);
    result.receipt = receipt;
    result.obligations = detail::derive_obligation_status(record, now);
    if (ledger.facility.has_value()) {
        result.facility = ledger.facility;
    }
    event = EngineEvent{"receipt-ingested", record.plan.id, record.phase, result.mutation.sequence, outcome,
                        applied_at, std::string(to_string(receipt.kind))};
    return result;
}

// ---------------------------------------------------------------------------
// grant-exception
// ---------------------------------------------------------------------------

Result<CommandResult> Coordinator::Impl::grant_exception(const GrantExceptionRequest& request,
                                                         std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    auto replay = replay_of(request.header, expected);
    if (!replay.ok()) {
        return replay.status();
    }
    if (replay.value().has_value()) {
        return std::move(*replay.value());
    }

    const LedgerState& ledger = store_->cached_ledger();
    const PlanRecord* existing = nullptr;
    if (auto status = require_plan(ledger, request.plan, request.revision, existing); !status.ok()) {
        return status;
    }
    if (!ledger.facility.has_value()) {
        return fail(ErrorCode::MissingGeneration, "no facility model has been installed", "facility");
    }
    const FacilitySnapshot& facility = *ledger.facility;
    if (is_terminal(existing->phase)) {
        return fail(ErrorCode::AlreadyTerminal, "this plan has already reached a terminal phase", "plan",
                    existing->plan.id.name());
    }
    if (!phases_equal(existing->phase, PlanPhase::Proposed) &&
        !phases_equal(existing->phase, PlanPhase::Evaluating) &&
        !phases_equal(existing->phase, PlanPhase::Blocked)) {
        return fail(ErrorCode::IllegalPhase,
                    "an exception changes the evaluation, so it can only be granted before approval", "plan",
                    std::string(to_string(existing->phase)));
    }
    if (request.waived.empty()) {
        return fail(ErrorCode::InvalidArgument, "an exception must name the conditions it waives", "exception");
    }
    if (request.validity_nanos < 0) {
        return fail(ErrorCode::ExceptionInvalid, "an exception must not carry a negative validity", "exception",
                    std::to_string(request.validity_nanos));
    }
    for (const auto code : request.waived) {
        if (!is_waivable_condition(code)) {
            return fail(ErrorCode::HardInterlock,
                        "hard safety interlocks are never waivable, by exception or otherwise", "exception",
                        std::string(to_string(code)));
        }
    }
    if (request.targets.empty()) {
        return fail(ErrorCode::InvalidArgument, "an exception must name the targets it covers", "exception");
    }
    if (request.granted_by.empty() || !is_valid_identifier(request.granted_by)) {
        return fail(ErrorCode::InvalidArgument, "an exception must name an accountable grantor", "exception");
    }
    if (request.justification.empty()) {
        return fail(ErrorCode::InvalidArgument, "an exception must state a justification", "exception");
    }
    for (const auto& target : request.targets) {
        const bool covered = std::any_of(existing->plan.scope.targets().begin(), existing->plan.scope.targets().end(),
                                         [&facility, &target](const TargetRef& scoped) {
                                             return facility.scope_conflicts(MaintenanceScope({scoped}),
                                                                             MaintenanceScope({target}));
                                         });
        if (!covered) {
            return fail(ErrorCode::ScopeViolation, "an exception may not reach outside the plan scope",
                        "exception", target.str());
        }
    }

    PlanRecord record = *existing;
    const Timestamp now = this->now();
    ExceptionGrant grant;
    grant.id = ExceptionId::parse("exception-" + record.plan.id.name() + "-" +
                                  std::to_string(record.exceptions.size() + 1U))
                   .value();
    grant.plan = record.plan.id;
    grant.waived = request.waived;
    grant.targets = request.targets;
    grant.granted_by = request.granted_by;
    grant.justification = request.justification;
    grant.granted_at = now;
    grant.expires_at = now + (request.validity_nanos > 0 ? request.validity_nanos
                                                         : facility.policy.approval_validity_nanos);
    grant.finalize();
    if (auto status = grant.validate(); !status.ok()) {
        return status;
    }
    record.exceptions.push_back(grant);
    record.updated_at = now;
    record.finalize();
    if (auto status = record.validate(); !status.ok()) {
        return fail(ErrorCode::InternalError, "plan record with the new exception failed its own validation",
                    "plan", std::string(status.render()));
    }

    Digest outcome;
    Timestamp applied_at = 0;
    auto committed = commit_plan(record, request.header, expected, "grant-exception", 0, outcome, applied_at);
    if (!committed.ok()) {
        return committed.status();
    }
    CommandResult result = base_result();
    result.mutation.intent_digest = expected;
    result.mutation.outcome_digest = outcome;
    result.mutation.sequence = committed.value();
    result.mutation.applied_at = applied_at;
    result.has_plan = true;
    result.plan = refreshed_plan(record);
    result.exception = grant;
    result.facility = facility;
    result.notes.push_back("the plan must be re-evaluated for the exception to take effect");
    event = EngineEvent{"exception-granted", record.plan.id, record.phase, result.mutation.sequence, outcome,
                        applied_at, grant.justification};
    return result;
}

// ---------------------------------------------------------------------------
// begin, progress, verification, completion, cancellation
// ---------------------------------------------------------------------------

Result<CommandResult> Coordinator::Impl::begin(const BeginRequest& request, std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    auto replay = replay_of(request.header, expected);
    if (!replay.ok()) {
        return replay.status();
    }
    if (replay.value().has_value()) {
        return std::move(*replay.value());
    }

    const LedgerState& ledger = store_->cached_ledger();
    const PlanRecord* existing = nullptr;
    if (auto status = require_plan(ledger, request.plan, request.revision, existing); !status.ok()) {
        return status;
    }
    if (!ledger.facility.has_value()) {
        return fail(ErrorCode::MissingGeneration, "no facility model has been installed", "facility");
    }
    const FacilitySnapshot& facility = *ledger.facility;
    if (is_terminal(existing->phase)) {
        return fail(ErrorCode::AlreadyTerminal, "this plan has already reached a terminal phase", "plan",
                    existing->plan.id.name());
    }
    if (existing->recovery_required) {
        return fail(ErrorCode::RecoveryRequired,
                    "this window was interrupted; it must be recovered and fresh stop evidence accepted", "plan",
                    existing->plan.id.name());
    }
    const Timestamp now = this->now();

    // Outstanding obligations are reported before the phase gate: "drain
    // requested is not drained" is a more useful answer than "not ready", and
    // it is the same answer for every phase in which obligations exist.
    const bool have_obligations = !existing->obligations.obligations.empty();
    const ObligationStatusReport statuses =
        have_obligations ? detail::derive_obligation_status(*existing, now) : ObligationStatusReport{};
    if (have_obligations && !statuses.ready_for_isolation) {
        std::string outstanding;
        for (const auto& status : statuses.statuses) {
            if (status.stage == ObligationStage::Completion) {
                continue;
            }
            if (status.disposition == Disposition::Satisfied || status.disposition == Disposition::Waived ||
                status.disposition == Disposition::NotApplicable) {
                continue;
            }
            if (!outstanding.empty()) {
                outstanding += ", ";
            }
            outstanding += status.id.name();
        }
        return fail(ErrorCode::ObligationsOutstanding,
                    "pre-drain and isolation obligations are still outstanding", "plan", outstanding);
    }
    if (!phases_equal(existing->phase, PlanPhase::Ready)) {
        return fail(ErrorCode::NotReady, "work may only begin from a ready plan", "plan",
                    std::string(to_string(existing->phase)));
    }
    if (!existing->approval.has_value() || !existing->evaluation.has_value()) {
        return fail(ErrorCode::NotApproved, "the plan carries no approval", "plan", existing->plan.id.name());
    }
    if (auto status = detail::check_approval_fence(*existing->approval, *existing->evaluation, facility);
        !status.ok()) {
        return status;
    }
    if (existing->approval->expired_at(now)) {
        return fail(ErrorCode::StaleApproval, "the approval has expired and must be granted again", "plan",
                    existing->plan.id.name());
    }
    if (now < existing->plan.window.start) {
        return fail(ErrorCode::WindowNotOpen, "the requested window has not opened", "plan",
                    std::to_string(existing->plan.window.start));
    }
    if (now > existing->plan.window.end) {
        return fail(ErrorCode::WindowExpired, "the requested window has closed", "plan",
                    std::to_string(existing->plan.window.end));
    }

    PlanRecord record = *existing;
    detail::GateContext context;
    context.facility = &facility;
    context.plan = &record.plan;
    context.record = &record;
    context.other_active_plans = active_plans(ledger);
    context.now = now;
    const PreconditionReport fresh = detail::evaluate_preconditions(context);
    if (!fresh.satisfied) {
        return detail::primary_blocker(fresh);
    }
    // The recorded evaluation is deliberately left untouched: it is the document
    // the approval is bound to by digest, and overwriting it here would break
    // that binding when completion re-checks the fence.
    record.blockers = fresh.blocking();

    record.phase = PlanPhase::InProgress;
    ProgressEvent started;
    started.kind = ProgressKind::Started;
    started.note = "maintenance began";
    started.actor = request.header.actor;
    started.at = now;
    record.progress.push_back(started);
    record.updated_at = now;
    record.finalize();
    if (auto status = record.validate(); !status.ok()) {
        return fail(ErrorCode::InternalError, "plan record failed its own validation at begin", "plan",
                    std::string(status.render()));
    }

    Digest outcome;
    Timestamp applied_at = 0;
    auto committed = commit_plan(record, request.header, expected, "begin", 0, outcome, applied_at);
    if (!committed.ok()) {
        return committed.status();
    }
    CommandResult result = base_result();
    result.mutation.intent_digest = expected;
    result.mutation.outcome_digest = outcome;
    result.mutation.sequence = committed.value();
    result.mutation.applied_at = applied_at;
    result.has_plan = true;
    result.plan = refreshed_plan(record);
    result.evaluation = fresh;
    result.obligations = statuses;
    result.facility = facility;
    event = EngineEvent{"begun", record.plan.id, record.phase, result.mutation.sequence, outcome, applied_at, {}};
    return result;
}
Result<CommandResult> Coordinator::Impl::record_progress(const RecordProgressRequest& request,
                                                         std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    auto replay = replay_of(request.header, expected);
    if (!replay.ok()) {
        return replay.status();
    }
    if (replay.value().has_value()) {
        return std::move(*replay.value());
    }

    const LedgerState& ledger = store_->cached_ledger();
    const PlanRecord* existing = nullptr;
    if (auto status = require_plan(ledger, request.plan, request.revision, existing); !status.ok()) {
        return status;
    }
    if (is_terminal(existing->phase)) {
        return fail(ErrorCode::AlreadyTerminal, "this plan has already reached a terminal phase", "plan",
                    existing->plan.id.name());
    }
    if (!phases_equal(existing->phase, PlanPhase::InProgress)) {
        return fail(ErrorCode::IllegalPhase, "progress is only recorded while work is in progress", "plan",
                    std::string(to_string(existing->phase)));
    }

    PlanRecord record = *existing;
    const Timestamp now = this->now();
    // Progress is a forward-only record: a clock that has gone backwards must
    // not be able to append an event that predates the last one.
    for (const auto& recorded : record.progress) {
        if (recorded.at > now) {
            return fail(ErrorCode::ProgressOutOfOrder,
                        "this progress event predates progress already recorded for the plan", "plan",
                        record.plan.id.name());
        }
    }
    ProgressEvent progress;
    progress.kind = request.kind;
    progress.note = request.note;
    progress.actor = request.header.actor;
    progress.at = now;
    record.progress.push_back(progress);
    if (request.kind == ProgressKind::WorkCompleted) {
        record.phase = PlanPhase::Verification;
    } else if (request.kind == ProgressKind::WorkStopped) {
        record.phase = PlanPhase::Restore;
    }
    record.updated_at = now;
    record.finalize();
    if (auto status = record.validate(); !status.ok()) {
        return fail(ErrorCode::InternalError, "plan record failed its own validation on progress", "plan",
                    std::string(status.render()));
    }
    Digest outcome;
    Timestamp applied_at = 0;
    auto committed = commit_plan(record, request.header, expected, "record-progress", 0, outcome, applied_at);
    if (!committed.ok()) {
        return committed.status();
    }
    CommandResult result = base_result();
    result.mutation.intent_digest = expected;
    result.mutation.outcome_digest = outcome;
    result.mutation.sequence = committed.value();
    result.mutation.applied_at = applied_at;
    result.has_plan = true;
    result.plan = refreshed_plan(record);
    result.obligations = detail::derive_obligation_status(record, now);
    if (ledger.facility.has_value()) {
        result.facility = ledger.facility;
    }
    event = EngineEvent{"progress", record.plan.id, record.phase, result.mutation.sequence, outcome, applied_at,
                        request.note};
    return result;
}

Result<CommandResult> Coordinator::Impl::verify_restoration(const VerifyRequest& request,
                                                            std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    auto replay = replay_of(request.header, expected);
    if (!replay.ok()) {
        return replay.status();
    }
    if (replay.value().has_value()) {
        return std::move(*replay.value());
    }

    const LedgerState& ledger = store_->cached_ledger();
    const PlanRecord* existing = nullptr;
    if (auto status = require_plan(ledger, request.plan, request.revision, existing); !status.ok()) {
        return status;
    }
    if (!ledger.facility.has_value()) {
        return fail(ErrorCode::MissingGeneration, "no facility model has been installed", "facility");
    }
    const FacilitySnapshot& facility = *ledger.facility;
    if (is_terminal(existing->phase)) {
        return fail(ErrorCode::AlreadyTerminal, "this plan has already reached a terminal phase", "plan",
                    existing->plan.id.name());
    }
    if (!phases_equal(existing->phase, PlanPhase::Verification)) {
        return fail(ErrorCode::IllegalPhase, "restoration is verified after the work is reported complete",
                    "plan", std::string(to_string(existing->phase)));
    }

    PlanRecord record = *existing;
    const Timestamp now = this->now();
    const ObligationStatusReport statuses = detail::derive_obligation_status(record, now);
    const ObligationStatus* maintenance = nullptr;
    for (const auto& status : statuses.statuses) {
        if (status.kind == ObligationKind::MaintenanceVerified) {
            maintenance = &status;
            break;
        }
    }
    if (maintenance == nullptr || (maintenance->disposition != Disposition::Satisfied &&
                                  maintenance->disposition != Disposition::NotApplicable)) {
        return fail(ErrorCode::CompletionEvidenceMissing,
                    "the completed work has not been verified against its change record", "plan",
                    record.plan.id.name());
    }

    RestorationReport report;
    report.plan = record.plan.id;
    report.plan_revision = record.plan.revision;
    report.verified_at = now;
    report.outstanding = statuses.outstanding(ObligationStage::Completion);
    report.violated_protections = detail::violated_protections(facility, record.plan);
    report.redundancy_below_requirement = detail::redundancy_below_requirement(facility, record.plan);
    report.restored = report.outstanding.empty() && report.violated_protections.empty() &&
                      report.redundancy_below_requirement.empty();
    report.finalize();

    Status blocker;
    if (!report.outstanding.empty()) {
        std::string list;
        for (const auto& id : report.outstanding) {
            if (!list.empty()) {
                list += ", ";
            }
            list += id.name();
        }
        blocker.merge(fail(ErrorCode::RestorationOutstanding,
                           "restoration obligations are still outstanding", "plan", list));
    }
    for (const auto& protection : report.violated_protections) {
        blocker.merge(fail(ErrorCode::ProtectedObligationViolated,
                           "a protected obligation is still violated after the work", "protected-obligation",
                           protection.name()));
    }
    for (const auto& group : report.redundancy_below_requirement) {
        blocker.merge(fail(ErrorCode::RedundancyInsufficient,
                           "redundancy has not been restored to its protected level", "redundancy-group",
                           group.name()));
    }
    if (!report.restored) {
        return blocker;
    }

    record.phase = PlanPhase::Restore;
    record.updated_at = now;
    record.finalize();
    if (auto status = record.validate(); !status.ok()) {
        return fail(ErrorCode::InternalError, "plan record failed its own validation on verification", "plan",
                    std::string(status.render()));
    }

    Digest outcome;
    Timestamp applied_at = 0;
    auto committed = commit_plan(record, request.header, expected, "verify-restoration", 0, outcome, applied_at);
    if (!committed.ok()) {
        return committed.status();
    }
    CommandResult result = base_result();
    result.mutation.intent_digest = expected;
    result.mutation.outcome_digest = outcome;
    result.mutation.sequence = committed.value();
    result.mutation.applied_at = applied_at;
    result.has_plan = true;
    result.plan = refreshed_plan(record);
    result.restoration = report;
    result.obligations = statuses;
    result.facility = facility;
    event = EngineEvent{"restoration-verified", record.plan.id, record.phase, result.mutation.sequence, outcome,
                        applied_at, {}};
    return result;
}
Result<CommandResult> Coordinator::Impl::complete(const CompleteRequest& request,
                                                  std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    auto replay = replay_of(request.header, expected);
    if (!replay.ok()) {
        return replay.status();
    }
    if (replay.value().has_value()) {
        return std::move(*replay.value());
    }

    const LedgerState& ledger = store_->cached_ledger();
    const PlanRecord* existing = nullptr;
    if (auto status = require_plan(ledger, request.plan, request.revision, existing); !status.ok()) {
        return status;
    }
    if (!ledger.facility.has_value()) {
        return fail(ErrorCode::MissingGeneration, "no facility model has been installed", "facility");
    }
    const FacilitySnapshot& facility = *ledger.facility;
    if (is_terminal(existing->phase)) {
        return fail(ErrorCode::AlreadyTerminal, "this plan has already reached a terminal phase", "plan",
                    existing->plan.id.name());
    }
    if (existing->recovery_required) {
        return fail(ErrorCode::RecoveryRequired,
                    "this window was interrupted and has not been recovered with fresh evidence", "plan",
                    existing->plan.id.name());
    }
    if (!phases_equal(existing->phase, PlanPhase::Restore)) {
        return fail(ErrorCode::IllegalPhase, "completion follows a verified restoration", "plan",
                    std::string(to_string(existing->phase)));
    }
    if (!existing->approval.has_value() || !existing->evaluation.has_value()) {
        return fail(ErrorCode::NotApproved, "the plan carries no approval", "plan", existing->plan.id.name());
    }
    if (auto status = detail::check_approval_fence(*existing->approval, *existing->evaluation, facility);
        !status.ok()) {
        return status;
    }

    PlanRecord record = *existing;
    const Timestamp now = this->now();
    if (existing->approval->expired_at(now)) {
        return fail(ErrorCode::StaleApproval, "the approval expired before completion; grant a new one", "plan",
                    record.plan.id.name());
    }
    const ObligationStatusReport statuses = detail::derive_obligation_status(record, now);
    if (!statuses.ready_for_completion) {
        std::string outstanding;
        for (const auto& status : statuses.statuses) {
            if (status.disposition == Disposition::Satisfied || status.disposition == Disposition::Waived ||
                status.disposition == Disposition::NotApplicable) {
                continue;
            }
            if (!outstanding.empty()) {
                outstanding += ", ";
            }
            outstanding += status.id.name();
        }
        return fail(ErrorCode::ObligationsOutstanding,
                    "completion evidence is missing: obligations remain outstanding", "plan", outstanding);
    }
    const std::vector<ProtectionId> protections = detail::violated_protections(facility, record.plan);
    if (!protections.empty()) {
        return fail(ErrorCode::ProtectedObligationViolated,
                    "a protected obligation is still violated, so the window cannot complete", "plan",
                    record.plan.id.name());
    }

    CompletionReport report;
    report.plan = record.plan.id;
    report.plan_revision = record.plan.revision;
    report.completed_at = now;
    report.plan_digest = record.plan.digest;
    report.approval_digest = record.approval->digest;
    report.obligation_set_digest = record.obligations.digest;
    report.receipt_set_digest = detail::receipt_set_digest(record.receipts);
    report.facility_digest = facility.digest;
    report.policy_digest = facility.policy.compute_digest();
    report.dependency_digest = facility.dependency_digest;
    report.protected_obligations_satisfied = true;
    report.restoration_verified = true;
    for (const auto& status : statuses.statuses) {
        report.satisfied.push_back(status.id);
    }

    record.phase = PlanPhase::Complete;
    record.updated_at = now;
    record.finalize();
    if (auto status = record.validate(); !status.ok()) {
        return fail(ErrorCode::InternalError, "completed plan record failed its own validation", "plan",
                    std::string(status.render()));
    }

    Digest outcome;
    Timestamp applied_at = 0;
    auto committed = commit_plan(record, request.header, expected, "complete", 0, outcome, applied_at);
    if (!committed.ok()) {
        return committed.status();
    }
    CommandResult result = base_result();
    result.mutation.intent_digest = expected;
    result.mutation.outcome_digest = outcome;
    result.mutation.sequence = committed.value();
    result.mutation.applied_at = applied_at;
    result.has_plan = true;
    result.plan = refreshed_plan(record);
    report.sequence = result.mutation.sequence;
    report.finalize();
    result.completion = report;
    result.obligations = statuses;
    result.facility = facility;
    result.notes.push_back("completion binds the plan, approval, obligation set and receipt set by digest");
    event = EngineEvent{"completed", record.plan.id, record.phase, result.mutation.sequence, outcome, applied_at,
                        request.note};
    return result;
}

Result<CommandResult> Coordinator::Impl::cancel(const CancelRequest& request, std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    auto replay = replay_of(request.header, expected);
    if (!replay.ok()) {
        return replay.status();
    }
    if (replay.value().has_value()) {
        return std::move(*replay.value());
    }

    const LedgerState& ledger = store_->cached_ledger();
    const PlanRecord* existing = nullptr;
    if (auto status = require_plan(ledger, request.plan, request.revision, existing); !status.ok()) {
        return status;
    }
    if (is_terminal(existing->phase)) {
        return fail(ErrorCode::AlreadyTerminal, "this plan has already reached a terminal phase", "plan",
                    existing->plan.id.name());
    }
    const bool before_work = phases_equal(existing->phase, PlanPhase::Proposed) ||
                             phases_equal(existing->phase, PlanPhase::Evaluating) ||
                             phases_equal(existing->phase, PlanPhase::Blocked) ||
                             phases_equal(existing->phase, PlanPhase::Approved) ||
                             phases_equal(existing->phase, PlanPhase::PreDrain) ||
                             phases_equal(existing->phase, PlanPhase::Ready);
    if (!before_work && !phases_equal(existing->phase, PlanPhase::Restore)) {
        return fail(ErrorCode::IllegalPhase,
                    "work has started; record the stop and verify restoration before cancelling", "plan",
                    std::string(to_string(existing->phase)));
    }

    PlanRecord record = *existing;
    const Timestamp now = this->now();
    if (phases_equal(existing->phase, PlanPhase::Restore)) {
        // Cancelling a window that already touched the plant is only allowed
        // once the facility is demonstrably back in its protected posture.
        if (!ledger.facility.has_value()) {
            return fail(ErrorCode::MissingGeneration, "no facility model has been installed", "facility");
        }
        const FacilitySnapshot& facility = *ledger.facility;
        const ObligationStatusReport statuses = detail::derive_obligation_status(record, now);
        if (!statuses.ready_for_completion) {
            return fail(ErrorCode::RestorationOutstanding,
                        "the facility has not been restored, so this window cannot be cancelled", "plan",
                        record.plan.id.name());
        }
        if (!detail::violated_protections(facility, record.plan).empty()) {
            return fail(ErrorCode::ProtectedObligationViolated,
                        "a protected obligation is still violated, so the window cannot be cancelled", "plan",
                        record.plan.id.name());
        }
    }

    record.phase = PlanPhase::Cancelled;
    record.updated_at = now;
    ProgressEvent cancelled;
    cancelled.kind = ProgressKind::WorkStopped;
    cancelled.note = request.reason.empty() ? "window cancelled" : request.reason;
    cancelled.actor = request.header.actor;
    cancelled.at = now;
    record.progress.push_back(cancelled);
    record.finalize();
    if (auto status = record.validate(); !status.ok()) {
        return fail(ErrorCode::InternalError, "cancelled plan record failed its own validation", "plan",
                    std::string(status.render()));
    }
    Digest outcome;
    Timestamp applied_at = 0;
    auto committed = commit_plan(record, request.header, expected, "cancel", 0, outcome, applied_at);
    if (!committed.ok()) {
        return committed.status();
    }
    CommandResult result = base_result();
    result.mutation.intent_digest = expected;
    result.mutation.outcome_digest = outcome;
    result.mutation.sequence = committed.value();
    result.mutation.applied_at = applied_at;
    result.has_plan = true;
    result.plan = refreshed_plan(record);
    result.obligations = detail::derive_obligation_status(record, now);
    if (ledger.facility.has_value()) {
        result.facility = ledger.facility;
    }
    event = EngineEvent{"cancelled", record.plan.id, record.phase, result.mutation.sequence, outcome, applied_at,
                        request.reason};
    return result;
}
Result<CommandResult> Coordinator::Impl::recover(const RecoverRequest& request, std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    auto replay = replay_of(request.header, expected);
    if (!replay.ok()) {
        return replay.status();
    }
    if (replay.value().has_value()) {
        return std::move(*replay.value());
    }

    const LedgerState& ledger = store_->cached_ledger();
    const PlanRecord* target = nullptr;
    if (!request.plan.empty()) {
        // Recovery deliberately does not name a revision: it is the operation
        // that decides what an interrupted revision now needs.
        target = ledger.find_plan(request.plan);
        if (target == nullptr) {
            return fail(ErrorCode::UnknownPlan, "no such plan", "plan", request.plan.name());
        }
    } else {
        std::size_t others = 0;
        for (const auto& entry : ledger.plans) {
            if (!is_started(entry.second.phase)) {
                continue;
            }
            if (target == nullptr) {
                target = &entry.second;
            } else {
                ++others;
            }
        }
        if (target == nullptr) {
            CommandResult result = base_result();
            result.mutation.intent_digest = expected;
            result.mutation.sequence = ledger.sequence;
            result.mutation.outcome_digest = detail::ledger_outcome_digest("recover", ledger);
            result.mutation.applied_at = this->now();
            result.notes.push_back("no plan requires recovery");
            if (ledger.facility.has_value()) {
                result.facility = ledger.facility;
            }
            return result;
        }
        if (others > 0) {
            // More than one plan is in flight; recovery is applied to one plan
            // per command, and the caller repeats with an explicit identity.
        }
    }
    if (is_terminal(target->phase) || !is_started(target->phase)) {
        CommandResult result = base_result();
        result.mutation.intent_digest = expected;
        result.mutation.sequence = ledger.sequence;
        result.mutation.outcome_digest = detail::ledger_outcome_digest("recover", ledger);
        result.mutation.applied_at = this->now();
        result.has_plan = true;
        result.plan = *target;
        result.notes.push_back("this plan is not in flight, so no recovery is required");
        result.obligations = detail::derive_obligation_status(*target, this->now());
        if (ledger.facility.has_value()) {
            result.facility = ledger.facility;
        }
        return result;
    }

    const Timestamp now = this->now();
    PlanRecord record = *target;
    RecoveryReport report;
    report.plan = record.plan.id;
    report.phase_before = record.phase;
    report.prior_incarnation = store_->recovery().prior_incarnation;
    report.current_incarnation = ledger.incarnation;
    report.recovered_at = now;

    if (phases_equal(record.phase, PlanPhase::InProgress)) {
        record.phase = PlanPhase::Restore;
        report.notes.push_back("work was in flight; the window is forced into restoration");
    } else {
        report.notes.push_back("verification or restoration was in flight; outstanding evidence is re-checked");
    }
    record.recovery_required = true;
    record.recovery_mark = record.last_observation_sequence;
    record.updated_at = now;
    record.finalize();
    const ObligationStatusReport statuses = detail::derive_obligation_status(record, now);
    report.outstanding = statuses.outstanding(ObligationStage::Completion);
    report.phase_after = record.phase;
    report.recovery_required_after = record.recovery_required;
    report.finalize();

    if (auto status = record.validate(); !status.ok()) {
        return fail(ErrorCode::InternalError, "recovered plan record failed its own validation", "plan",
                    std::string(status.render()));
    }
    Digest outcome;
    Timestamp applied_at = 0;
    auto committed = commit_plan(record, request.header, expected, "recover", 0, outcome, applied_at);
    if (!committed.ok()) {
        return committed.status();
    }
    CommandResult result = base_result();
    result.mutation.intent_digest = expected;
    result.mutation.outcome_digest = outcome;
    result.mutation.sequence = committed.value();
    result.mutation.applied_at = applied_at;
    result.has_plan = true;
    result.plan = refreshed_plan(record);
    result.recovery = report;
    result.obligations = statuses;
    if (ledger.facility.has_value()) {
        result.facility = ledger.facility;
    }
    event = EngineEvent{"recovered", record.plan.id, record.phase, result.mutation.sequence, outcome, applied_at,
                        "interrupted window recovered"};
    return result;
}

// ---------------------------------------------------------------------------
// install-facility and compact
// ---------------------------------------------------------------------------

Result<CommandResult> Coordinator::Impl::install_facility(const InstallFacilityRequest& request,
                                                          std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    auto replay = replay_of(request.header, expected);
    if (!replay.ok()) {
        return replay.status();
    }
    if (replay.value().has_value()) {
        return std::move(*replay.value());
    }

    FacilitySnapshot snapshot = request.facility;
    snapshot.finalize();
    if (auto status = snapshot.validate(); !status.ok()) {
        return status;
    }
    const LedgerState& ledger = store_->cached_ledger();
    if (ledger.facility.has_value()) {
        const FacilitySnapshot& current = *ledger.facility;
        if (!(current.facility_epoch < snapshot.facility_epoch)) {
            return fail(ErrorCode::StaleFacilityEpoch,
                        "a facility model must advance the facility epoch when it replaces another", "facility",
                        "installed " + std::to_string(current.facility_epoch.value()) + ", offered " +
                            std::to_string(snapshot.facility_epoch.value()));
        }
        if (is_set(current.observed_at) && is_set(snapshot.observed_at) &&
            snapshot.observed_at < current.observed_at) {
            return fail(ErrorCode::StaleObservation,
                        "a facility model may not be older than the one it replaces", "facility",
                        "installed " + std::to_string(current.observed_at) + ", offered " +
                            std::to_string(snapshot.observed_at));
        }
    }
    Digest outcome;
    Timestamp applied_at = 0;
    auto committed = commit_facility(snapshot, request.header, expected, "install-facility", outcome, applied_at);
    if (!committed.ok()) {
        return committed.status();
    }
    CommandResult result = base_result();
    result.mutation.intent_digest = expected;
    result.mutation.outcome_digest = outcome;
    result.mutation.sequence = committed.value();
    result.mutation.applied_at = applied_at;
    result.facility = snapshot;
    result.notes.push_back("every approval taken against an earlier epoch is fenced from now on");
    event = EngineEvent{"facility-installed", PlanId{}, PlanPhase::Proposed, result.mutation.sequence, outcome,
                        applied_at, snapshot.digest.hex()};
    return result;
}

Result<CommandResult> Coordinator::Impl::compact(const CompactRequest& request, std::optional<EngineEvent>& event) {
    const Digest expected = intent_digest_of(request);
    if (request.header.attempt.empty()) {
        return fail(ErrorCode::InvalidArgument, "every mutating command needs an attempt identity", "attempt");
    }
    if (!(request.header.intent_digest == expected)) {
        return fail(ErrorCode::ReplayIntentMismatch, "the supplied intent digest does not describe this request",
                    "attempt", "supplied " + request.header.intent_digest.hex() + ", derived " + expected.hex());
    }
    auto committed = store_->compact();
    if (!committed.ok()) {
        return committed.status();
    }
    CommandResult result = base_result();
    result.mutation.intent_digest = expected;
    result.mutation.sequence = committed.value();
    result.mutation.outcome_digest = detail::ledger_outcome_digest("compact", store_->cached_ledger());
    result.mutation.applied_at = this->now();
    result.notes.push_back("the ledger was published as a verified snapshot and the journal prefix was truncated");
    event = EngineEvent{"compacted", PlanId{}, PlanPhase::Proposed, result.mutation.sequence,
                        result.mutation.outcome_digest, result.mutation.applied_at, {}};
    return result;
}
// ---------------------------------------------------------------------------
// Read-only commands
// ---------------------------------------------------------------------------

Result<CommandResult> Coordinator::explain(const ExplainRequest& request) const {
    const std::lock_guard<std::mutex> guard(impl_->mutex());
    const LedgerState& ledger = impl_->store().cached_ledger();
    const PlanRecord* record = ledger.find_plan(request.plan);
    if (record == nullptr) {
        return fail(ErrorCode::UnknownPlan, "no such plan", "plan", request.plan.name());
    }
    CommandResult result = impl_->base_result();
    result.has_plan = true;
    result.plan = *record;
    result.obligations = detail::derive_obligation_status(*record, impl_->now());
    if (ledger.facility.has_value()) {
        result.facility = ledger.facility;
        result.explanation = detail::build_explanation(*ledger.facility, *record, impl_->now());
    } else {
        Explanation explanation;
        explanation.plan = record->plan.id;
        explanation.revision = record->plan.revision;
        explanation.phase = record->phase;
        explanation.reasons.push_back("no facility model has been installed, so no gate can be evaluated");
        explanation.finalize();
        result.explanation = explanation;
    }
    return result;
}

Result<CommandResult> Coordinator::show(const ShowRequest& request) const {
    const std::lock_guard<std::mutex> guard(impl_->mutex());
    const LedgerState& ledger = impl_->store().cached_ledger();
    const PlanRecord* record = ledger.find_plan(request.plan);
    if (record == nullptr) {
        return fail(ErrorCode::UnknownPlan, "no such plan", "plan", request.plan.name());
    }
    CommandResult result = impl_->base_result();
    result.has_plan = true;
    result.plan = *record;
    result.obligations = detail::derive_obligation_status(*record, impl_->now());
    if (record->evaluation.has_value()) {
        result.evaluation = record->evaluation;
    }
    if (record->approval.has_value()) {
        result.approval = record->approval;
    }
    if (ledger.facility.has_value()) {
        result.facility = ledger.facility;
    }
    return result;
}

Result<CommandResult> Coordinator::list(const ListRequest& request) const {
    const std::lock_guard<std::mutex> guard(impl_->mutex());
    const LedgerState& ledger = impl_->store().cached_ledger();
    CommandResult result = impl_->base_result();
    for (const auto& entry : ledger.plans) {
        if (!request.include_terminal && is_terminal(entry.second.phase)) {
            continue;
        }
        result.plans.push_back(detail::summarize(entry.second));
    }
    if (ledger.facility.has_value()) {
        result.facility = ledger.facility;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------

Coordinator::Coordinator(Store& store, const Clock& clock, EventSink* sink)
    : impl_(std::make_unique<Impl>(store, clock, sink)) {}

Coordinator::~Coordinator() = default;

Result<CommandResult> Coordinator::propose(const ProposeRequest& request) {
    return impl_->execute(request, &Impl::propose);
}

Result<CommandResult> Coordinator::evaluate(const EvaluateRequest& request) {
    return impl_->execute(request, &Impl::evaluate);
}

Result<CommandResult> Coordinator::approve(const ApproveRequest& request) {
    return impl_->execute(request, &Impl::approve);
}

Result<CommandResult> Coordinator::derive_obligations(const DeriveObligationsRequest& request) {
    return impl_->execute(request, &Impl::derive_obligations);
}

Result<CommandResult> Coordinator::ingest_receipt(const IngestReceiptRequest& request) {
    return impl_->execute(request, &Impl::ingest_receipt);
}

Result<CommandResult> Coordinator::grant_exception(const GrantExceptionRequest& request) {
    return impl_->execute(request, &Impl::grant_exception);
}

Result<CommandResult> Coordinator::begin(const BeginRequest& request) {
    return impl_->execute(request, &Impl::begin);
}

Result<CommandResult> Coordinator::record_progress(const RecordProgressRequest& request) {
    return impl_->execute(request, &Impl::record_progress);
}

Result<CommandResult> Coordinator::verify_restoration(const VerifyRequest& request) {
    return impl_->execute(request, &Impl::verify_restoration);
}

Result<CommandResult> Coordinator::complete(const CompleteRequest& request) {
    return impl_->execute(request, &Impl::complete);
}

Result<CommandResult> Coordinator::cancel(const CancelRequest& request) {
    return impl_->execute(request, &Impl::cancel);
}

Result<CommandResult> Coordinator::recover(const RecoverRequest& request) {
    return impl_->execute(request, &Impl::recover);
}

Result<CommandResult> Coordinator::install_facility(const InstallFacilityRequest& request) {
    return impl_->execute(request, &Impl::install_facility);
}

Result<CommandResult> Coordinator::compact(const CompactRequest& request) {
    return impl_->execute(request, &Impl::compact);
}

}  // namespace mc

// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// The coordination engine.
//
// Every mutating command carries an AttemptId and the digest of the intent it
// believes it is executing.  A command that has already been applied is
// replayed from the durable attempt table instead of being applied twice, and a
// reused AttemptId with a different intent is rejected.  Lost-response replay
// is therefore handled before ordinary stale-plan rejection, which is the only
// ordering in which a retry after a dropped response can be safe.
//
// A Coordinator serialises the commands it executes with one state mutex.  The
// sink of EngineEvent is invoked after that mutex is released, and no callback
// of any kind runs while the lock is held.

#ifndef MC_ENGINE_HPP
#define MC_ENGINE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mc/approval.hpp"
#include "mc/digest.hpp"
#include "mc/facility.hpp"
#include "mc/ident.hpp"
#include "mc/obligation.hpp"
#include "mc/plan.hpp"
#include "mc/scope.hpp"
#include "mc/status.hpp"
#include "mc/store.hpp"
#include "mc/time.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

// Shared by every mutating command.  `intent_digest` must equal the digest the
// engine derives from the request; an honest caller obtains it from
// intent_digest_of(request).  A mismatch means the caller is not replaying the
// operation it thinks it is, and is rejected with ReplayIntentMismatch.
struct CommandHeader {
    AttemptId attempt;
    Digest intent_digest;
    std::string actor;
};

struct ProposeRequest {
    CommandHeader header;
    PlanId plan_id;  // optional; the engine assigns a deterministic id when empty
    std::string reason;
    std::string requested_by;
    MaintenanceActivity activity{MaintenanceActivity::Inspection};
    PlanPriority priority{PlanPriority::Routine};
    ServiceRiskClass risk{ServiceRiskClass::Low};
    MaintenanceScope scope;
    WindowSpec window;
};

struct EvaluateRequest {
    CommandHeader header;
    PlanId plan;
    Revision revision;
};

struct ApproveRequest {
    CommandHeader header;
    PlanId plan;
    Revision revision;
    std::string approved_by;
    std::string approval_evidence;
    std::string witness;  // required when policy demands dual approval
};

struct DeriveObligationsRequest {
    CommandHeader header;
    PlanId plan;
    Revision revision;
};

struct IngestReceiptRequest {
    CommandHeader header;
    PlanId plan;
    Revision revision;
    ReceiptId receipt_id;
    ObligationId obligation;
    ReceiptKind kind{ReceiptKind::DrainObserved};
    ObligationAuthority issuer_authority{ObligationAuthority::Asi};
    std::string issuer;
    ObservationSequence observation_sequence;
    std::string evidence;
    Timestamp observed_at{kNoTimestamp};
};

struct GrantExceptionRequest {
    CommandHeader header;
    PlanId plan;
    Revision revision;
    std::vector<ErrorCode> waived;
    std::vector<TargetRef> targets;
    std::string granted_by;
    std::string justification;
    std::int64_t validity_nanos{0};
};

struct BeginRequest {
    CommandHeader header;
    PlanId plan;
    Revision revision;
};

struct RecordProgressRequest {
    CommandHeader header;
    PlanId plan;
    Revision revision;
    ProgressKind kind{ProgressKind::StepCompleted};
    std::string note;
};

struct VerifyRequest {
    CommandHeader header;
    PlanId plan;
    Revision revision;
};

struct CompleteRequest {
    CommandHeader header;
    PlanId plan;
    Revision revision;
    std::string note;
};

struct CancelRequest {
    CommandHeader header;
    PlanId plan;
    Revision revision;
    std::string reason;
};

struct RecoverRequest {
    CommandHeader header;
    PlanId plan;  // empty means: every plan that needs recovery
};

struct ExplainRequest {
    PlanId plan;
};

struct ShowRequest {
    PlanId plan;
};

struct ListRequest {
    bool include_terminal{true};
};

struct InstallFacilityRequest {
    CommandHeader header;
    FacilitySnapshot facility;
};

struct CompactRequest {
    CommandHeader header;
};

// Intent digests.  These are computed from the canonical encoding of the
// request, so the CLI, a test and the engine all derive the same value.
[[nodiscard]] Digest intent_digest_of(const ProposeRequest& request);
[[nodiscard]] Digest intent_digest_of(const EvaluateRequest& request);
[[nodiscard]] Digest intent_digest_of(const ApproveRequest& request);
[[nodiscard]] Digest intent_digest_of(const DeriveObligationsRequest& request);
[[nodiscard]] Digest intent_digest_of(const IngestReceiptRequest& request);
[[nodiscard]] Digest intent_digest_of(const GrantExceptionRequest& request);
[[nodiscard]] Digest intent_digest_of(const BeginRequest& request);
[[nodiscard]] Digest intent_digest_of(const RecordProgressRequest& request);
[[nodiscard]] Digest intent_digest_of(const VerifyRequest& request);
[[nodiscard]] Digest intent_digest_of(const CompleteRequest& request);
[[nodiscard]] Digest intent_digest_of(const CancelRequest& request);
[[nodiscard]] Digest intent_digest_of(const RecoverRequest& request);
[[nodiscard]] Digest intent_digest_of(const InstallFacilityRequest& request);
[[nodiscard]] Digest intent_digest_of(const CompactRequest& request);

// ---------------------------------------------------------------------------
// Outcomes
// ---------------------------------------------------------------------------

struct MutationOutcome {
    Digest intent_digest;
    Digest outcome_digest;
    CommitSequence sequence;
    Timestamp applied_at{kNoTimestamp};
    // True when this command was recognised as an already-applied attempt: no
    // new mutation was written, and the record reported below is the current
    // authoritative one.
    bool replayed{false};
};

struct PlanSummary {
    PlanId id;
    Revision revision;
    PlanPhase phase{PlanPhase::Proposed};
    MaintenanceActivity activity{MaintenanceActivity::Inspection};
    PlanPriority priority{PlanPriority::Routine};
    ServiceRiskClass risk{ServiceRiskClass::Low};
    WindowSpec window;
    std::size_t target_count{0};
    bool recovery_required{false};
    Timestamp updated_at{kNoTimestamp};
};

// The result of any command.  The plan record is always the current
// authoritative record for the plan the command addressed, including when the
// command was a replay.
struct CommandResult {
    MutationOutcome mutation;
    bool has_plan{false};
    PlanRecord plan;
    std::optional<FacilitySnapshot> facility;
    std::optional<PreconditionReport> evaluation;
    std::optional<ApprovalRecord> approval;
    std::optional<ObligationStatusReport> obligations;
    std::optional<Receipt> receipt;
    std::optional<ExceptionGrant> exception;
    std::optional<RestorationReport> restoration;
    std::optional<CompletionReport> completion;
    std::optional<RecoveryReport> recovery;
    std::optional<Explanation> explanation;
    std::vector<PlanSummary> plans;
    std::vector<std::string> notes;
    RecoverySummary store_recovery;
};

// Observability.  Events are emitted after the coordinator lock is released,
// so a sink must not call back into the same Coordinator.
struct EngineEvent {
    std::string name;
    PlanId plan;
    PlanPhase phase{PlanPhase::Proposed};
    CommitSequence sequence;
    Digest outcome_digest;
    Timestamp at{kNoTimestamp};
    std::string detail;
};

class EventSink {
public:
    EventSink() = default;
    EventSink(const EventSink&) = delete;
    EventSink& operator=(const EventSink&) = delete;
    virtual ~EventSink() = default;

    virtual void on_event(const EngineEvent& event) = 0;
};

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------

class Coordinator {
public:
    // The store and the clock must outlive the coordinator.  The sink is
    // optional and must also outlive it.
    Coordinator(Store& store, const Clock& clock, EventSink* sink = nullptr);
    ~Coordinator();

    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;
    Coordinator(Coordinator&&) = delete;
    Coordinator& operator=(Coordinator&&) = delete;

    [[nodiscard]] Result<CommandResult> propose(const ProposeRequest& request);
    [[nodiscard]] Result<CommandResult> evaluate(const EvaluateRequest& request);
    [[nodiscard]] Result<CommandResult> approve(const ApproveRequest& request);
    [[nodiscard]] Result<CommandResult> derive_obligations(const DeriveObligationsRequest& request);
    [[nodiscard]] Result<CommandResult> ingest_receipt(const IngestReceiptRequest& request);
    [[nodiscard]] Result<CommandResult> grant_exception(const GrantExceptionRequest& request);
    [[nodiscard]] Result<CommandResult> begin(const BeginRequest& request);
    [[nodiscard]] Result<CommandResult> record_progress(const RecordProgressRequest& request);
    [[nodiscard]] Result<CommandResult> verify_restoration(const VerifyRequest& request);
    [[nodiscard]] Result<CommandResult> complete(const CompleteRequest& request);
    [[nodiscard]] Result<CommandResult> cancel(const CancelRequest& request);
    [[nodiscard]] Result<CommandResult> recover(const RecoverRequest& request);
    [[nodiscard]] Result<CommandResult> install_facility(const InstallFacilityRequest& request);
    [[nodiscard]] Result<CommandResult> compact(const CompactRequest& request);

    [[nodiscard]] Result<CommandResult> explain(const ExplainRequest& request) const;
    [[nodiscard]] Result<CommandResult> show(const ShowRequest& request) const;
    [[nodiscard]] Result<CommandResult> list(const ListRequest& request) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mc

#endif  // MC_ENGINE_HPP

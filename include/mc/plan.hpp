// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Maintenance plans, their lifecycle phases, and the durable plan record.
//
// The phase names are the contract.  Their meaning is deliberately asymmetric:
//
//   Proposed    the intent exists; nothing has been checked
//   Evaluating  preconditions were computed; the plan may still be blocked
//   Approved    an authority approved this exact revision against exact generations
//   PreDrain    obligations were derived and requests went to ASI/DFI/plant
//   Ready       every pre-drain and isolation obligation is satisfied
//   InProgress  work is happening
//   Verification work is finished but not yet verified
//   Restore     the facility is being returned to its protected posture
//   Complete    completion evidence exists and restoration was verified
//   Blocked     an explicit, named blocker prevents progress
//   Cancelled   stopped before work began, or restored after an interrupted start
//   Failed      the window ended in a state that requires operator attention
//
// Scheduled is not approved.  Approved is not ready.  Drain requested is not
// drained.  Started is not completed.  Completion requires evidence.

#ifndef MC_PLAN_HPP
#define MC_PLAN_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mc/approval.hpp"
#include "mc/digest.hpp"
#include "mc/facility.hpp"
#include "mc/ident.hpp"
#include "mc/obligation.hpp"
#include "mc/scope.hpp"
#include "mc/status.hpp"

namespace mc {

enum class PlanPhase : std::uint8_t {
    Proposed = 0,
    Evaluating = 1,
    Approved = 2,
    PreDrain = 3,
    Ready = 4,
    InProgress = 5,
    Verification = 6,
    Restore = 7,
    Complete = 8,
    Blocked = 9,
    Cancelled = 10,
    Failed = 11,
};

[[nodiscard]] std::string_view to_string(PlanPhase phase) noexcept;
[[nodiscard]] bool parse_plan_phase(std::string_view text, PlanPhase& out) noexcept;
[[nodiscard]] bool is_terminal(PlanPhase phase) noexcept;
// True once the plan has begun affecting the plant (work started or evidence
// of effect was accepted), which forbids a plain cancellation.
[[nodiscard]] bool is_started(PlanPhase phase) noexcept;
// The canonical forward order, used for deterministic reporting only.
[[nodiscard]] std::uint8_t phase_order(PlanPhase phase) noexcept;

// ---------------------------------------------------------------------------
// Plan intent
// ---------------------------------------------------------------------------

class MaintenancePlan {
public:
    PlanId id;
    Revision revision;
    std::string reason;
    std::string requested_by;
    MaintenanceActivity activity{MaintenanceActivity::Inspection};
    PlanPriority priority{PlanPriority::Routine};
    ServiceRiskClass risk{ServiceRiskClass::Low};
    MaintenanceScope scope;
    WindowSpec window;

    // The exact view this intent was planned against.
    FacilityEpoch facility_epoch;
    ControlEpoch control_epoch;
    PolicyGeneration policy_generation;
    DependencyGeneration dependency_generation;
    CapacityGeneration capacity_generation;
    TopologyGeneration topology_generation;
    MaintenanceGeneration maintenance_generation;
    Digest policy_digest;
    Digest dependency_digest;
    Digest digest;

    void finalize();
    [[nodiscard]] Status validate() const;

    [[nodiscard]] friend bool operator==(const MaintenancePlan&, const MaintenancePlan&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Phase transitions and progress
// ---------------------------------------------------------------------------

struct PhaseTransition {
    PlanPhase from{PlanPhase::Proposed};
    PlanPhase to{PlanPhase::Proposed};
    Timestamp at{kNoTimestamp};
    std::string actor;
    std::string reason;
    ErrorCode cause{ErrorCode::Ok};  // the blocker that caused a move to Blocked
    CommitSequence sequence;
    Digest digest;

    [[nodiscard]] friend bool operator==(const PhaseTransition&, const PhaseTransition&) noexcept = default;
};

enum class ProgressKind : std::uint8_t {
    Started = 0,
    StepCompleted = 1,
    WorkCompleted = 2,  // declares that physical work is finished: InProgress -> Verification
    WorkStopped = 3,    // declares that work stopped early: InProgress -> Restore
    IssueObserved = 4,  // records a problem without changing phase
};

[[nodiscard]] std::string_view to_string(ProgressKind kind) noexcept;
[[nodiscard]] bool parse_progress_kind(std::string_view text, ProgressKind& out) noexcept;

struct ProgressEvent {
    ProgressKind kind{ProgressKind::Started};
    std::string note;
    std::string actor;
    Timestamp at{kNoTimestamp};
    Digest digest;

    [[nodiscard]] friend bool operator==(const ProgressEvent&, const ProgressEvent&) noexcept = default;
};

// ---------------------------------------------------------------------------
// The durable plan record
// ---------------------------------------------------------------------------

class PlanRecord {
public:
    MaintenancePlan plan;
    PlanPhase phase{PlanPhase::Proposed};
    std::vector<MaintenancePlan> history;  // superseded revisions, canonical order by revision

    std::optional<PreconditionReport> evaluation;
    std::optional<ApprovalRecord> approval;
    ObligationSet obligations;
    std::vector<Receipt> receipts;          // canonical order by (observation_sequence, id)
    std::vector<ProgressEvent> progress;    // canonical order by (at, digest)
    std::vector<ExceptionGrant> exceptions; // canonical order by id
    std::vector<PhaseTransition> transitions;
    std::vector<ConditionResult> blockers;  // the blocker set of the current phase

    // High-water mark of accepted evidence: any receipt at or below this
    // sequence is stale, including everything recovered from disk.
    ObservationSequence last_observation_sequence;
    // The mark that was in force when recovery was declared, so that "fresh"
    // means strictly newer than the state that was recovered rather than newer
    // than some earlier moment.
    ObservationSequence recovery_mark;
    bool recovery_required{false};
    Timestamp created_at{kNoTimestamp};
    Timestamp updated_at{kNoTimestamp};
    CommitSequence sequence;
    Digest digest;

    void canonicalize();
    void finalize();
    [[nodiscard]] Status validate() const;

    [[nodiscard]] const Receipt* find_receipt(const ReceiptId& id) const noexcept;
    [[nodiscard]] const ExceptionGrant* find_exception(const ExceptionId& id) const noexcept;
    [[nodiscard]] bool work_stopped() const noexcept;
    [[nodiscard]] bool work_completed() const noexcept;
    [[nodiscard]] std::vector<const Receipt*> receipts_for(const ObligationId& id) const;

    [[nodiscard]] friend bool operator==(const PlanRecord&, const PlanRecord&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

class RestorationReport {
public:
    PlanId plan;
    Revision plan_revision;
    Timestamp verified_at{kNoTimestamp};
    std::vector<ObligationId> outstanding;              // canonical order
    std::vector<ProtectionId> violated_protections;     // canonical order
    std::vector<RedundancyGroupId> redundancy_below_requirement;  // canonical order
    std::vector<ConditionResult> conditions;            // canonical order
    bool restored{false};
    Digest digest;

    void finalize();

    [[nodiscard]] friend bool operator==(const RestorationReport&, const RestorationReport&) noexcept = default;
};

class CompletionReport {
public:
    PlanId plan;
    Revision plan_revision;
    Timestamp completed_at{kNoTimestamp};
    CommitSequence sequence;
    Digest plan_digest;
    Digest approval_digest;
    Digest obligation_set_digest;
    Digest receipt_set_digest;
    Digest facility_digest;
    Digest policy_digest;
    Digest dependency_digest;
    std::vector<ObligationId> satisfied;  // canonical order
    bool protected_obligations_satisfied{false};
    bool restoration_verified{false};
    Digest digest;

    void finalize();

    [[nodiscard]] friend bool operator==(const CompletionReport&, const CompletionReport&) noexcept = default;
};

class RecoveryReport {
public:
    PlanId plan;
    PlanPhase phase_before{PlanPhase::Proposed};
    PlanPhase phase_after{PlanPhase::Proposed};
    Timestamp recovered_at{kNoTimestamp};
    IncarnationId prior_incarnation;
    IncarnationId current_incarnation;
    std::vector<std::string> notes;               // canonical order
    std::vector<ObligationId> outstanding;        // canonical order
    bool recovery_required_after{false};
    Digest digest;

    void finalize();

    [[nodiscard]] friend bool operator==(const RecoveryReport&, const RecoveryReport&) noexcept = default;
};

// Explanation of why a plan may or may not proceed, and what must happen next.
class Explanation {
public:
    PlanId plan;
    Revision revision;
    PlanPhase phase{PlanPhase::Proposed};
    std::vector<ConditionResult> blockers;    // canonical order
    std::vector<std::string> reasons;         // canonical order
    std::vector<ObligationStatus> obligations;  // canonical order
    std::vector<std::string> next_steps;      // canonical order
    bool can_evaluate{false};
    bool can_approve{false};
    bool can_begin{false};
    bool can_complete{false};
    bool can_cancel{false};
    Digest digest;

    void finalize();

    [[nodiscard]] friend bool operator==(const Explanation&, const Explanation&) noexcept = default;
};

}  // namespace mc

#endif  // MC_PLAN_HPP

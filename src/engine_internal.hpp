// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// INTERNAL contracts shared by the engine translation units.
//
// The gate functions are pure: they take the exact view they are asked about
// and return a value.  They never read a clock, never touch a store and never
// mutate their inputs, which is what makes "the same invalid request resolves
// to the same primary error" testable rather than aspirational.

#ifndef MC_INTERNAL_ENGINE_INTERNAL_HPP
#define MC_INTERNAL_ENGINE_INTERNAL_HPP

#include <string>
#include <string_view>
#include <vector>

#include "mc/engine.hpp"
#include "mc/facility.hpp"
#include "mc/obligation.hpp"
#include "mc/plan.hpp"
#include "mc/status.hpp"

namespace mc::detail {

// ---------------------------------------------------------------------------
// Preconditions
// ---------------------------------------------------------------------------

struct GateContext {
    const FacilitySnapshot* facility{nullptr};
    const MaintenancePlan* plan{nullptr};
    const PlanRecord* record{nullptr};
    // Plans that currently hold a scope: the concurrency condition is the only
    // one that depends on anything outside the facility view, and it is passed
    // in explicitly rather than read from ambient state.
    std::vector<const PlanRecord*> other_active_plans;
    Timestamp now{kNoTimestamp};
};

// Evaluates every precondition that applies to the plan's scope and window.
// Every condition is reported, satisfied or not, and the waivers recorded on
// the plan are applied to the conditions they explicitly cover.
[[nodiscard]] PreconditionReport evaluate_preconditions(const GateContext& context);

// Redundancy that would survive the maintenance, per redundancy group, and the
// groups that would fall below (required + policy margin).
struct RedundancyView {
    struct Group {
        RedundancyGroupId id;
        std::uint32_t observed_units{0};
        std::uint32_t removed_units{0};
        std::uint32_t surviving_units{0};
        std::uint32_t required_units{0};
    };
    std::vector<Group> groups;  // canonical order by id
};

[[nodiscard]] RedundancyView redundancy_after_maintenance(const FacilitySnapshot& facility,
                                                          const MaintenancePlan& plan);
[[nodiscard]] std::vector<RedundancyGroupId> redundancy_below_requirement(const FacilitySnapshot& facility,
                                                                          const MaintenancePlan& plan);
// Protected obligations that the maintenance would violate, with the group
// that would fall below the protection's floor.
[[nodiscard]] std::vector<ProtectionId> violated_protections(const FacilitySnapshot& facility,
                                                            const MaintenancePlan& plan);

// ---------------------------------------------------------------------------
// Obligations
// ---------------------------------------------------------------------------

// Derives the obligation set for a plan from its activity, scope, the facility
// model and the waivers recorded in its evaluation.  Deterministic: the same
// inputs always produce the same identities, contents and digest.
[[nodiscard]] ObligationSet derive_obligations(const GateContext& context);

// Derives the disposition of every obligation from the receipts bound to it.
// Nothing here is stored, so no partial write can make an obligation look
// satisfied without evidence.
[[nodiscard]] ObligationStatusReport derive_obligation_status(const PlanRecord& record, Timestamp now);

// True when the receipt kind can lawfully be ingested against the obligation.
[[nodiscard]] bool receipt_matches_obligation(ObligationKind obligation, ReceiptKind receipt) noexcept;

[[nodiscard]] Digest evidence_digest_of(std::string_view evidence);

// ---------------------------------------------------------------------------
// Fencing
// ---------------------------------------------------------------------------

// Compares an approval with the current facility view.  Returns the most
// specific stale code when a bound generation, epoch or digest no longer
// matches, and success when the approval still describes this facility.
[[nodiscard]] Status check_approval_fence(const ApprovalRecord& approval, const PreconditionReport& evaluation,
                                          const FacilitySnapshot& facility);

// The most primary blocking condition of a report, or success when none
// blocks.  Used to turn a failed gate into one deterministic error.
[[nodiscard]] Status primary_blocker(const PreconditionReport& report);

// ---------------------------------------------------------------------------
// Outcomes
// ---------------------------------------------------------------------------

[[nodiscard]] Digest outcome_digest_of(std::string_view command, const PlanRecord& record,
                                       CommitSequence sequence);
[[nodiscard]] Digest outcome_digest_of(std::string_view command, const FacilitySnapshot& facility,
                                       CommitSequence sequence);
[[nodiscard]] Digest receipt_set_digest(const std::vector<Receipt>& receipts);

// Outcome binding for commands that do not address a single record: the digest
// covers the command, the resulting commit sequence and every plan digest in
// the ledger, so the outcome is a verifiable claim about the whole state.
[[nodiscard]] Digest ledger_outcome_digest(std::string_view command, const LedgerState& ledger);

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

// Explains the current state of a plan: why it is where it is, what blocks it,
// what it still owes, and which commands are available from here.
[[nodiscard]] Explanation build_explanation(const FacilitySnapshot& facility, const PlanRecord& record,
                                            Timestamp now);
[[nodiscard]] PlanSummary summarize(const PlanRecord& record);

}  // namespace mc::detail

#endif  // MC_INTERNAL_ENGINE_INTERNAL_HPP

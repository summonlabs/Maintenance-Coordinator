// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Read-only report construction.  These functions never mutate anything: they
// answer "why is this plan here and what would have to be true for it to move".

#include <cstddef>
#include <string>
#include <utility>

#include "engine_internal.hpp"
#include "mc/plan.hpp"

namespace mc::detail {
namespace {

void add_reason(Explanation& explanation, std::string text) {
    explanation.reasons.push_back(std::move(text));
}

}  // namespace

Explanation build_explanation(const FacilitySnapshot& facility, const PlanRecord& record, Timestamp now) {
    Explanation explanation;
    explanation.plan = record.plan.id;
    explanation.revision = record.plan.revision;
    explanation.phase = record.phase;

    const ObligationStatusReport statuses = derive_obligation_status(record, now);
    explanation.obligations = statuses.statuses;

    if (record.evaluation.has_value()) {
        explanation.blockers = record.evaluation->blocking();
    } else {
        explanation.blockers = record.blockers;
    }

    const bool has_approval = record.approval.has_value();
    const bool fence_clear = has_approval && record.evaluation.has_value() &&
                             check_approval_fence(*record.approval, *record.evaluation, facility).ok();
    const bool window_open = is_set(record.plan.window.start) && is_set(record.plan.window.end) &&
                             now >= record.plan.window.start && now <= record.plan.window.end;

    switch (record.phase) {
        case PlanPhase::Proposed:
            add_reason(explanation, "the intent is recorded; no precondition has been evaluated");
            explanation.next_steps.push_back("evaluate");
            break;
        case PlanPhase::Evaluating:
            add_reason(explanation, "preconditions were evaluated and none blocks the plan");
            explanation.next_steps.push_back("approve");
            break;
        case PlanPhase::Approved:
            add_reason(explanation, "an authority approved this revision against this generation set");
            explanation.next_steps.push_back("derive-obligations");
            break;
        case PlanPhase::Blocked:
            add_reason(explanation, "at least one precondition blocks this plan");
            explanation.next_steps.push_back("grant-exception or change the facility state");
            explanation.next_steps.push_back("evaluate");
            break;
        case PlanPhase::PreDrain:
            add_reason(explanation, "obligations are outstanding; each needs observed evidence");
            explanation.next_steps.push_back("ingest-receipt for every outstanding obligation");
            break;
        case PlanPhase::Ready:
            add_reason(explanation, "every pre-drain and isolation obligation is satisfied");
            explanation.next_steps.push_back(window_open ? "begin" : "wait for the window to open");
            break;
        case PlanPhase::InProgress:
            add_reason(explanation, "work is in progress under this window");
            explanation.next_steps.push_back("record-progress");
            break;
        case PlanPhase::Verification:
            add_reason(explanation, "work was reported complete; it has not been verified");
            explanation.next_steps.push_back("ingest-receipt for maintenance-verified");
            explanation.next_steps.push_back("verify-restoration");
            break;
        case PlanPhase::Restore:
            add_reason(explanation, "the facility is being returned to its protected posture");
            explanation.next_steps.push_back("complete or cancel once restoration evidence exists");
            break;
        case PlanPhase::Complete:
            add_reason(explanation, "the window completed with evidence and a verified restoration");
            break;
        case PlanPhase::Cancelled:
            add_reason(explanation, "the window was cancelled");
            break;
        case PlanPhase::Failed:
            add_reason(explanation, "the window failed and needs operator attention");
            break;
        default:
            add_reason(explanation, "phase is not recognised");
            break;
    }

    if (record.recovery_required) {
        add_reason(explanation, "this window was interrupted and needs fresh evidence before it can move");
        explanation.next_steps.push_back("recover");
        explanation.next_steps.push_back("ingest-receipt for work-stop-confirmed with a newer observation");
    }
    if (has_approval && !fence_clear) {
        add_reason(explanation, "the approval no longer describes the facility it was granted against");
        explanation.next_steps.push_back("evaluate and approve again");
    }
    for (const auto& status : statuses.statuses) {
        if (status.disposition == Disposition::Failed) {
            add_reason(explanation, "obligation " + status.id.name() + " was reported as failed by " +
                                        std::string(to_string(status.authority)));
        }
    }

    explanation.can_evaluate = record.phase == PlanPhase::Proposed || record.phase == PlanPhase::Evaluating ||
                               record.phase == PlanPhase::Blocked;
    explanation.can_approve = record.phase == PlanPhase::Evaluating && !has_approval &&
                              record.evaluation.has_value() && record.evaluation->satisfied;
    explanation.can_begin = record.phase == PlanPhase::Ready && fence_clear && statuses.ready_for_isolation &&
                            !record.recovery_required && window_open;
    explanation.can_complete = record.phase == PlanPhase::Restore && fence_clear &&
                               statuses.ready_for_completion && !record.recovery_required;
    explanation.can_cancel = record.phase == PlanPhase::Proposed || record.phase == PlanPhase::Evaluating ||
                             record.phase == PlanPhase::Blocked || record.phase == PlanPhase::Approved ||
                             record.phase == PlanPhase::PreDrain || record.phase == PlanPhase::Ready ||
                             (record.phase == PlanPhase::Restore && statuses.ready_for_completion);
    explanation.finalize();
    return explanation;
}

PlanSummary summarize(const PlanRecord& record) {
    PlanSummary summary;
    summary.id = record.plan.id;
    summary.revision = record.plan.revision;
    summary.phase = record.phase;
    summary.activity = record.plan.activity;
    summary.priority = record.plan.priority;
    summary.risk = record.plan.risk;
    summary.window = record.plan.window;
    summary.target_count = record.plan.scope.size();
    summary.recovery_required = record.recovery_required;
    summary.updated_at = record.updated_at;
    return summary;
}

}  // namespace mc::detail

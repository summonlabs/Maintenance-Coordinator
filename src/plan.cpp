// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.

#include "mc/plan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "enum_table.hpp"
#include "serialize.hpp"

namespace mc {
namespace {

constexpr std::array<std::pair<std::string_view, unsigned>, 12> kPhases{{
    {"proposed", 0}, {"evaluating", 1}, {"approved", 2}, {"pre-drain", 3}, {"ready", 4}, {"in-progress", 5},
    {"verification", 6}, {"restore", 7}, {"complete", 8}, {"blocked", 9}, {"cancelled", 10}, {"failed", 11},
}};

constexpr std::array<std::pair<std::string_view, unsigned>, 5> kProgressKinds{{
    {"started", 0}, {"step-completed", 1}, {"work-completed", 2}, {"work-stopped", 3}, {"issue-observed", 4},
}};

constexpr std::size_t kMaxTextLength = 4096;

[[nodiscard]] Status validate_text(const std::string& text, std::string_view what, std::string_view subject) {
    if (text.empty()) {
        return fail(ErrorCode::InvalidArgument, std::string(what) + " must not be empty", std::string(subject));
    }
    if (text.size() > kMaxTextLength) {
        return fail(ErrorCode::LimitExceeded, std::string(what) + " exceeds the accepted length",
                    std::string(subject), std::to_string(text.size()));
    }
    return Status::success();
}

}  // namespace

std::string_view to_string(PlanPhase phase) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(phase), kPhases);
}

bool parse_plan_phase(std::string_view text, PlanPhase& out) noexcept {
    unsigned raw = 0;
    if (!detail::parse_enum_name(text, kPhases, raw)) {
        return false;
    }
    out = static_cast<PlanPhase>(raw);
    return true;
}

bool is_terminal(PlanPhase phase) noexcept {
    return phase == PlanPhase::Complete || phase == PlanPhase::Cancelled || phase == PlanPhase::Failed;
}

bool is_started(PlanPhase phase) noexcept {
    return phase == PlanPhase::InProgress || phase == PlanPhase::Verification || phase == PlanPhase::Restore;
}

std::uint8_t phase_order(PlanPhase phase) noexcept {
    switch (phase) {
        case PlanPhase::Proposed: return 0;
        case PlanPhase::Evaluating: return 1;
        case PlanPhase::Approved: return 2;
        case PlanPhase::PreDrain: return 3;
        case PlanPhase::Ready: return 4;
        case PlanPhase::InProgress: return 5;
        case PlanPhase::Verification: return 6;
        case PlanPhase::Restore: return 7;
        case PlanPhase::Complete: return 8;
        case PlanPhase::Blocked:
        case PlanPhase::Cancelled:
        case PlanPhase::Failed:
        default:
            return 9;
    }
}

std::string_view to_string(ProgressKind kind) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(kind), kProgressKinds);
}

bool parse_progress_kind(std::string_view text, ProgressKind& out) noexcept {
    unsigned raw = 0;
    if (!detail::parse_enum_name(text, kProgressKinds, raw)) {
        return false;
    }
    out = static_cast<ProgressKind>(raw);
    return true;
}

// ---------------------------------------------------------------------------
// Plan intent
// ---------------------------------------------------------------------------

void MaintenancePlan::finalize() {
    scope.canonicalize();
    digest = detail::digest_of(*this);
}

Status MaintenancePlan::validate() const {
    Status primary;
    if (id.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "plan has no identity", "plan"));
    }
    if (!revision.is_set()) {
        primary.merge(fail(ErrorCode::MissingGeneration, "plan has no revision", "plan", id.name()));
    }
    primary.merge(validate_text(reason, "plan reason", id.name()));
    if (requested_by.empty() || !is_valid_identifier(requested_by)) {
        primary.merge(fail(ErrorCode::InvalidArgument, "plan has no accountable requester", "plan", id.name()));
    }
    if (scope.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "plan names no target", "plan", id.name()));
    }
    for (const auto& target : scope.targets()) {
        if (target.empty()) {
            primary.merge(fail(ErrorCode::InvalidArgument, "plan names an empty target reference", "plan", id.name()));
        }
    }
    if (!is_set(window.start) || !is_set(window.end)) {
        primary.merge(fail(ErrorCode::InvalidArgument, "plan window is not fully specified", "plan", id.name()));
    } else if (window.start >= window.end) {
        primary.merge(fail(ErrorCode::InvalidArgument, "plan window ends before it starts", "plan", id.name()));
    }
    if (!facility_epoch.is_set() || !policy_generation.is_set() || !dependency_generation.is_set() ||
        !capacity_generation.is_set() || !topology_generation.is_set() || !maintenance_generation.is_set() ||
        !control_epoch.is_set()) {
        primary.merge(fail(ErrorCode::MissingGeneration, "plan is not bound to a full generation set", "plan",
                           id.name()));
    }
    if (policy_digest.is_zero() || dependency_digest.is_zero()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "plan is not bound to policy and dependency digests", "plan",
                           id.name()));
    }
    if (digest.is_zero()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "plan digest has not been computed", "plan", id.name()));
    }
    return primary;
}

// ---------------------------------------------------------------------------
// Plan record
// ---------------------------------------------------------------------------

void PlanRecord::canonicalize() {
    plan.finalize();
    std::sort(history.begin(), history.end(),
              [](const MaintenancePlan& lhs, const MaintenancePlan& rhs) { return lhs.revision < rhs.revision; });
    for (auto& revision : history) {
        revision.finalize();
    }
    if (evaluation.has_value()) {
        evaluation->finalize();
    }
    if (approval.has_value()) {
        approval->finalize();
    }
    obligations.finalize();
    std::stable_sort(receipts.begin(), receipts.end(), [](const Receipt& lhs, const Receipt& rhs) {
        if (!(lhs.observation_sequence == rhs.observation_sequence)) {
            return lhs.observation_sequence < rhs.observation_sequence;
        }
        return lhs.id < rhs.id;
    });
    for (auto& receipt : receipts) {
        receipt.finalize();
    }
    std::stable_sort(progress.begin(), progress.end(), [](const ProgressEvent& lhs, const ProgressEvent& rhs) {
        if (lhs.at != rhs.at) {
            return lhs.at < rhs.at;
        }
        return lhs.note < rhs.note;
    });
    for (auto& event : progress) {
        event.digest = detail::digest_of(event);
    }
    std::sort(exceptions.begin(), exceptions.end(),
              [](const ExceptionGrant& lhs, const ExceptionGrant& rhs) { return lhs.id < rhs.id; });
    for (auto& grant : exceptions) {
        grant.finalize();
    }
    std::stable_sort(transitions.begin(), transitions.end(),
                     [](const PhaseTransition& lhs, const PhaseTransition& rhs) {
                         if (lhs.at != rhs.at) {
                             return lhs.at < rhs.at;
                         }
                         return lhs.sequence < rhs.sequence;
                     });
    for (auto& transition : transitions) {
        transition.digest = detail::digest_of(transition);
    }
    std::sort(blockers.begin(), blockers.end());
    blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
}

void PlanRecord::finalize() {
    canonicalize();
    digest = detail::digest_of(*this);
}

Status PlanRecord::validate() const {
    Status primary = plan.validate();
    if (evaluation.has_value()) {
        primary.merge(evaluation->validate());
        if (evaluation->plan_revision != plan.revision) {
            primary.merge(fail(ErrorCode::StaleRevision, "evaluation report does not belong to this plan revision",
                               "plan", plan.id.name()));
        }
        if (!(evaluation->plan_digest == plan.digest)) {
            primary.merge(fail(ErrorCode::StalePlanDigest, "evaluation report does not match the plan digest",
                               "plan", plan.id.name()));
        }
    }
    if (approval.has_value()) {
        primary.merge(approval->validate());
        if (approval->plan != plan.id) {
            primary.merge(fail(ErrorCode::IdentityMismatch, "approval belongs to a different plan", "plan",
                               plan.id.name()));
        }
        if (approval->plan_revision != plan.revision) {
            primary.merge(fail(ErrorCode::StaleRevision, "approval does not belong to this plan revision", "plan",
                               plan.id.name()));
        }
    }
    primary.merge(obligations.validate());
    for (const auto& receipt : receipts) {
        primary.merge(receipt.validate());
        if (receipt.plan_revision != plan.revision) {
            primary.merge(fail(ErrorCode::StaleRevision, "receipt does not belong to this plan revision", "receipt",
                               receipt.id.name()));
        }
    }
    for (std::size_t index = 1; index < receipts.size(); ++index) {
        if (!(receipts[index - 1].observation_sequence < receipts[index].observation_sequence) &&
            receipts[index - 1].observation_sequence == receipts[index].observation_sequence) {
            if (!(receipts[index - 1].id < receipts[index].id)) {
                primary.merge(fail(ErrorCode::LayoutInvalid, "receipts are not in canonical order", "plan",
                                   plan.id.name()));
                break;
            }
        }
    }
    for (std::size_t index = 1; index < receipts.size(); ++index) {
        if (receipts[index - 1].id == receipts[index].id) {
            primary.merge(fail(ErrorCode::DuplicateIdentity, "two receipts share one identity", "receipt",
                               receipts[index].id.name()));
        }
    }
    for (std::size_t index = 1; index < transitions.size(); ++index) {
        if (transitions[index - 1].to != transitions[index].from) {
            primary.merge(fail(ErrorCode::LayoutInvalid, "phase transitions do not form a chain", "plan",
                               plan.id.name()));
            break;
        }
    }
    if (!transitions.empty() && transitions.back().to != phase) {
        primary.merge(fail(ErrorCode::LayoutInvalid, "recorded phase does not match the last transition", "plan",
                           plan.id.name()));
    }
    if (last_observation_sequence.is_set()) {
        for (const auto& receipt : receipts) {
            if (receipt.observation_sequence.value() > last_observation_sequence.value()) {
                primary.merge(fail(ErrorCode::LayoutInvalid,
                                   "receipt observation sequence exceeds the recorded high-water mark", "plan",
                                   plan.id.name()));
                break;
            }
        }
    }
    if (recovery_required && !recovery_mark.is_set()) {
        primary.merge(fail(ErrorCode::LayoutInvalid, "recovery is required but no recovery mark was recorded",
                           "plan", plan.id.name()));
    }
    if (recovery_mark.is_set() && last_observation_sequence.is_set() &&
        recovery_mark.value() > last_observation_sequence.value()) {
        primary.merge(fail(ErrorCode::LayoutInvalid, "recovery mark is ahead of the evidence high-water mark",
                           "plan", plan.id.name()));
    }
    if (digest.is_zero()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "plan record digest has not been computed", "plan",
                           plan.id.name()));
    }
    return primary;
}

const Receipt* PlanRecord::find_receipt(const ReceiptId& id) const noexcept {
    for (const auto& receipt : receipts) {
        if (receipt.id == id) {
            return &receipt;
        }
    }
    return nullptr;
}

const ExceptionGrant* PlanRecord::find_exception(const ExceptionId& id) const noexcept {
    for (const auto& grant : exceptions) {
        if (grant.id == id) {
            return &grant;
        }
    }
    return nullptr;
}

bool PlanRecord::work_stopped() const noexcept {
    return std::any_of(receipts.begin(), receipts.end(),
                       [](const Receipt& receipt) { return receipt.kind == ReceiptKind::WorkStopObserved; });
}

bool PlanRecord::work_completed() const noexcept {
    return std::any_of(receipts.begin(), receipts.end(), [](const Receipt& receipt) {
        return receipt.kind == ReceiptKind::MaintenanceVerifiedObserved;
    });
}

std::vector<const Receipt*> PlanRecord::receipts_for(const ObligationId& id) const {
    std::vector<const Receipt*> result;
    for (const auto& receipt : receipts) {
        if (receipt.obligation == id) {
            result.push_back(&receipt);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

void RestorationReport::finalize() {
    std::sort(outstanding.begin(), outstanding.end());
    outstanding.erase(std::unique(outstanding.begin(), outstanding.end()), outstanding.end());
    std::sort(violated_protections.begin(), violated_protections.end());
    violated_protections.erase(std::unique(violated_protections.begin(), violated_protections.end()),
                               violated_protections.end());
    std::sort(redundancy_below_requirement.begin(), redundancy_below_requirement.end());
    redundancy_below_requirement.erase(
        std::unique(redundancy_below_requirement.begin(), redundancy_below_requirement.end()),
        redundancy_below_requirement.end());
    std::sort(conditions.begin(), conditions.end());
    digest = detail::digest_of(*this);
}

void CompletionReport::finalize() {
    std::sort(satisfied.begin(), satisfied.end());
    satisfied.erase(std::unique(satisfied.begin(), satisfied.end()), satisfied.end());
    digest = detail::digest_of(*this);
}

void RecoveryReport::finalize() {
    std::sort(notes.begin(), notes.end());
    notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
    std::sort(outstanding.begin(), outstanding.end());
    outstanding.erase(std::unique(outstanding.begin(), outstanding.end()), outstanding.end());
    digest = detail::digest_of(*this);
}

void Explanation::finalize() {
    std::sort(blockers.begin(), blockers.end());
    blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
    std::sort(reasons.begin(), reasons.end());
    reasons.erase(std::unique(reasons.begin(), reasons.end()), reasons.end());
    std::sort(next_steps.begin(), next_steps.end());
    next_steps.erase(std::unique(next_steps.begin(), next_steps.end()), next_steps.end());
    std::sort(obligations.begin(), obligations.end(), [](const ObligationStatus& lhs, const ObligationStatus& rhs) {
        const auto left = std::tuple{static_cast<std::uint8_t>(lhs.stage), static_cast<std::uint8_t>(lhs.kind),
                                     lhs.target, lhs.id};
        const auto right = std::tuple{static_cast<std::uint8_t>(rhs.stage), static_cast<std::uint8_t>(rhs.kind),
                                      rhs.target, rhs.id};
        return left < right;
    });
    digest = detail::digest_of(*this);
}

}  // namespace mc

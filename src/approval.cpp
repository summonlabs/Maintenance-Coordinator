// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.

#include "mc/approval.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "serialize.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// Exception grants
// ---------------------------------------------------------------------------

void ExceptionGrant::canonicalize() {
    std::sort(waived.begin(), waived.end(),
              [](ErrorCode lhs, ErrorCode rhs) {
                  return static_cast<std::uint16_t>(lhs) < static_cast<std::uint16_t>(rhs);
              });
    waived.erase(std::unique(waived.begin(), waived.end()), waived.end());
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
}

void ExceptionGrant::finalize() {
    canonicalize();
    digest = detail::digest_of(*this);
}

Status ExceptionGrant::validate() const {
    Status primary;
    if (id.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "exception grant has no identity", "exception"));
    }
    if (plan.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "exception grant is not bound to a plan", "exception",
                           id.name()));
    }
    if (granted_by.empty() || !is_valid_identifier(granted_by)) {
        primary.merge(fail(ErrorCode::InvalidArgument, "exception grant has no accountable grantor", "exception",
                           id.name()));
    }
    if (justification.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "exception grant states no justification", "exception",
                           id.name()));
    }
    if (waived.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "exception grant waives no condition", "exception",
                           id.name()));
    }
    if (targets.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "exception grant names no target", "exception", id.name()));
    }
    if (!is_set(granted_at) || !is_set(expires_at)) {
        primary.merge(fail(ErrorCode::InvalidArgument, "exception grant is not bounded in time", "exception",
                           id.name()));
    } else if (expires_at <= granted_at) {
        primary.merge(fail(ErrorCode::InvalidArgument, "exception grant expires before it is granted", "exception",
                           id.name()));
    }
    for (const auto code : waived) {
        if (!is_waivable_condition(code)) {
            primary.merge(fail(ErrorCode::HardInterlock,
                               "exception grant attempts to waive a condition that is not waivable", "exception",
                               std::string(to_string(code))));
        }
    }
    if (digest.is_zero()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "exception grant digest has not been computed", "exception",
                           id.name()));
    }
    return primary;
}

bool ExceptionGrant::covers(ErrorCode condition, const TargetRef& target, Timestamp instant) const noexcept {
    if (!active_at(instant)) {
        return false;
    }
    if (std::find(waived.begin(), waived.end(), condition) == waived.end()) {
        return false;
    }
    return std::binary_search(targets.begin(), targets.end(), target);
}

// ---------------------------------------------------------------------------
// Precondition report
// ---------------------------------------------------------------------------

void PreconditionReport::canonicalize() {
    std::sort(conditions.begin(), conditions.end());
    std::sort(applied_exceptions.begin(), applied_exceptions.end());
    applied_exceptions.erase(std::unique(applied_exceptions.begin(), applied_exceptions.end()),
                             applied_exceptions.end());
}

void PreconditionReport::finalize() {
    canonicalize();
    digest = detail::digest_of(*this);
}

Status PreconditionReport::validate() const {
    Status primary;
    if (!plan_revision.is_set()) {
        primary.merge(fail(ErrorCode::MissingGeneration, "evaluation report is not bound to a revision",
                           "evaluation"));
    }
    if (plan_digest.is_zero()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "evaluation report is not bound to a plan digest",
                           "evaluation"));
    }
    if (!is_set(evaluated_at)) {
        primary.merge(fail(ErrorCode::InvalidArgument, "evaluation report has no evaluation time", "evaluation"));
    }
    if (!facility_epoch.is_set() || !policy_generation.is_set() || !dependency_generation.is_set() ||
        !capacity_generation.is_set() || !topology_generation.is_set() || !maintenance_generation.is_set() ||
        !control_epoch.is_set()) {
        primary.merge(fail(ErrorCode::MissingGeneration, "evaluation report is not bound to a full generation set",
                           "evaluation"));
    }
    if (conditions.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "evaluation report contains no condition", "evaluation"));
    }
    for (std::size_t index = 1; index < conditions.size(); ++index) {
        if (!(conditions[index - 1] < conditions[index])) {
            primary.merge(fail(ErrorCode::LayoutInvalid, "evaluation conditions are not in canonical order",
                               "evaluation"));
            break;
        }
    }
    if (digest.is_zero()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "evaluation report digest has not been computed",
                           "evaluation"));
    }
    return primary;
}

std::vector<ConditionResult> PreconditionReport::blocking(bool only_hard) const {
    std::vector<ConditionResult> result;
    for (const auto& condition : conditions) {
        if (condition.satisfied) {
            continue;
        }
        if (only_hard && !condition.hard) {
            continue;
        }
        result.push_back(condition);
    }
    return result;
}

bool PreconditionReport::has(ErrorCode condition) const noexcept {
    return find(condition) != nullptr;
}

const ConditionResult* PreconditionReport::find(ErrorCode condition) const noexcept {
    for (const auto& entry : conditions) {
        if (entry.condition == condition) {
            return &entry;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Approval
// ---------------------------------------------------------------------------

void ApprovalRecord::canonicalize() {
    std::sort(exceptions.begin(), exceptions.end());
    exceptions.erase(std::unique(exceptions.begin(), exceptions.end()), exceptions.end());
}

void ApprovalRecord::finalize() {
    canonicalize();
    digest = detail::digest_of(*this);
}

Status ApprovalRecord::validate() const {
    Status primary;
    if (id.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "approval has no identity", "approval"));
    }
    if (plan.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "approval is not bound to a plan", "approval", id.name()));
    }
    if (!plan_revision.is_set()) {
        primary.merge(fail(ErrorCode::MissingGeneration, "approval is not bound to a plan revision", "approval",
                           id.name()));
    }
    if (approved_by.empty() || !is_valid_identifier(approved_by)) {
        primary.merge(fail(ErrorCode::InvalidArgument, "approval has no accountable approver", "approval",
                           id.name()));
    }
    if (approval_evidence.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "approval cites no evidence", "approval", id.name()));
    }
    if (!is_set(approved_at) || !is_set(expires_at)) {
        primary.merge(fail(ErrorCode::InvalidArgument, "approval is not bounded in time", "approval", id.name()));
    } else if (expires_at <= approved_at) {
        primary.merge(fail(ErrorCode::InvalidArgument, "approval expires before it is granted", "approval",
                           id.name()));
    }
    if (plan_digest.is_zero() || facility_digest.is_zero() || policy_digest.is_zero() ||
        dependency_digest.is_zero() || evaluation_digest.is_zero()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "approval is not bound to a complete digest set", "approval",
                           id.name()));
    }
    if (!facility_epoch.is_set() || !policy_generation.is_set() || !dependency_generation.is_set() ||
        !capacity_generation.is_set() || !topology_generation.is_set() || !maintenance_generation.is_set() ||
        !control_epoch.is_set()) {
        primary.merge(fail(ErrorCode::MissingGeneration, "approval is not bound to a full generation set",
                           "approval", id.name()));
    }
    if (digest.is_zero()) {
        primary.merge(
            fail(ErrorCode::InvalidArgument, "approval digest has not been computed", "approval", id.name()));
    }
    return primary;
}

}  // namespace mc

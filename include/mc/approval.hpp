// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Precondition evaluation, approval binding and explicit exception grants.
//
// An approval is a claim about one exact revision of one plan evaluated against
// one exact set of generations.  It is not a durable permission: every gate
// re-derives the generation set and compares it with the approval, so any
// incident, dependency, redundancy, capacity, policy or maintenance generation
// change fences the stale approval instead of inheriting it.
//
// An exception is the only way a policy condition may be set aside.  It is
// explicit (a named condition), scoped (targets within the plan), attributable
// (a named grantor and justification), expiring (an end instant) and it can
// never cover a hard safety interlock.

#ifndef MC_APPROVAL_HPP
#define MC_APPROVAL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "mc/digest.hpp"
#include "mc/ident.hpp"
#include "mc/scope.hpp"
#include "mc/status.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------

// The outcome of one precondition check.  A condition is reported even when it
// passes, so the report is complete evidence rather than a list of complaints.
struct ConditionResult {
    ErrorCode condition{ErrorCode::Ok};
    bool satisfied{false};
    bool hard{false};       // a hard interlock: never waivable, always blocking
    bool waivable{false};   // an explicit exception grant may set this aside
    bool measured{true};    // false when the plant model did not measure it
    ExceptionId waiver;     // the grant that waived it, when waived
    std::string subject;    // the object the condition is about
    std::string detail;     // the precise fact
    std::int64_t observed{0};
    std::int64_t required{0};

    [[nodiscard]] friend bool operator==(const ConditionResult&, const ConditionResult&) noexcept = default;
    [[nodiscard]] friend std::strong_ordering operator<=>(const ConditionResult& lhs, const ConditionResult& rhs) noexcept {
        if (lhs.condition != rhs.condition) {
            return static_cast<std::uint16_t>(lhs.condition) <=> static_cast<std::uint16_t>(rhs.condition);
        }
        if (lhs.subject != rhs.subject) {
            return lhs.subject <=> rhs.subject;
        }
        return lhs.detail <=> rhs.detail;
    }
};

class PreconditionReport {
public:
    Revision plan_revision;
    Digest plan_digest;
    Timestamp evaluated_at{kNoTimestamp};

    // The exact view the evaluation was computed against.
    FacilityEpoch facility_epoch;
    ControlEpoch control_epoch;
    PolicyGeneration policy_generation;
    DependencyGeneration dependency_generation;
    CapacityGeneration capacity_generation;
    TopologyGeneration topology_generation;
    MaintenanceGeneration maintenance_generation;
    Digest facility_digest;
    Digest policy_digest;
    Digest dependency_digest;

    std::vector<ConditionResult> conditions;  // canonical order, satisfied ones included
    std::vector<ExceptionId> applied_exceptions;
    bool satisfied{false};                     // every condition satisfied after waivers
    bool satisfied_without_exceptions{false};  // every condition satisfied on its own
    Digest digest;

    void canonicalize();
    void finalize();
    [[nodiscard]] Status validate() const;

    [[nodiscard]] std::vector<ConditionResult> blocking(bool only_hard = false) const;
    [[nodiscard]] bool has(ErrorCode condition) const noexcept;
    [[nodiscard]] const ConditionResult* find(ErrorCode condition) const noexcept;

    [[nodiscard]] friend bool operator==(const PreconditionReport&, const PreconditionReport&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Approval
// ---------------------------------------------------------------------------

class ApprovalRecord {
public:
    ApprovalId id;
    PlanId plan;
    Revision plan_revision;
    std::string approved_by;
    std::string approval_evidence;  // the change record the approval cites
    std::string witness;            // second approver when policy demands duality
    Timestamp approved_at{kNoTimestamp};
    Timestamp expires_at{kNoTimestamp};

    // The full binding set.  All of these must still hold at every later gate.
    Digest plan_digest;
    Digest facility_digest;
    Digest policy_digest;
    Digest dependency_digest;
    Digest evaluation_digest;
    Digest obligation_set_digest;  // zero until obligations are derived
    FacilityEpoch facility_epoch;
    ControlEpoch control_epoch;
    PolicyGeneration policy_generation;
    DependencyGeneration dependency_generation;
    CapacityGeneration capacity_generation;
    TopologyGeneration topology_generation;
    MaintenanceGeneration maintenance_generation;
    std::vector<ExceptionId> exceptions;  // canonical order
    Digest digest;

    void canonicalize();
    void finalize();
    [[nodiscard]] Status validate() const;

    [[nodiscard]] bool expired_at(Timestamp instant) const noexcept {
        return !is_set(expires_at) || instant > expires_at;
    }

    [[nodiscard]] friend bool operator==(const ApprovalRecord&, const ApprovalRecord&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------

class ExceptionGrant {
public:
    ExceptionId id;
    PlanId plan;
    std::vector<ErrorCode> waived;     // canonical order; soft conditions only
    std::vector<TargetRef> targets;    // canonical order; must lie within the plan scope
    std::string granted_by;
    std::string justification;
    Timestamp granted_at{kNoTimestamp};
    Timestamp expires_at{kNoTimestamp};
    Digest digest;

    void canonicalize();
    void finalize();
    [[nodiscard]] Status validate() const;

    [[nodiscard]] bool active_at(Timestamp instant) const noexcept {
        return is_set(granted_at) && is_set(expires_at) && instant >= granted_at && instant <= expires_at;
    }
    [[nodiscard]] bool covers(ErrorCode condition, const TargetRef& target, Timestamp instant) const noexcept;

    [[nodiscard]] friend bool operator==(const ExceptionGrant&, const ExceptionGrant&) noexcept = default;
};

}  // namespace mc

#endif  // MC_APPROVAL_HPP

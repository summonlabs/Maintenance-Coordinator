// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.

#include "mc/status.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

namespace mc {
namespace {

struct CodeName {
    ErrorCode code;
    std::string_view name;
};

// The spelling of every code is part of the CLI contract, so the table is
// explicit and ordered by value.
constexpr std::array<CodeName, 72> kCodeNames{{
    {ErrorCode::Ok, "ok"},
    {ErrorCode::InvalidUsage, "InvalidUsage"},
    {ErrorCode::UnknownCommand, "UnknownCommand"},
    {ErrorCode::MissingArgument, "MissingArgument"},
    {ErrorCode::InvalidArgument, "InvalidArgument"},
    {ErrorCode::MalformedInput, "MalformedInput"},
    {ErrorCode::UnsupportedFormatVersion, "UnsupportedFormatVersion"},
    {ErrorCode::UnknownPlan, "UnknownPlan"},
    {ErrorCode::UnknownTarget, "UnknownTarget"},
    {ErrorCode::DuplicateIdentity, "DuplicateIdentity"},
    {ErrorCode::IdentityMismatch, "IdentityMismatch"},
    {ErrorCode::UnknownObligation, "UnknownObligation"},
    {ErrorCode::UnknownAsset, "UnknownAsset"},
    {ErrorCode::StaleRevision, "StaleRevision"},
    {ErrorCode::StaleAuthority, "StaleAuthority"},
    {ErrorCode::StalePolicyGeneration, "StalePolicyGeneration"},
    {ErrorCode::StaleDependencyGeneration, "StaleDependencyGeneration"},
    {ErrorCode::StaleCapacityGeneration, "StaleCapacityGeneration"},
    {ErrorCode::StaleTopologyGeneration, "StaleTopologyGeneration"},
    {ErrorCode::StaleMaintenanceGeneration, "StaleMaintenanceGeneration"},
    {ErrorCode::StaleFacilityEpoch, "StaleFacilityEpoch"},
    {ErrorCode::StaleObservation, "StaleObservation"},
    {ErrorCode::MissingGeneration, "MissingGeneration"},
    {ErrorCode::StaleApproval, "StaleApproval"},
    {ErrorCode::StalePlanDigest, "StalePlanDigest"},
    {ErrorCode::IllegalPhase, "IllegalPhase"},
    {ErrorCode::AlreadyTerminal, "AlreadyTerminal"},
    {ErrorCode::NotApproved, "NotApproved"},
    {ErrorCode::NotReady, "NotReady"},
    {ErrorCode::ObligationsOutstanding, "ObligationsOutstanding"},
    {ErrorCode::CompletionEvidenceMissing, "CompletionEvidenceMissing"},
    {ErrorCode::RestorationOutstanding, "RestorationOutstanding"},
    {ErrorCode::ProtectedObligationViolated, "ProtectedObligationViolated"},
    {ErrorCode::PlanBlocked, "PlanBlocked"},
    {ErrorCode::RecoveryRequired, "RecoveryRequired"},
    {ErrorCode::WindowNotOpen, "WindowNotOpen"},
    {ErrorCode::ProgressOutOfOrder, "ProgressOutOfOrder"},
    {ErrorCode::ProtectedWindowActive, "ProtectedWindowActive"},
    {ErrorCode::BlackoutPeriodActive, "BlackoutPeriodActive"},
    {ErrorCode::RedundancyInsufficient, "RedundancyInsufficient"},
    {ErrorCode::SpareCapacityInsufficient, "SpareCapacityInsufficient"},
    {ErrorCode::PowerHeadroomInsufficient, "PowerHeadroomInsufficient"},
    {ErrorCode::CoolingHeadroomInsufficient, "CoolingHeadroomInsufficient"},
    {ErrorCode::ActiveIncident, "ActiveIncident"},
    {ErrorCode::LifecycleStateInvalid, "LifecycleStateInvalid"},
    {ErrorCode::ServiceClassViolation, "ServiceClassViolation"},
    {ErrorCode::HardInterlock, "HardInterlock"},
    {ErrorCode::ExceptionExpired, "ExceptionExpired"},
    {ErrorCode::ExceptionInvalid, "ExceptionInvalid"},
    {ErrorCode::ConcurrentMaintenanceConflict, "ConcurrentMaintenanceConflict"},
    {ErrorCode::PersonnelEvidenceMissing, "PersonnelEvidenceMissing"},
    {ErrorCode::ApprovalEvidenceMissing, "ApprovalEvidenceMissing"},
    {ErrorCode::ScopeViolation, "ScopeViolation"},
    {ErrorCode::WindowExpired, "WindowExpired"},
    {ErrorCode::IoError, "IoError"},
    {ErrorCode::LockHeld, "LockHeld"},
    {ErrorCode::LockError, "LockError"},
    {ErrorCode::RecordCorrupt, "RecordCorrupt"},
    {ErrorCode::RecordTruncated, "RecordTruncated"},
    {ErrorCode::ChecksumMismatch, "ChecksumMismatch"},
    {ErrorCode::TrailingBytes, "TrailingBytes"},
    {ErrorCode::ReservedFieldViolation, "ReservedFieldViolation"},
    {ErrorCode::LayoutInvalid, "LayoutInvalid"},
    {ErrorCode::CommitSequenceRegression, "CommitSequenceRegression"},
    {ErrorCode::PublicationFailed, "PublicationFailed"},
    {ErrorCode::PathInvalid, "PathInvalid"},
    {ErrorCode::PathTraversalRejected, "PathTraversalRejected"},
    {ErrorCode::ReadOnlyStore, "ReadOnlyStore"},
    {ErrorCode::LimitExceeded, "LimitExceeded"},
    {ErrorCode::DuplicateOperation, "DuplicateOperation"},
    {ErrorCode::ReplayIntentMismatch, "ReplayIntentMismatch"},
    {ErrorCode::InternalError, "InternalError"},
}};

}  // namespace

std::string_view to_string(ErrorCode code) noexcept {
    for (const auto& entry : kCodeNames) {
        if (entry.code == code) {
            return entry.name;
        }
    }
    return "UnknownErrorCode";
}

bool parse_error_code(std::string_view name, ErrorCode& out) noexcept {
    if (name == "ok") {
        return false;
    }
    for (const auto& entry : kCodeNames) {
        if (entry.code != ErrorCode::Ok && entry.name == name) {
            out = entry.code;
            return true;
        }
    }
    return false;
}

bool is_waivable_condition(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::ProtectedWindowActive:
        case ErrorCode::BlackoutPeriodActive:
        case ErrorCode::RedundancyInsufficient:
        case ErrorCode::SpareCapacityInsufficient:
        case ErrorCode::PowerHeadroomInsufficient:
        case ErrorCode::CoolingHeadroomInsufficient:
        case ErrorCode::ActiveIncident:
        case ErrorCode::ServiceClassViolation:
        case ErrorCode::PersonnelEvidenceMissing:
        case ErrorCode::ApprovalEvidenceMissing:
            return true;
        default:
            return false;
    }
}

void Status::merge(const Status& other) {
    if (other.ok()) {
        return;
    }
    if (ok()) {
        *this = other;
        return;
    }
    if (static_cast<std::uint16_t>(other.code_) < static_cast<std::uint16_t>(code_)) {
        Status promoted = other;
        promoted.suppressed_.insert(promoted.suppressed_.end(), suppressed_.begin(), suppressed_.end());
        promoted.suppressed_.push_back(*this);
        suppressed_.clear();
        *this = std::move(promoted);
        return;
    }
    if (other.code_ == code_ && other.message_ == message_ && other.subject_ == subject_ &&
        other.detail_ == detail_) {
        return;  // identical faults are collapsed so reports stay readable
    }
    suppressed_.push_back(other);
}

std::string Status::render() const {
    if (ok()) {
        return "ok";
    }
    std::string text;
    text.reserve(message_.size() + subject_.size() + detail_.size() + 32U);
    text += to_string(code_);
    if (!message_.empty()) {
        text += ": ";
        text += message_;
    }
    if (!subject_.empty()) {
        text += " [";
        text += subject_;
        text += ']';
    }
    if (!detail_.empty()) {
        text += " (";
        text += detail_;
        text += ')';
    }
    for (const auto& extra : suppressed_) {
        text += " | suppressed: ";
        text += extra.render();
    }
    return text;
}

Status fail(ErrorCode code, std::string message) {
    if (code == ErrorCode::Ok) {
        code = ErrorCode::InternalError;
    }
    return Status(code, std::move(message));
}

Status fail(ErrorCode code, std::string message, std::string subject) {
    if (code == ErrorCode::Ok) {
        code = ErrorCode::InternalError;
    }
    return Status(code, std::move(message), std::move(subject));
}

Status fail(ErrorCode code, std::string message, std::string subject, std::string detail) {
    if (code == ErrorCode::Ok) {
        code = ErrorCode::InternalError;
    }
    return Status(code, std::move(message), std::move(subject), std::move(detail));
}

}  // namespace mc

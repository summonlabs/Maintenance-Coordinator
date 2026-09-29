// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.

#include "mc/obligation.hpp"

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

constexpr std::array<std::pair<std::string_view, unsigned>, 16> kObligationKinds{{
    {"asi-drain", 0}, {"asi-quiesce", 1}, {"dfi-drain-traffic", 2}, {"dfi-isolate-path", 3},
    {"power-isolate", 4}, {"cooling-adjust", 5}, {"lifecycle-transition", 6}, {"personnel-on-site", 7},
    {"approval-witness", 8}, {"maintenance-verified", 9}, {"work-stop-confirmed", 10}, {"drain-release", 11},
    {"power-restore", 12}, {"cooling-restore", 13}, {"traffic-restore", 14}, {"redundancy-restore", 15},
}};

constexpr std::array<std::pair<std::string_view, unsigned>, 5> kAuthorities{{
    {"asi", 0}, {"dfi", 1}, {"dccp-plant", 2}, {"dccp-inventory", 3}, {"operator", 4},
}};

constexpr std::array<std::pair<std::string_view, unsigned>, 3> kStages{{
    {"pre-drain", 0}, {"isolation", 1}, {"completion", 2},
}};

constexpr std::array<std::pair<std::string_view, unsigned>, 6> kDispositions{{
    {"outstanding", 0}, {"acknowledged", 1}, {"satisfied", 2}, {"failed", 3}, {"waived", 4}, {"not-applicable", 5},
}};

constexpr std::array<std::pair<std::string_view, unsigned>, 19> kReceiptKinds{{
    {"drain-requested", 0}, {"drain-acknowledged", 1}, {"drain-observed", 2}, {"quiesce-observed", 3},
    {"traffic-drain-observed", 4}, {"path-isolation-observed", 5}, {"power-isolation-observed", 6},
    {"cooling-adjustment-observed", 7}, {"lifecycle-transition-observed", 8}, {"personnel-on-site-observed", 9},
    {"approval-witness-observed", 10}, {"maintenance-verified-observed", 11}, {"work-stop-observed", 12},
    {"drain-release-observed", 13}, {"power-restore-observed", 14}, {"cooling-restore-observed", 15},
    {"traffic-restore-observed", 16}, {"redundancy-restore-observed", 17}, {"failure-observed", 18},
}};

[[nodiscard]] bool is_canonical(const std::vector<Obligation>& items) {
    for (std::size_t index = 1; index < items.size(); ++index) {
        const auto& left = items[index - 1];
        const auto& right = items[index];
        const auto left_key = std::tuple{static_cast<std::uint8_t>(stage_of(left.kind)),
                                         static_cast<std::uint8_t>(left.kind), left.target, left.id};
        const auto right_key = std::tuple{static_cast<std::uint8_t>(stage_of(right.kind)),
                                          static_cast<std::uint8_t>(right.kind), right.target, right.id};
        if (!(left_key < right_key)) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string_view to_string(ObligationKind kind) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(kind), kObligationKinds);
}

bool parse_obligation_kind(std::string_view text, ObligationKind& out) noexcept {
    unsigned raw = 0;
    if (!detail::parse_enum_name(text, kObligationKinds, raw)) {
        return false;
    }
    out = static_cast<ObligationKind>(raw);
    return true;
}

std::string_view to_string(ObligationAuthority authority) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(authority), kAuthorities);
}

bool parse_obligation_authority(std::string_view text, ObligationAuthority& out) noexcept {
    unsigned raw = 0;
    if (!detail::parse_enum_name(text, kAuthorities, raw)) {
        return false;
    }
    out = static_cast<ObligationAuthority>(raw);
    return true;
}

std::string_view to_string(ObligationStage stage) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(stage), kStages);
}

std::string_view to_string(Disposition disposition) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(disposition), kDispositions);
}

std::string_view to_string(ReceiptKind kind) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(kind), kReceiptKinds);
}

bool parse_receipt_kind(std::string_view text, ReceiptKind& out) noexcept {
    unsigned raw = 0;
    if (!detail::parse_enum_name(text, kReceiptKinds, raw)) {
        return false;
    }
    out = static_cast<ReceiptKind>(raw);
    return true;
}

ObligationStage stage_of(ObligationKind kind) noexcept {
    switch (kind) {
        case ObligationKind::AsiDrain:
        case ObligationKind::AsiQuiesce:
        case ObligationKind::DfiDrainTraffic:
        case ObligationKind::PersonnelOnSite:
        case ObligationKind::ApprovalWitness:
            return ObligationStage::PreDrain;
        case ObligationKind::DfiIsolatePath:
        case ObligationKind::PowerIsolate:
        case ObligationKind::CoolingAdjust:
        case ObligationKind::LifecycleTransition:
            return ObligationStage::Isolation;
        case ObligationKind::MaintenanceVerified:
        case ObligationKind::WorkStopConfirmed:
        case ObligationKind::DrainRelease:
        case ObligationKind::PowerRestore:
        case ObligationKind::CoolingRestore:
        case ObligationKind::TrafficRestore:
        case ObligationKind::RedundancyRestore:
        default:
            return ObligationStage::Completion;
    }
}

ObligationAuthority authority_of(ObligationKind kind) noexcept {
    switch (kind) {
        case ObligationKind::AsiDrain:
        case ObligationKind::AsiQuiesce:
            return ObligationAuthority::Asi;
        case ObligationKind::DfiDrainTraffic:
        case ObligationKind::DfiIsolatePath:
            return ObligationAuthority::Dfi;
        case ObligationKind::PowerIsolate:
        case ObligationKind::CoolingAdjust:
        case ObligationKind::PowerRestore:
        case ObligationKind::CoolingRestore:
            return ObligationAuthority::DccpPlant;
        case ObligationKind::LifecycleTransition:
            return ObligationAuthority::DccpInventory;
        case ObligationKind::PersonnelOnSite:
        case ObligationKind::ApprovalWitness:
            return ObligationAuthority::Operator;
        case ObligationKind::MaintenanceVerified:
        case ObligationKind::WorkStopConfirmed:
        case ObligationKind::DrainRelease:
        case ObligationKind::TrafficRestore:
        case ObligationKind::RedundancyRestore:
        default:
            return ObligationAuthority::DccpPlant;
    }
}

ReceiptKind satisfying_receipt(ObligationKind kind) noexcept {
    switch (kind) {
        case ObligationKind::AsiDrain:
            return ReceiptKind::DrainObserved;
        case ObligationKind::AsiQuiesce:
            return ReceiptKind::QuiesceObserved;
        case ObligationKind::DfiDrainTraffic:
            return ReceiptKind::TrafficDrainObserved;
        case ObligationKind::DfiIsolatePath:
            return ReceiptKind::PathIsolationObserved;
        case ObligationKind::PowerIsolate:
            return ReceiptKind::PowerIsolationObserved;
        case ObligationKind::CoolingAdjust:
            return ReceiptKind::CoolingAdjustmentObserved;
        case ObligationKind::LifecycleTransition:
            return ReceiptKind::LifecycleTransitionObserved;
        case ObligationKind::PersonnelOnSite:
            return ReceiptKind::PersonnelOnSiteObserved;
        case ObligationKind::ApprovalWitness:
            return ReceiptKind::ApprovalWitnessObserved;
        case ObligationKind::MaintenanceVerified:
            return ReceiptKind::MaintenanceVerifiedObserved;
        case ObligationKind::WorkStopConfirmed:
            return ReceiptKind::WorkStopObserved;
        case ObligationKind::DrainRelease:
            return ReceiptKind::DrainReleaseObserved;
        case ObligationKind::PowerRestore:
            return ReceiptKind::PowerRestoreObserved;
        case ObligationKind::CoolingRestore:
            return ReceiptKind::CoolingRestoreObserved;
        case ObligationKind::TrafficRestore:
            return ReceiptKind::TrafficRestoreObserved;
        case ObligationKind::RedundancyRestore:
            return ReceiptKind::RedundancyRestoreObserved;
        default:
            return ReceiptKind::FailureObserved;
    }
}

bool is_observation(ReceiptKind kind) noexcept {
    return kind != ReceiptKind::DrainRequested && kind != ReceiptKind::DrainAcknowledged &&
           kind != ReceiptKind::FailureObserved;
}

bool is_request_or_acknowledgement(ReceiptKind kind) noexcept {
    return kind == ReceiptKind::DrainRequested || kind == ReceiptKind::DrainAcknowledged;
}

// ---------------------------------------------------------------------------
// Obligation
// ---------------------------------------------------------------------------

Status Obligation::validate() const {
    Status primary;
    if (id.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "obligation has no identity", "obligation"));
    }
    if (target.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "obligation names no target", "obligation", id.name()));
    }
    if (requirement.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "obligation states no requirement", "obligation", id.name()));
    }
    if (authority != authority_of(kind)) {
        primary.merge(fail(ErrorCode::IdentityMismatch,
                           "obligation is placed on an authority that does not own this obligation kind",
                           "obligation", id.name()));
    }
    return primary;
}

Digest Obligation::compute_digest() const {
    return detail::digest_of(*this);
}

void ObligationSet::canonicalize() {
    std::sort(obligations.begin(), obligations.end(), [](const Obligation& lhs, const Obligation& rhs) {
        const auto left = std::tuple{static_cast<std::uint8_t>(stage_of(lhs.kind)),
                                     static_cast<std::uint8_t>(lhs.kind), lhs.target, lhs.id};
        const auto right = std::tuple{static_cast<std::uint8_t>(stage_of(rhs.kind)),
                                      static_cast<std::uint8_t>(rhs.kind), rhs.target, rhs.id};
        return left < right;
    });
}

void ObligationSet::finalize() {
    canonicalize();
    for (auto& obligation : obligations) {
        obligation.finalize();
    }
    digest = detail::digest_of(*this);
}

Status ObligationSet::validate() const {
    Status primary;
    if (!is_canonical(obligations)) {
        primary.merge(fail(ErrorCode::LayoutInvalid, "obligations are not in canonical order", "obligation-set"));
    }
    for (const auto& obligation : obligations) {
        primary.merge(obligation.validate());
    }
    for (std::size_t index = 1; index < obligations.size(); ++index) {
        if (obligations[index - 1].id == obligations[index].id) {
            primary.merge(fail(ErrorCode::DuplicateIdentity, "two obligations share one identity", "obligation",
                               obligations[index].id.name()));
        }
    }
    if (digest.is_zero()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "obligation set digest has not been computed",
                           "obligation-set"));
    }
    return primary;
}

const Obligation* ObligationSet::find(const ObligationId& id) const noexcept {
    // The canonical order is (stage, kind, target, id), so identity lookup is a
    // scan: sets here are small and a mistaken binary search over the wrong key
    // would be a correctness bug that no test would catch by accident.
    for (const auto& obligation : obligations) {
        if (obligation.id == id) {
            return &obligation;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Receipt
// ---------------------------------------------------------------------------

Status Receipt::validate() const {
    Status primary;
    if (id.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "receipt has no identity", "receipt"));
    }
    if (obligation.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "receipt names no obligation", "receipt", id.name()));
    }
    if (issuer.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "receipt has no accountable issuer", "receipt", id.name()));
    } else if (!is_valid_identifier(issuer)) {
        primary.merge(fail(ErrorCode::InvalidArgument, "receipt issuer is not a valid identity", "receipt",
                           id.name()));
    }
    if (!observation_sequence.is_set()) {
        primary.merge(fail(ErrorCode::MissingGeneration, "receipt carries no observation sequence", "receipt",
                           id.name()));
    }
    if (!plan_revision.is_set()) {
        primary.merge(fail(ErrorCode::MissingGeneration, "receipt is not bound to a plan revision", "receipt",
                           id.name()));
    }
    if (!is_set(observed_at)) {
        primary.merge(fail(ErrorCode::InvalidArgument, "receipt does not state when the fact was observed",
                           "receipt", id.name()));
    }
    if (!is_set(ingested_at)) {
        primary.merge(fail(ErrorCode::InvalidArgument, "receipt does not state when it was ingested", "receipt",
                           id.name()));
    }
    if (evidence.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "receipt carries no provenance", "receipt", id.name()));
    }
    if (evidence_digest.is_zero()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "receipt carries no evidence digest", "receipt", id.name()));
    }
    if (obligation_digest.is_zero()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "receipt is not bound to an obligation digest", "receipt",
                           id.name()));
    }
    return primary;
}

Digest Receipt::compute_digest() const {
    return detail::digest_of(*this);
}

// ---------------------------------------------------------------------------
// Obligation status report
// ---------------------------------------------------------------------------

void ObligationStatusReport::finalize() {
    std::sort(statuses.begin(), statuses.end(), [](const ObligationStatus& lhs, const ObligationStatus& rhs) {
        const auto left = std::tuple{static_cast<std::uint8_t>(lhs.stage), static_cast<std::uint8_t>(lhs.kind),
                                     lhs.target, lhs.id};
        const auto right = std::tuple{static_cast<std::uint8_t>(rhs.stage), static_cast<std::uint8_t>(rhs.kind),
                                      rhs.target, rhs.id};
        return left < right;
    });
    for (auto& status : statuses) {
        std::sort(status.supporting_receipts.begin(), status.supporting_receipts.end());
        status.supporting_receipts.erase(
            std::unique(status.supporting_receipts.begin(), status.supporting_receipts.end()),
            status.supporting_receipts.end());
    }
    digest = detail::digest_of(*this);
}

std::vector<ObligationId> ObligationStatusReport::outstanding(ObligationStage stage) const {
    std::vector<ObligationId> result;
    for (const auto& status : statuses) {
        if (status.stage != stage) {
            continue;
        }
        if (status.disposition == Disposition::Satisfied || status.disposition == Disposition::Waived ||
            status.disposition == Disposition::NotApplicable) {
            continue;
        }
        result.push_back(status.id);
    }
    return result;
}

const ObligationStatus* ObligationStatusReport::find(const ObligationId& id) const noexcept {
    for (const auto& status : statuses) {
        if (status.id == id) {
            return &status;
        }
    }
    return nullptr;
}

}  // namespace mc

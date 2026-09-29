// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.

#include "mc/scope.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "enum_table.hpp"
#include "mc/digest.hpp"

namespace mc {
namespace {

constexpr std::array<std::pair<std::string_view, unsigned>, 6> kTargetKinds{{
    {"site", 0}, {"rack", 1}, {"asset", 2}, {"power-domain", 3}, {"cooling-zone", 4}, {"fabric-segment", 5},
}};

constexpr std::array<std::pair<std::string_view, unsigned>, 10> kActivities{{
    {"inspection", 0}, {"hardware-repair", 1}, {"hardware-upgrade", 2}, {"firmware-upgrade", 3},
    {"power-work", 4}, {"cooling-work", 5}, {"network-work", 6}, {"rack-install", 7},
    {"decommission", 8}, {"data-wipe", 9},
}};

constexpr std::array<std::pair<std::string_view, unsigned>, 3> kPriorities{{
    {"routine", 0}, {"elevated", 1}, {"emergency", 2},
}};

constexpr std::array<std::pair<std::string_view, unsigned>, 4> kRisks{{
    {"low", 0}, {"moderate", 1}, {"high", 2}, {"critical", 3},
}};

}  // namespace

std::string_view to_string(TargetKind kind) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(kind), kTargetKinds);
}

bool parse_target_kind(std::string_view text, TargetKind& out) noexcept {
    unsigned raw = 0;
    if (!detail::parse_enum_name(text, kTargetKinds, raw)) {
        return false;
    }
    out = static_cast<TargetKind>(raw);
    return true;
}

std::string_view to_string(MaintenanceActivity activity) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(activity), kActivities);
}

bool parse_maintenance_activity(std::string_view text, MaintenanceActivity& out) noexcept {
    unsigned raw = 0;
    if (!detail::parse_enum_name(text, kActivities, raw)) {
        return false;
    }
    out = static_cast<MaintenanceActivity>(raw);
    return true;
}

std::string_view to_string(PlanPriority priority) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(priority), kPriorities);
}

bool parse_plan_priority(std::string_view text, PlanPriority& out) noexcept {
    unsigned raw = 0;
    if (!detail::parse_enum_name(text, kPriorities, raw)) {
        return false;
    }
    out = static_cast<PlanPriority>(raw);
    return true;
}

std::string_view to_string(ServiceRiskClass risk) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(risk), kRisks);
}

bool parse_service_risk_class(std::string_view text, ServiceRiskClass& out) noexcept {
    unsigned raw = 0;
    if (!detail::parse_enum_name(text, kRisks, raw)) {
        return false;
    }
    out = static_cast<ServiceRiskClass>(raw);
    return true;
}

Result<TargetRef> TargetRef::make(TargetKind kind, std::string_view name) {
    if (auto status = validate_identifier(name, "target"); !status.ok()) {
        return status;
    }
    TargetRef target;
    target.kind_ = kind;
    target.name_ = std::string(name);
    return target;
}

Result<TargetRef> TargetRef::parse(std::string_view text) {
    const std::size_t separator = text.find(':');
    if (separator == std::string_view::npos) {
        return fail(ErrorCode::InvalidArgument, "target reference must be spelled kind:name", "target",
                    std::string(text));
    }
    TargetKind kind{TargetKind::Site};
    if (!parse_target_kind(text.substr(0, separator), kind)) {
        return fail(ErrorCode::InvalidArgument, "target reference names an unknown kind", "target",
                    std::string(text.substr(0, separator)));
    }
    return make(kind, text.substr(separator + 1U));
}

std::string TargetRef::str() const {
    std::string text(to_string(kind_));
    text.push_back(':');
    text += name_;
    return text;
}

void MaintenanceScope::canonicalize() {
    std::sort(targets_.begin(), targets_.end());
    targets_.erase(std::unique(targets_.begin(), targets_.end()), targets_.end());
}

bool MaintenanceScope::contains(const TargetRef& target) const noexcept {
    return std::find(targets_.begin(), targets_.end(), target) != targets_.end();
}

bool MaintenanceScope::intersects(const MaintenanceScope& other) const noexcept {
    for (const auto& left : targets_) {
        if (other.contains(left)) {
            return true;
        }
    }
    return false;
}

}  // namespace mc

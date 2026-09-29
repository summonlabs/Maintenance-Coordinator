// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Maintenance scope, activity classification and window description.
//
// A scope is a canonical, duplicate-free, ordered set of target references.
// Canonical order is part of the contract: it makes digests stable and makes
// "the same invalid request resolves to the same primary error" reachable,
// because nothing in the engine depends on incidental container ordering.

#ifndef MC_SCOPE_HPP
#define MC_SCOPE_HPP

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "mc/ident.hpp"
#include "mc/status.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// Target references
// ---------------------------------------------------------------------------

enum class TargetKind : std::uint8_t {
    Site = 0,
    Rack = 1,
    Asset = 2,
    PowerDomain = 3,
    CoolingZone = 4,
    FabricSegment = 5,
};

[[nodiscard]] std::string_view to_string(TargetKind kind) noexcept;
[[nodiscard]] bool parse_target_kind(std::string_view text, TargetKind& out) noexcept;

// A target reference is a kind plus the opaque name issued by the authority
// that owns that object.  The kind is always explicit, so a rack and an asset
// that happen to share a name are never confused.
class TargetRef {
public:
    TargetRef() = default;

    [[nodiscard]] static Result<TargetRef> make(TargetKind kind, std::string_view name);
    [[nodiscard]] static Result<TargetRef> parse(std::string_view text);

    [[nodiscard]] TargetKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] bool empty() const noexcept { return name_.empty(); }
    [[nodiscard]] std::string str() const;

    [[nodiscard]] friend bool operator==(const TargetRef&, const TargetRef&) noexcept = default;
    [[nodiscard]] friend std::strong_ordering operator<=>(const TargetRef& lhs, const TargetRef& rhs) noexcept {
        if (lhs.kind_ != rhs.kind_) {
            return static_cast<std::uint8_t>(lhs.kind_) <=> static_cast<std::uint8_t>(rhs.kind_);
        }
        return lhs.name_ <=> rhs.name_;
    }

private:
    TargetKind kind_{TargetKind::Site};
    std::string name_;
};

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------

class MaintenanceScope {
public:
    MaintenanceScope() = default;
    explicit MaintenanceScope(std::vector<TargetRef> targets) : targets_(std::move(targets)) {}

    // Sorts by (kind, name) and removes duplicates.  Called by finalize().
    void canonicalize();

    [[nodiscard]] const std::vector<TargetRef>& targets() const noexcept { return targets_; }
    [[nodiscard]] bool empty() const noexcept { return targets_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return targets_.size(); }
    // Membership is a linear scan: a scope is small, and a binary search over a
    // scope that the caller forgot to canonicalise would silently answer "no".
    [[nodiscard]] bool contains(const TargetRef& target) const noexcept;

    // True when the two scopes share at least one target.  Ancestor/descendant
    // relationships between different kinds are resolved by the facility model
    // (see FacilitySnapshot::scope_conflicts), because only the model knows
    // which rack an asset belongs to.
    [[nodiscard]] bool intersects(const MaintenanceScope& other) const noexcept;

    [[nodiscard]] friend bool operator==(const MaintenanceScope&, const MaintenanceScope&) noexcept = default;

private:
    std::vector<TargetRef> targets_;
};

// What kind of physical work the window represents.  This drives obligation
// derivation: not every activity needs a drain, and not every activity needs
// power isolation.
enum class MaintenanceActivity : std::uint8_t {
    Inspection = 0,        // non-invasive, read-only on the plant
    HardwareRepair = 1,    // component replacement or repair
    HardwareUpgrade = 2,   // capacity or capability change
    FirmwareUpgrade = 3,   // code change on an asset, requires quiesce
    PowerWork = 4,         // work on the power path itself
    CoolingWork = 5,       // work on the cooling path itself
    NetworkWork = 6,       // fabric or cabling work
    RackInstall = 7,       // new rack commissioning
    Decommission = 8,      // permanent removal
    DataWipe = 9,          // sanitisation of media
};

[[nodiscard]] std::string_view to_string(MaintenanceActivity activity) noexcept;
[[nodiscard]] bool parse_maintenance_activity(std::string_view text, MaintenanceActivity& out) noexcept;

enum class PlanPriority : std::uint8_t {
    Routine = 0,
    Elevated = 1,
    Emergency = 2,
};

[[nodiscard]] std::string_view to_string(PlanPriority priority) noexcept;
[[nodiscard]] bool parse_plan_priority(std::string_view text, PlanPriority& out) noexcept;

// Risk to the service that the maintenance targets carry.  The coordinator
// classifies and explains risk; it does not own the tenant-facing risk model.
enum class ServiceRiskClass : std::uint8_t {
    Low = 0,
    Moderate = 1,
    High = 2,
    Critical = 3,
};

[[nodiscard]] std::string_view to_string(ServiceRiskClass risk) noexcept;
[[nodiscard]] bool parse_service_risk_class(std::string_view text, ServiceRiskClass& out) noexcept;

struct WindowSpec {
    Timestamp start{kNoTimestamp};  // requested, not authoritative
    Timestamp end{kNoTimestamp};
    bool flexible{false};  // may the coordinator shift the window to satisfy headroom

    [[nodiscard]] bool contains(Timestamp instant) const noexcept {
        return is_set(start) && is_set(end) && instant >= start && instant <= end;
    }
    [[nodiscard]] bool overlaps(Timestamp from, Timestamp to) const noexcept {
        return is_set(start) && is_set(end) && from <= end && to >= start;
    }
    // Unsigned subtraction followed by a clamp: a window whose span does not
    // fit in a signed 64-bit value is reported as the largest representable
    // span, so policy comparison rejects it instead of wrapping silently.
    [[nodiscard]] std::int64_t duration_nanos() const noexcept {
        if (!is_set(start) || !is_set(end)) {
            return 0;
        }
        const std::uint64_t span = static_cast<std::uint64_t>(end) - static_cast<std::uint64_t>(start);
        const auto limit = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
        return span > limit ? (std::numeric_limits<std::int64_t>::max)() : static_cast<std::int64_t>(span);
    }

    [[nodiscard]] friend bool operator==(const WindowSpec&, const WindowSpec&) noexcept = default;
};

}  // namespace mc

#endif  // MC_SCOPE_HPP

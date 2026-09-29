// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// The facility model: what the coordinator is told about the plant, and the
// generations that fence decisions taken against an older view of it.
//
// Nothing here is discovered by this repository.  The model is supplied by
// adjacent authorities (DCCP inventory, ASI, DFI, the plant controls) and every
// fact in it is either explicitly measured or explicitly unmeasured.  A missing
// measurement is never converted to zero, to headroom, or to "safe": the
// Headroom type carries an explicit measured flag precisely so that an
// unmeasured power or cooling budget fails evaluation instead of passing it.

#ifndef MC_FACILITY_HPP
#define MC_FACILITY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "mc/digest.hpp"
#include "mc/ident.hpp"
#include "mc/scope.hpp"
#include "mc/status.hpp"
#include "mc/time.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

enum class LifecycleState : std::uint8_t {
    Unknown = 0,  // discovery without evidence of state
    Commissioning = 1,
    InService = 2,
    Maintenance = 3,
    Draining = 4,
    Drained = 5,
    Decommissioning = 6,
    Decommissioned = 7,
    Failed = 8,
};

[[nodiscard]] std::string_view to_string(LifecycleState state) noexcept;
[[nodiscard]] bool parse_lifecycle_state(std::string_view text, LifecycleState& out) noexcept;

// May the asset be the target of physical work at all?
[[nodiscard]] bool is_lifecycle_serviceable(LifecycleState state) noexcept;

enum class Severity : std::uint8_t { Low = 0, Moderate = 1, High = 2, Critical = 3 };
[[nodiscard]] std::string_view to_string(Severity severity) noexcept;
[[nodiscard]] bool parse_severity(std::string_view text, Severity& out) noexcept;

enum class ServiceClass : std::uint8_t {
    BestEffort = 0,
    Standard = 1,
    BusinessCritical = 2,
    MissionCritical = 3,
};
[[nodiscard]] std::string_view to_string(ServiceClass value) noexcept;
[[nodiscard]] bool parse_service_class(std::string_view text, ServiceClass& out) noexcept;

enum class RedundancyMode : std::uint8_t { None = 0, N = 1, NPlusOne = 2, NPlusTwo = 3, TwoN = 4 };
[[nodiscard]] std::string_view to_string(RedundancyMode mode) noexcept;
[[nodiscard]] bool parse_redundancy_mode(std::string_view text, RedundancyMode& out) noexcept;

// ---------------------------------------------------------------------------
// Headroom: an explicit measurement, or an explicit absence of one.
// ---------------------------------------------------------------------------

struct Headroom {
    bool measured{false};
    std::int64_t available_units{0};
    std::int64_t required_units{0};

    [[nodiscard]] bool satisfies(std::int64_t margin_units) const noexcept {
        return measured && (available_units - required_units) >= margin_units;
    }
    [[nodiscard]] std::int64_t margin_units() const noexcept { return available_units - required_units; }

    [[nodiscard]] friend bool operator==(const Headroom&, const Headroom&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Plant records
// ---------------------------------------------------------------------------

struct AssetRecord {
    AssetId id;
    RackId rack;
    SiteId site;
    LifecycleState lifecycle{LifecycleState::Unknown};
    ServiceClass service_class{ServiceClass::Standard};
    LifecycleGeneration lifecycle_generation;
    HardwareGeneration hardware_generation;
    FirmwareGeneration firmware_generation;
    // Capacity this asset contributes to the redundancy groups and capacity
    // pools it belongs to.  Explicitly stated by the inventory owner.
    std::uint32_t capacity_units{0};
    // Whether the plant controls have declared this asset isolatable.  This is
    // a capability claim, not a discovery result.
    bool isolatable{false};
    bool drainable{false};
    bool healthy{false};

    [[nodiscard]] friend bool operator==(const AssetRecord&, const AssetRecord&) noexcept = default;
};

struct RedundancyGroup {
    RedundancyGroupId id;
    SiteId site;
    RedundancyMode mode{RedundancyMode::None};
    std::uint32_t required_units{0};           // units that must survive
    std::uint32_t observed_available_units{0}; // units currently serving
    std::vector<AssetId> members;              // canonical order

    [[nodiscard]] friend bool operator==(const RedundancyGroup&, const RedundancyGroup&) noexcept = default;
};

struct CapacityPool {
    CapacityPoolId id;
    SiteId site;
    std::uint32_t units_total{0};
    std::uint32_t units_available{0};
    std::uint32_t units_protected{0};  // reserved for protected obligations

    [[nodiscard]] friend bool operator==(const CapacityPool&, const CapacityPool&) noexcept = default;
};

struct PowerDomain {
    PowerDomainId id;
    SiteId site;
    Headroom headroom;      // milliwatts
    bool interlocked{false};  // hard safety interlock: work must not proceed
    std::string interlock_reason;

    [[nodiscard]] friend bool operator==(const PowerDomain&, const PowerDomain&) noexcept = default;
};

struct CoolingZone {
    CoolingZoneId id;
    SiteId site;
    Headroom headroom;  // thermal units
    bool interlocked{false};
    std::string interlock_reason;

    [[nodiscard]] friend bool operator==(const CoolingZone&, const CoolingZone&) noexcept = default;
};

struct FabricSegment {
    FabricSegmentId id;
    SiteId site;
    std::uint32_t available_paths{0};
    std::uint32_t required_paths{0};
    bool drainable{false};

    [[nodiscard]] friend bool operator==(const FabricSegment&, const FabricSegment&) noexcept = default;
};

struct BlackoutPeriod {
    BlackoutId id;
    std::vector<TargetRef> targets;  // canonical order; empty means facility-wide
    Timestamp start{kNoTimestamp};
    Timestamp end{kNoTimestamp};
    bool hard{true};  // a hard blackout is a safety interlock, not a preference
    std::string reason;

    [[nodiscard]] bool overlaps(Timestamp from, Timestamp to) const noexcept {
        return is_set(start) && is_set(end) && from <= end && to >= start;
    }

    [[nodiscard]] friend bool operator==(const BlackoutPeriod&, const BlackoutPeriod&) noexcept = default;
};

struct IncidentRecord {
    IncidentId id;
    std::vector<TargetRef> targets;  // canonical order
    Severity severity{Severity::Low};
    bool active{false};
    bool hard_block{false};  // active incident is a hard interlock for this scope
    std::string summary;

    [[nodiscard]] friend bool operator==(const IncidentRecord&, const IncidentRecord&) noexcept = default;
};

struct ProtectedObligation {
    ProtectionId id;
    std::vector<TargetRef> targets;  // canonical order
    RedundancyMode required_mode{RedundancyMode::None};
    std::uint32_t required_units{0};
    std::uint32_t tolerance_units{0};
    bool hard_interlock{true};  // never waivable while hard
    std::string description;

    [[nodiscard]] friend bool operator==(const ProtectedObligation&, const ProtectedObligation&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

class PolicyDocument {
public:
    PolicyId id;
    Revision revision;
    PolicyGeneration generation;
    // Margins are floors applied on top of the plant's own required units.
    std::uint32_t min_redundancy_margin_units{0};
    std::uint32_t min_spare_capacity_units{0};
    std::int64_t min_power_headroom_milliwatts{0};
    std::int64_t min_cooling_headroom_units{0};
    std::int64_t approval_validity_nanos{8LL * kNanosPerHour};
    std::int64_t max_window_duration_nanos{7LL * kNanosPerDay};
    std::uint32_t max_concurrent_windows_per_rack{1};
    bool require_personnel_evidence{true};
    bool require_dual_approval{false};
    bool require_work_stop_evidence{true};
    bool require_restoration_evidence{true};
    std::string service_class_ceiling_note;  // free text, never parsed for authority

    // Structural validation plus digest computation.
    [[nodiscard]] Status validate() const;
    [[nodiscard]] Digest compute_digest() const;
    void finalize();

    [[nodiscard]] friend bool operator==(const PolicyDocument&, const PolicyDocument&) noexcept = default;

private:
    Digest digest_;
};

// ---------------------------------------------------------------------------
// The facility snapshot
// ---------------------------------------------------------------------------

class FacilitySnapshot {
public:
    // Every generation and epoch is explicit and must be set; an unset
    // generation is MissingGeneration, never "generation 0".
    Revision revision;
    FacilityEpoch facility_epoch;
    PolicyGeneration policy_generation;
    DependencyGeneration dependency_generation;
    CapacityGeneration capacity_generation;
    TopologyGeneration topology_generation;
    MaintenanceGeneration maintenance_generation;
    ControlEpoch control_epoch;
    Timestamp observed_at{kNoTimestamp};

    PolicyDocument policy;
    std::vector<AssetRecord> assets;
    std::vector<RedundancyGroup> redundancy_groups;
    std::vector<CapacityPool> capacity_pools;
    std::vector<PowerDomain> power_domains;
    std::vector<CoolingZone> cooling_zones;
    std::vector<FabricSegment> fabric_segments;
    std::vector<BlackoutPeriod> blackouts;
    std::vector<IncidentRecord> incidents;
    std::vector<ProtectedObligation> protected_obligations;

    Digest dependency_digest;  // over the dependency-relevant projection
    Digest digest;             // over the whole snapshot

    // Sorts every collection into canonical order and computes both digests.
    void finalize();
    // Structural validation: identities, references, generations, timestamps,
    // impossible combinations.  Returns the most primary fault found.
    [[nodiscard]] Status validate() const;
    [[nodiscard]] Status validate_generations() const;

    [[nodiscard]] const AssetRecord* find_asset(const AssetId& id) const noexcept;
    [[nodiscard]] const RedundancyGroup* find_redundancy_group(const RedundancyGroupId& id) const noexcept;
    [[nodiscard]] const CapacityPool* find_capacity_pool(const CapacityPoolId& id) const noexcept;
    [[nodiscard]] const PowerDomain* find_power_domain(const PowerDomainId& id) const noexcept;
    [[nodiscard]] const CoolingZone* find_cooling_zone(const CoolingZoneId& id) const noexcept;
    [[nodiscard]] const FabricSegment* find_fabric_segment(const FabricSegmentId& id) const noexcept;

    // True when the named target exists in the model.  Facility-wide pseudo
    // targets (kind Site with the site's own name) resolve through sites().
    [[nodiscard]] bool knows_target(const TargetRef& target) const noexcept;

    [[nodiscard]] std::vector<SiteId> sites() const;
    // Every site the scope touches, whether named directly, reached through a
    // rack, or reached through an asset.  Site-level plant objects (power,
    // cooling, capacity, fabric) resolve through this set.
    [[nodiscard]] std::vector<SiteId> sites_in_scope(const MaintenanceScope& scope) const;
    // Assets that are members of the scope, directly or through the rack or
    // site that contains them.  Deterministic order.
    [[nodiscard]] std::vector<AssetId> assets_in_scope(const MaintenanceScope& scope) const;
    // Redundancy groups, capacity pools, power domains, cooling zones and
    // fabric segments touched by the scope.  Deterministic order.
    [[nodiscard]] std::vector<RedundancyGroupId> redundancy_groups_in_scope(const MaintenanceScope& scope) const;
    [[nodiscard]] std::vector<CapacityPoolId> capacity_pools_in_scope(const MaintenanceScope& scope) const;
    [[nodiscard]] std::vector<PowerDomainId> power_domains_in_scope(const MaintenanceScope& scope) const;
    [[nodiscard]] std::vector<CoolingZoneId> cooling_zones_in_scope(const MaintenanceScope& scope) const;
    [[nodiscard]] std::vector<FabricSegmentId> fabric_segments_in_scope(const MaintenanceScope& scope) const;

    // Conflict relation used for concurrent-window policy: two scopes conflict
    // when they share a target or when one contains the other.
    [[nodiscard]] bool scope_conflicts(const MaintenanceScope& a, const MaintenanceScope& b) const;

    // Does any blackout / incident / protected obligation touch the scope?
    [[nodiscard]] std::vector<BlackoutId> blackouts_covering(const MaintenanceScope& scope, const WindowSpec& window) const;
    [[nodiscard]] std::vector<IncidentId> incidents_covering(const MaintenanceScope& scope, bool active_only) const;
    [[nodiscard]] std::vector<ProtectionId> protections_covering(const MaintenanceScope& scope) const;

private:
    [[nodiscard]] bool target_covers(const TargetRef& outer, const TargetRef& inner) const;
};

}  // namespace mc

#endif  // MC_FACILITY_HPP

// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.

#include "mc/facility.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "enum_table.hpp"
#include "serialize.hpp"

namespace mc {
namespace {

constexpr std::array<std::pair<std::string_view, unsigned>, 9> kLifecycleStates{{
    {"unknown", 0}, {"commissioning", 1}, {"in-service", 2}, {"maintenance", 3}, {"draining", 4},
    {"drained", 5}, {"decommissioning", 6}, {"decommissioned", 7}, {"failed", 8},
}};

constexpr std::array<std::pair<std::string_view, unsigned>, 4> kSeverities{{
    {"low", 0}, {"moderate", 1}, {"high", 2}, {"critical", 3},
}};

constexpr std::array<std::pair<std::string_view, unsigned>, 4> kServiceClasses{{
    {"best-effort", 0}, {"standard", 1}, {"business-critical", 2}, {"mission-critical", 3},
}};

constexpr std::array<std::pair<std::string_view, unsigned>, 5> kRedundancyModes{{
    {"none", 0}, {"n", 1}, {"n+1", 2}, {"n+2", 3}, {"2n", 4},
}};

template <typename T, typename KeyFn>
[[nodiscard]] bool is_canonical(const std::vector<T>& items, KeyFn key) {
    for (std::size_t index = 1; index < items.size(); ++index) {
        if (!(key(items[index - 1]) < key(items[index]))) {
            return false;
        }
    }
    return true;
}

// A target reference is only ever built from an identity the model already
// validated; a name that cannot be parsed yields an empty reference, which
// matches no scope, rather than an exception.
[[nodiscard]] TargetRef make_target(TargetKind kind, const std::string& name) {
    auto made = TargetRef::make(kind, name);
    return made.ok() ? made.value() : TargetRef{};
}

}  // namespace

std::string_view to_string(LifecycleState state) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(state), kLifecycleStates);
}

bool parse_lifecycle_state(std::string_view text, LifecycleState& out) noexcept {
    unsigned raw = 0;
    if (!detail::parse_enum_name(text, kLifecycleStates, raw)) {
        return false;
    }
    out = static_cast<LifecycleState>(raw);
    return true;
}

bool is_lifecycle_serviceable(LifecycleState state) noexcept {
    switch (state) {
        case LifecycleState::Commissioning:
        case LifecycleState::InService:
        case LifecycleState::Maintenance:
        case LifecycleState::Draining:
        case LifecycleState::Drained:
        case LifecycleState::Decommissioning:
            return true;
        case LifecycleState::Unknown:
        case LifecycleState::Decommissioned:
        case LifecycleState::Failed:
        default:
            return false;
    }
}

std::string_view to_string(Severity severity) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(severity), kSeverities);
}

bool parse_severity(std::string_view text, Severity& out) noexcept {
    unsigned raw = 0;
    if (!detail::parse_enum_name(text, kSeverities, raw)) {
        return false;
    }
    out = static_cast<Severity>(raw);
    return true;
}

std::string_view to_string(ServiceClass value) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(value), kServiceClasses);
}

bool parse_service_class(std::string_view text, ServiceClass& out) noexcept {
    unsigned raw = 0;
    if (!detail::parse_enum_name(text, kServiceClasses, raw)) {
        return false;
    }
    out = static_cast<ServiceClass>(raw);
    return true;
}

std::string_view to_string(RedundancyMode mode) noexcept {
    return detail::enum_name_of(static_cast<unsigned>(mode), kRedundancyModes);
}

bool parse_redundancy_mode(std::string_view text, RedundancyMode& out) noexcept {
    unsigned raw = 0;
    if (!detail::parse_enum_name(text, kRedundancyModes, raw)) {
        return false;
    }
    out = static_cast<RedundancyMode>(raw);
    return true;
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

Status PolicyDocument::validate() const {
    Status primary;
    if (id.empty()) {
        primary.merge(fail(ErrorCode::InvalidArgument, "policy document has no identity", "policy"));
    }
    if (!revision.is_set()) {
        primary.merge(fail(ErrorCode::MissingGeneration, "policy document has no revision", "policy", id.name()));
    }
    if (!generation.is_set()) {
        primary.merge(
            fail(ErrorCode::MissingGeneration, "policy document has no policy generation", "policy", id.name()));
    }
    if (approval_validity_nanos <= 0) {
        primary.merge(fail(ErrorCode::InvalidArgument, "policy approval validity must be positive", "policy",
                           id.name()));
    }
    if (max_window_duration_nanos <= 0) {
        primary.merge(
            fail(ErrorCode::InvalidArgument, "policy maximum window duration must be positive", "policy", id.name()));
    }
    if (max_concurrent_windows_per_rack == 0U) {
        primary.merge(fail(ErrorCode::InvalidArgument,
                           "policy must allow at least one concurrent window per rack", "policy", id.name()));
    }
    return primary;
}

Digest PolicyDocument::compute_digest() const {
    return detail::digest_of(*this);
}

void PolicyDocument::finalize() {
    digest_ = compute_digest();
}

// ---------------------------------------------------------------------------
// Facility snapshot
// ---------------------------------------------------------------------------

void FacilitySnapshot::finalize() {
    std::sort(assets.begin(), assets.end(),
              [](const AssetRecord& lhs, const AssetRecord& rhs) { return lhs.id < rhs.id; });
    for (auto& group : redundancy_groups) {
        std::sort(group.members.begin(), group.members.end());
        group.members.erase(std::unique(group.members.begin(), group.members.end()), group.members.end());
    }
    std::sort(redundancy_groups.begin(), redundancy_groups.end(),
              [](const RedundancyGroup& lhs, const RedundancyGroup& rhs) { return lhs.id < rhs.id; });
    std::sort(capacity_pools.begin(), capacity_pools.end(),
              [](const CapacityPool& lhs, const CapacityPool& rhs) { return lhs.id < rhs.id; });
    std::sort(power_domains.begin(), power_domains.end(),
              [](const PowerDomain& lhs, const PowerDomain& rhs) { return lhs.id < rhs.id; });
    std::sort(cooling_zones.begin(), cooling_zones.end(),
              [](const CoolingZone& lhs, const CoolingZone& rhs) { return lhs.id < rhs.id; });
    std::sort(fabric_segments.begin(), fabric_segments.end(),
              [](const FabricSegment& lhs, const FabricSegment& rhs) { return lhs.id < rhs.id; });
    for (auto& blackout : blackouts) {
        std::sort(blackout.targets.begin(), blackout.targets.end());
        blackout.targets.erase(std::unique(blackout.targets.begin(), blackout.targets.end()), blackout.targets.end());
    }
    std::sort(blackouts.begin(), blackouts.end(),
              [](const BlackoutPeriod& lhs, const BlackoutPeriod& rhs) { return lhs.id < rhs.id; });
    for (auto& incident : incidents) {
        std::sort(incident.targets.begin(), incident.targets.end());
        incident.targets.erase(std::unique(incident.targets.begin(), incident.targets.end()), incident.targets.end());
    }
    std::sort(incidents.begin(), incidents.end(),
              [](const IncidentRecord& lhs, const IncidentRecord& rhs) { return lhs.id < rhs.id; });
    for (auto& protection : protected_obligations) {
        std::sort(protection.targets.begin(), protection.targets.end());
        protection.targets.erase(std::unique(protection.targets.begin(), protection.targets.end()),
                                 protection.targets.end());
    }
    std::sort(protected_obligations.begin(), protected_obligations.end(),
              [](const ProtectedObligation& lhs, const ProtectedObligation& rhs) { return lhs.id < rhs.id; });
    dependency_digest = detail::dependency_digest_of(*this);
    digest = detail::digest_of(*this);
}

Status FacilitySnapshot::validate_generations() const {
    Status primary;
    const auto require = [&primary](bool set, std::string_view what) {
        if (!set) {
            primary.merge(fail(ErrorCode::MissingGeneration, "facility snapshot is missing a generation",
                               std::string(what)));
        }
    };
    require(revision.is_set(), "revision");
    require(facility_epoch.is_set(), "facility-epoch");
    require(policy_generation.is_set(), "policy-generation");
    require(dependency_generation.is_set(), "dependency-generation");
    require(capacity_generation.is_set(), "capacity-generation");
    require(topology_generation.is_set(), "topology-generation");
    require(maintenance_generation.is_set(), "maintenance-generation");
    require(control_epoch.is_set(), "control-epoch");
    return primary;
}

Status FacilitySnapshot::validate() const {
    Status primary = validate_generations();
    primary.merge(policy.validate());
    if (!is_set(observed_at)) {
        primary.merge(fail(ErrorCode::InvalidArgument,
                           "facility snapshot is not bound to an observation time", "facility"));
    }
    if (dependency_digest.is_zero() || digest.is_zero()) {
        primary.merge(fail(ErrorCode::InvalidArgument,
                           "facility snapshot digests have not been computed", "facility"));
    }

    // Duplicate identities are the most primary structural fault: later checks
    // would otherwise depend on which duplicate happened to be found first.
    for (std::size_t index = 1; index < assets.size(); ++index) {
        if (!(assets[index - 1].id < assets[index].id)) {
            primary.merge(fail(ErrorCode::DuplicateIdentity, "two assets share one identity", "asset",
                               assets[index].id.name()));
            break;
        }
    }
    for (std::size_t index = 1; index < redundancy_groups.size(); ++index) {
        if (!(redundancy_groups[index - 1].id < redundancy_groups[index].id)) {
            primary.merge(fail(ErrorCode::DuplicateIdentity, "two redundancy groups share one identity",
                               "redundancy-group", redundancy_groups[index].id.name()));
            break;
        }
    }
    for (std::size_t index = 1; index < capacity_pools.size(); ++index) {
        if (!(capacity_pools[index - 1].id < capacity_pools[index].id)) {
            primary.merge(fail(ErrorCode::DuplicateIdentity, "two capacity pools share one identity",
                               "capacity-pool", capacity_pools[index].id.name()));
            break;
        }
    }
    for (std::size_t index = 1; index < power_domains.size(); ++index) {
        if (!(power_domains[index - 1].id < power_domains[index].id)) {
            primary.merge(fail(ErrorCode::DuplicateIdentity, "two power domains share one identity",
                               "power-domain", power_domains[index].id.name()));
            break;
        }
    }
    for (std::size_t index = 1; index < cooling_zones.size(); ++index) {
        if (!(cooling_zones[index - 1].id < cooling_zones[index].id)) {
            primary.merge(fail(ErrorCode::DuplicateIdentity, "two cooling zones share one identity",
                               "cooling-zone", cooling_zones[index].id.name()));
            break;
        }
    }
    for (std::size_t index = 1; index < fabric_segments.size(); ++index) {
        if (!(fabric_segments[index - 1].id < fabric_segments[index].id)) {
            primary.merge(fail(ErrorCode::DuplicateIdentity, "two fabric segments share one identity",
                               "fabric-segment", fabric_segments[index].id.name()));
            break;
        }
    }
    for (std::size_t index = 1; index < blackouts.size(); ++index) {
        if (!(blackouts[index - 1].id < blackouts[index].id)) {
            primary.merge(fail(ErrorCode::DuplicateIdentity, "two blackout periods share one identity", "blackout",
                               blackouts[index].id.name()));
            break;
        }
    }
    for (std::size_t index = 1; index < incidents.size(); ++index) {
        if (!(incidents[index - 1].id < incidents[index].id)) {
            primary.merge(fail(ErrorCode::DuplicateIdentity, "two incidents share one identity", "incident",
                               incidents[index].id.name()));
            break;
        }
    }
    for (std::size_t index = 1; index < protected_obligations.size(); ++index) {
        if (!(protected_obligations[index - 1].id < protected_obligations[index].id)) {
            primary.merge(fail(ErrorCode::DuplicateIdentity, "two protected obligations share one identity",
                               "protected-obligation", protected_obligations[index].id.name()));
            break;
        }
    }

    // A rack belongs to exactly one site.  A model that says otherwise cannot
    // be used to reason about scope containment at all.
    std::map<RackId, SiteId> rack_site;
    for (const auto& asset : assets) {
        if (asset.id.empty() || asset.rack.empty() || asset.site.empty()) {
            primary.merge(fail(ErrorCode::UnknownTarget, "asset record is missing an identity or a parent",
                               "asset", asset.id.name()));
            continue;
        }
        const auto existing = rack_site.find(asset.rack);
        if (existing == rack_site.end()) {
            rack_site.emplace(asset.rack, asset.site);
        } else if (existing->second != asset.site) {
            primary.merge(fail(ErrorCode::IdentityMismatch, "one rack is reported in two different sites", "rack",
                               asset.rack.name()));
        }
        if (!asset.lifecycle_generation.is_set() || !asset.hardware_generation.is_set() ||
            !asset.firmware_generation.is_set()) {
            primary.merge(fail(ErrorCode::MissingGeneration, "asset record is missing a generation", "asset",
                               asset.id.name()));
        }
    }

    std::set<AssetId> asset_ids;
    for (const auto& asset : assets) {
        asset_ids.insert(asset.id);
    }
    for (const auto& group : redundancy_groups) {
        // A group below its required units is a fact, not a malformed model:
        // the evaluation reports it as a blocking condition instead.
        for (const auto& member : group.members) {
            if (asset_ids.find(member) == asset_ids.end()) {
                primary.merge(fail(ErrorCode::UnknownAsset, "redundancy group names an unknown member asset",
                                   group.id.name(), member.name()));
            }
        }
        if (!is_canonical(group.members, [](const AssetId& id) { return id; })) {
            primary.merge(fail(ErrorCode::LayoutInvalid, "redundancy group members are not in canonical order",
                               group.id.name()));
        }
    }

    for (const auto& blackout : blackouts) {
        if (!is_set(blackout.start) || !is_set(blackout.end) || blackout.start >= blackout.end) {
            primary.merge(fail(ErrorCode::InvalidArgument, "blackout period has an impossible time range",
                               "blackout", blackout.id.name()));
        }
        for (const auto& target : blackout.targets) {
            if (!knows_target(target)) {
                primary.merge(fail(ErrorCode::UnknownTarget, "blackout period names an unknown target", "blackout",
                                   target.str()));
            }
        }
    }
    for (const auto& incident : incidents) {
        if (incident.targets.empty()) {
            primary.merge(
                fail(ErrorCode::InvalidArgument, "incident record names no target", "incident", incident.id.name()));
        }
        for (const auto& target : incident.targets) {
            if (!knows_target(target)) {
                primary.merge(fail(ErrorCode::UnknownTarget, "incident names an unknown target", "incident",
                                   target.str()));
            }
        }
    }
    for (const auto& protection : protected_obligations) {
        if (protection.required_units == 0U) {
            primary.merge(fail(ErrorCode::InvalidArgument,
                               "protected obligation requires zero units, which protects nothing",
                               "protected-obligation", protection.id.name()));
        }
        if (protection.targets.empty()) {
            primary.merge(fail(ErrorCode::InvalidArgument, "protected obligation names no target",
                               "protected-obligation", protection.id.name()));
        }
        for (const auto& target : protection.targets) {
            if (!knows_target(target)) {
                primary.merge(fail(ErrorCode::UnknownTarget, "protected obligation names an unknown target",
                                   "protected-obligation", target.str()));
            }
        }
    }
    for (const auto& domain : power_domains) {
        if (domain.interlocked && domain.interlock_reason.empty()) {
            primary.merge(fail(ErrorCode::InvalidArgument, "power interlock has no stated reason", "power-domain",
                               domain.id.name()));
        }
    }
    for (const auto& zone : cooling_zones) {
        if (zone.interlocked && zone.interlock_reason.empty()) {
            primary.merge(fail(ErrorCode::InvalidArgument, "cooling interlock has no stated reason", "cooling-zone",
                               zone.id.name()));
        }
    }
    return primary;
}

const AssetRecord* FacilitySnapshot::find_asset(const AssetId& id) const noexcept {
    const auto found = std::lower_bound(assets.begin(), assets.end(), id,
                                        [](const AssetRecord& record, const AssetId& key) { return record.id < key; });
    if (found == assets.end() || found->id != id) {
        return nullptr;
    }
    return &*found;
}

const RedundancyGroup* FacilitySnapshot::find_redundancy_group(const RedundancyGroupId& id) const noexcept {
    const auto found = std::lower_bound(
        redundancy_groups.begin(), redundancy_groups.end(), id,
        [](const RedundancyGroup& record, const RedundancyGroupId& key) { return record.id < key; });
    if (found == redundancy_groups.end() || found->id != id) {
        return nullptr;
    }
    return &*found;
}

const CapacityPool* FacilitySnapshot::find_capacity_pool(const CapacityPoolId& id) const noexcept {
    const auto found = std::lower_bound(
        capacity_pools.begin(), capacity_pools.end(), id,
        [](const CapacityPool& record, const CapacityPoolId& key) { return record.id < key; });
    if (found == capacity_pools.end() || found->id != id) {
        return nullptr;
    }
    return &*found;
}

const PowerDomain* FacilitySnapshot::find_power_domain(const PowerDomainId& id) const noexcept {
    const auto found = std::lower_bound(
        power_domains.begin(), power_domains.end(), id,
        [](const PowerDomain& record, const PowerDomainId& key) { return record.id < key; });
    if (found == power_domains.end() || found->id != id) {
        return nullptr;
    }
    return &*found;
}

const CoolingZone* FacilitySnapshot::find_cooling_zone(const CoolingZoneId& id) const noexcept {
    const auto found = std::lower_bound(
        cooling_zones.begin(), cooling_zones.end(), id,
        [](const CoolingZone& record, const CoolingZoneId& key) { return record.id < key; });
    if (found == cooling_zones.end() || found->id != id) {
        return nullptr;
    }
    return &*found;
}

const FabricSegment* FacilitySnapshot::find_fabric_segment(const FabricSegmentId& id) const noexcept {
    const auto found = std::lower_bound(
        fabric_segments.begin(), fabric_segments.end(), id,
        [](const FabricSegment& record, const FabricSegmentId& key) { return record.id < key; });
    if (found == fabric_segments.end() || found->id != id) {
        return nullptr;
    }
    return &*found;
}

bool FacilitySnapshot::knows_target(const TargetRef& target) const noexcept {
    switch (target.kind()) {
        case TargetKind::Asset: {
            const auto parsed = AssetId::parse(target.name());
            return parsed.ok() && find_asset(parsed.value()) != nullptr;
        }
        case TargetKind::Rack:
            return std::any_of(assets.begin(), assets.end(),
                               [&target](const AssetRecord& asset) { return asset.rack.name() == target.name(); });
        case TargetKind::Site: {
            const auto matches = [&target](const SiteId& site) { return site.name() == target.name(); };
            if (std::any_of(assets.begin(), assets.end(),
                            [&matches](const AssetRecord& asset) { return matches(asset.site); })) {
                return true;
            }
            if (std::any_of(redundancy_groups.begin(), redundancy_groups.end(),
                            [&matches](const RedundancyGroup& group) { return matches(group.site); })) {
                return true;
            }
            if (std::any_of(capacity_pools.begin(), capacity_pools.end(),
                            [&matches](const CapacityPool& pool) { return matches(pool.site); })) {
                return true;
            }
            if (std::any_of(power_domains.begin(), power_domains.end(),
                            [&matches](const PowerDomain& domain) { return matches(domain.site); })) {
                return true;
            }
            if (std::any_of(cooling_zones.begin(), cooling_zones.end(),
                            [&matches](const CoolingZone& zone) { return matches(zone.site); })) {
                return true;
            }
            return std::any_of(fabric_segments.begin(), fabric_segments.end(),
                               [&matches](const FabricSegment& segment) { return matches(segment.site); });
        }
        case TargetKind::PowerDomain: {
            const auto parsed = PowerDomainId::parse(target.name());
            return parsed.ok() && find_power_domain(parsed.value()) != nullptr;
        }
        case TargetKind::CoolingZone: {
            const auto parsed = CoolingZoneId::parse(target.name());
            return parsed.ok() && find_cooling_zone(parsed.value()) != nullptr;
        }
        case TargetKind::FabricSegment: {
            const auto parsed = FabricSegmentId::parse(target.name());
            return parsed.ok() && find_fabric_segment(parsed.value()) != nullptr;
        }
        default:
            return false;
    }
}

std::vector<SiteId> FacilitySnapshot::sites() const {
    std::vector<SiteId> result;
    const auto add = [&result](const SiteId& site) {
        if (!site.empty()) {
            result.push_back(site);
        }
    };
    for (const auto& asset : assets) {
        add(asset.site);
    }
    for (const auto& group : redundancy_groups) {
        add(group.site);
    }
    for (const auto& pool : capacity_pools) {
        add(pool.site);
    }
    for (const auto& domain : power_domains) {
        add(domain.site);
    }
    for (const auto& zone : cooling_zones) {
        add(zone.site);
    }
    for (const auto& segment : fabric_segments) {
        add(segment.site);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<SiteId> FacilitySnapshot::sites_in_scope(const MaintenanceScope& scope) const {
    std::vector<SiteId> result;
    const auto add = [&result](const SiteId& site) {
        if (!site.empty()) {
            result.push_back(site);
        }
    };
    for (const auto& asset : assets) {
        const bool in_scope = scope.contains(make_target(TargetKind::Asset, asset.id.name())) ||
                              scope.contains(make_target(TargetKind::Rack, asset.rack.name())) ||
                              scope.contains(make_target(TargetKind::Site, asset.site.name()));
        if (in_scope) {
            add(asset.site);
        }
    }
    for (const auto& pool : capacity_pools) {
        if (scope.contains(make_target(TargetKind::Site, pool.site.name()))) {
            add(pool.site);
        }
    }
    for (const auto& domain : power_domains) {
        if (scope.contains(make_target(TargetKind::Site, domain.site.name())) ||
            scope.contains(make_target(TargetKind::PowerDomain, domain.id.name()))) {
            add(domain.site);
        }
    }
    for (const auto& zone : cooling_zones) {
        if (scope.contains(make_target(TargetKind::Site, zone.site.name())) ||
            scope.contains(make_target(TargetKind::CoolingZone, zone.id.name()))) {
            add(zone.site);
        }
    }
    for (const auto& segment : fabric_segments) {
        if (scope.contains(make_target(TargetKind::Site, segment.site.name())) ||
            scope.contains(make_target(TargetKind::FabricSegment, segment.id.name()))) {
            add(segment.site);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<AssetId> FacilitySnapshot::assets_in_scope(const MaintenanceScope& scope) const {
    std::vector<AssetId> result;
    for (const auto& asset : assets) {
        const bool direct = scope.contains(make_target(TargetKind::Asset, asset.id.name()));
        const bool via_rack = scope.contains(make_target(TargetKind::Rack, asset.rack.name()));
        const bool via_site = scope.contains(make_target(TargetKind::Site, asset.site.name()));
        if (direct || via_rack || via_site) {
            result.push_back(asset.id);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<RedundancyGroupId> FacilitySnapshot::redundancy_groups_in_scope(const MaintenanceScope& scope) const {
    const std::vector<AssetId> scoped_assets = assets_in_scope(scope);
    std::vector<RedundancyGroupId> result;
    for (const auto& group : redundancy_groups) {
        const bool direct = scope.contains(make_target(TargetKind::Site, group.site.name()));
        bool shares_member = false;
        for (const auto& member : group.members) {
            if (std::binary_search(scoped_assets.begin(), scoped_assets.end(), member)) {
                shares_member = true;
                break;
            }
        }
        if (direct || shares_member) {
            result.push_back(group.id);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<CapacityPoolId> FacilitySnapshot::capacity_pools_in_scope(const MaintenanceScope& scope) const {
    const std::vector<SiteId> sites = sites_in_scope(scope);
    std::vector<CapacityPoolId> result;
    for (const auto& pool : capacity_pools) {
        // Capacity pools are reached through the site that owns them; a pool is
        // not itself a maintainable physical target.
        if (std::binary_search(sites.begin(), sites.end(), pool.site)) {
            result.push_back(pool.id);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<PowerDomainId> FacilitySnapshot::power_domains_in_scope(const MaintenanceScope& scope) const {
    const std::vector<SiteId> sites = sites_in_scope(scope);
    std::vector<PowerDomainId> result;
    for (const auto& domain : power_domains) {
        if (std::binary_search(sites.begin(), sites.end(), domain.site) ||
            scope.contains(make_target(TargetKind::PowerDomain, domain.id.name()))) {
            result.push_back(domain.id);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<CoolingZoneId> FacilitySnapshot::cooling_zones_in_scope(const MaintenanceScope& scope) const {
    const std::vector<SiteId> sites = sites_in_scope(scope);
    std::vector<CoolingZoneId> result;
    for (const auto& zone : cooling_zones) {
        if (std::binary_search(sites.begin(), sites.end(), zone.site) ||
            scope.contains(make_target(TargetKind::CoolingZone, zone.id.name()))) {
            result.push_back(zone.id);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<FabricSegmentId> FacilitySnapshot::fabric_segments_in_scope(const MaintenanceScope& scope) const {
    const std::vector<SiteId> sites = sites_in_scope(scope);
    std::vector<FabricSegmentId> result;
    for (const auto& segment : fabric_segments) {
        if (std::binary_search(sites.begin(), sites.end(), segment.site) ||
            scope.contains(make_target(TargetKind::FabricSegment, segment.id.name()))) {
            result.push_back(segment.id);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool FacilitySnapshot::target_covers(const TargetRef& outer, const TargetRef& inner) const {
    if (outer == inner) {
        return true;
    }
    if (outer.kind() == TargetKind::Site && inner.kind() == TargetKind::Asset) {
        const auto parsed = AssetId::parse(inner.name());
        const AssetRecord* asset = parsed.ok() ? find_asset(parsed.value()) : nullptr;
        return asset != nullptr && asset->site.name() == outer.name();
    }
    if (outer.kind() == TargetKind::Site && inner.kind() == TargetKind::Rack) {
        return std::any_of(assets.begin(), assets.end(), [&outer, &inner](const AssetRecord& asset) {
            return asset.rack.name() == inner.name() && asset.site.name() == outer.name();
        });
    }
    if (outer.kind() == TargetKind::Rack && inner.kind() == TargetKind::Asset) {
        const auto parsed = AssetId::parse(inner.name());
        const AssetRecord* asset = parsed.ok() ? find_asset(parsed.value()) : nullptr;
        return asset != nullptr && asset->rack.name() == outer.name();
    }
    return false;
}

bool FacilitySnapshot::scope_conflicts(const MaintenanceScope& a, const MaintenanceScope& b) const {
    for (const auto& left : a.targets()) {
        for (const auto& right : b.targets()) {
            if (target_covers(left, right) || target_covers(right, left)) {
                return true;
            }
        }
    }
    return false;
}

std::vector<BlackoutId> FacilitySnapshot::blackouts_covering(const MaintenanceScope& scope,
                                                             const WindowSpec& window) const {
    std::vector<BlackoutId> result;
    if (!is_set(window.start) || !is_set(window.end)) {
        return result;
    }
    for (const auto& blackout : blackouts) {
        if (!blackout.overlaps(window.start, window.end)) {
            continue;
        }
        if (blackout.targets.empty()) {
            result.push_back(blackout.id);  // facility wide
            continue;
        }
        bool intersects = false;
        for (const auto& blackout_target : blackout.targets) {
            for (const auto& scope_target : scope.targets()) {
                if (target_covers(blackout_target, scope_target) || target_covers(scope_target, blackout_target)) {
                    intersects = true;
                    break;
                }
            }
            if (intersects) {
                break;
            }
        }
        if (intersects) {
            result.push_back(blackout.id);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<IncidentId> FacilitySnapshot::incidents_covering(const MaintenanceScope& scope,
                                                             bool active_only) const {
    std::vector<IncidentId> result;
    for (const auto& incident : incidents) {
        if (active_only && !incident.active) {
            continue;
        }
        bool intersects = false;
        for (const auto& incident_target : incident.targets) {
            for (const auto& scope_target : scope.targets()) {
                if (target_covers(incident_target, scope_target) || target_covers(scope_target, incident_target)) {
                    intersects = true;
                    break;
                }
            }
            if (intersects) {
                break;
            }
        }
        if (intersects) {
            result.push_back(incident.id);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<ProtectionId> FacilitySnapshot::protections_covering(const MaintenanceScope& scope) const {
    std::vector<ProtectionId> result;
    for (const auto& protection : protected_obligations) {
        bool intersects = false;
        for (const auto& protection_target : protection.targets) {
            for (const auto& scope_target : scope.targets()) {
                if (target_covers(protection_target, scope_target) ||
                    target_covers(scope_target, protection_target)) {
                    intersects = true;
                    break;
                }
            }
            if (intersects) {
                break;
            }
        }
        if (intersects) {
            result.push_back(protection.id);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

}  // namespace mc

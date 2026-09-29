// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Pure gate functions: precondition evaluation, obligation derivation,
// disposition derivation and fencing.  None of them reads a clock, a store or
// any ambient state, so a gate result is a function of its inputs only.

#include "engine_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "mc/digest.hpp"
#include "serialize.hpp"

namespace mc::detail {
namespace {

// A target reference is only ever built from an identity the model validated;
// a name that cannot be parsed yields an empty reference, which matches no
// scope, rather than an exception.
[[nodiscard]] TargetRef target_of(TargetKind kind, const std::string& name) {
    auto made = TargetRef::make(kind, name);
    return made.ok() ? made.value() : TargetRef{};
}

[[nodiscard]] bool group_matches_target(const FacilitySnapshot& facility, const RedundancyGroup& group,
                                        const TargetRef& target) {
    switch (target.kind()) {
        case TargetKind::Site:
            return group.site.name() == target.name();
        case TargetKind::Rack:
            return std::any_of(group.members.begin(), group.members.end(), [&](const AssetId& member) {
                const AssetRecord* asset = facility.find_asset(member);
                return asset != nullptr && asset->rack.name() == target.name();
            });
        case TargetKind::Asset:
            return std::any_of(group.members.begin(), group.members.end(),
                               [&](const AssetId& member) { return member.name() == target.name(); });
        default:
            return false;
    }
}

[[nodiscard]] std::uint32_t scoped_capacity(const FacilitySnapshot& facility, const RedundancyGroup& group,
                                            const std::vector<AssetId>& scoped) {
    std::uint32_t removed = 0;
    for (const auto& member : group.members) {
        if (!std::binary_search(scoped.begin(), scoped.end(), member)) {
            continue;
        }
        if (const AssetRecord* asset = facility.find_asset(member); asset != nullptr) {
            removed += asset->capacity_units;
        }
    }
    return removed;
}

[[nodiscard]] bool is_invasive(MaintenanceActivity activity) noexcept {
    return activity != MaintenanceActivity::Inspection;
}

[[nodiscard]] bool needs_power_isolation(MaintenanceActivity activity) noexcept {
    switch (activity) {
        case MaintenanceActivity::HardwareRepair:
        case MaintenanceActivity::HardwareUpgrade:
        case MaintenanceActivity::RackInstall:
        case MaintenanceActivity::PowerWork:
        case MaintenanceActivity::Decommission:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool needs_cooling_adjustment(MaintenanceActivity activity) noexcept {
    return activity == MaintenanceActivity::CoolingWork || activity == MaintenanceActivity::Decommission;
}

[[nodiscard]] bool needs_fabric_work(MaintenanceActivity activity) noexcept {
    switch (activity) {
        case MaintenanceActivity::NetworkWork:
        case MaintenanceActivity::FirmwareUpgrade:
        case MaintenanceActivity::HardwareUpgrade:
        case MaintenanceActivity::RackInstall:
        case MaintenanceActivity::Decommission:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool needs_lifecycle_transition(MaintenanceActivity activity) noexcept {
    switch (activity) {
        case MaintenanceActivity::RackInstall:
        case MaintenanceActivity::Decommission:
        case MaintenanceActivity::HardwareRepair:
        case MaintenanceActivity::HardwareUpgrade:
        case MaintenanceActivity::PowerWork:
        case MaintenanceActivity::CoolingWork:
            return true;
        default:
            return false;
    }
}

struct PendingCondition {
    ConditionResult result;
    TargetRef target;
};

[[nodiscard]] ConditionResult make_condition(ErrorCode code, bool satisfied, bool hard, bool waivable,
                                             const TargetRef& target, std::string detail,
                                             std::int64_t observed, std::int64_t required, bool measured) {
    ConditionResult condition;
    condition.condition = code;
    condition.satisfied = satisfied;
    condition.hard = hard;
    condition.waivable = waivable && !hard;
    condition.measured = measured;
    condition.subject = target.str();
    condition.detail = std::move(detail);
    condition.observed = observed;
    condition.required = required;
    return condition;
}

}  // namespace

// ---------------------------------------------------------------------------
// Redundancy and protection
// ---------------------------------------------------------------------------

RedundancyView redundancy_after_maintenance(const FacilitySnapshot& facility, const MaintenancePlan& plan) {
    RedundancyView view;
    const std::vector<AssetId> scoped = facility.assets_in_scope(plan.scope);
    for (const auto& group_id : facility.redundancy_groups_in_scope(plan.scope)) {
        const RedundancyGroup* group = facility.find_redundancy_group(group_id);
        if (group == nullptr) {
            continue;
        }
        RedundancyView::Group entry;
        entry.id = group->id;
        entry.observed_units = group->observed_available_units;
        entry.removed_units = scoped_capacity(facility, *group, scoped);
        entry.surviving_units = entry.observed_units > entry.removed_units ? entry.observed_units - entry.removed_units : 0U;
        entry.required_units = group->required_units;
        view.groups.push_back(entry);
    }
    std::sort(view.groups.begin(), view.groups.end(),
              [](const RedundancyView::Group& lhs, const RedundancyView::Group& rhs) { return lhs.id < rhs.id; });
    return view;
}

std::vector<RedundancyGroupId> redundancy_below_requirement(const FacilitySnapshot& facility,
                                                            const MaintenancePlan& plan) {
    std::vector<RedundancyGroupId> result;
    const std::uint32_t margin = facility.policy.min_redundancy_margin_units;
    for (const auto& group : redundancy_after_maintenance(facility, plan).groups) {
        if (group.surviving_units < group.required_units + margin) {
            result.push_back(group.id);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<ProtectionId> violated_protections(const FacilitySnapshot& facility, const MaintenancePlan& plan) {
    std::vector<ProtectionId> result;
    const std::vector<AssetId> scoped = facility.assets_in_scope(plan.scope);
    for (const auto& protection_id : facility.protections_covering(plan.scope)) {
        const auto found = std::find_if(facility.protected_obligations.begin(), facility.protected_obligations.end(),
                                        [&protection_id](const ProtectedObligation& protection) {
                                            return protection.id == protection_id;
                                        });
        if (found == facility.protected_obligations.end()) {
            continue;
        }
        const ProtectedObligation& protection = *found;
        // The protection is evaluated against the redundancy groups its targets
        // name.  A facility that has no such group cannot demonstrate the floor,
        // which is a violation rather than a silent pass.
        std::vector<const RedundancyGroup*> relevant;
        for (const auto& group : facility.redundancy_groups) {
            const bool matches = std::any_of(protection.targets.begin(), protection.targets.end(),
                                             [&](const TargetRef& target) {
                                                 return group_matches_target(facility, group, target);
                                             });
            if (matches) {
                relevant.push_back(&group);
            }
        }
        if (relevant.empty()) {
            result.push_back(protection.id);
            continue;
        }
        const std::uint32_t floor = protection.required_units > protection.tolerance_units
                                        ? protection.required_units - protection.tolerance_units
                                        : 0U;
        for (const RedundancyGroup* group : relevant) {
            const std::uint32_t removed = scoped_capacity(facility, *group, scoped);
            const std::uint32_t surviving =
                group->observed_available_units > removed ? group->observed_available_units - removed : 0U;
            if (surviving < floor) {
                result.push_back(protection.id);
                break;
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

// ---------------------------------------------------------------------------
// Preconditions
// ---------------------------------------------------------------------------

PreconditionReport evaluate_preconditions(const GateContext& context) {
    PreconditionReport report;
    if (context.facility == nullptr || context.plan == nullptr || context.record == nullptr) {
        report.finalize();
        return report;
    }
    const FacilitySnapshot& facility = *context.facility;
    const MaintenancePlan& plan = *context.plan;
    const PlanRecord& record = *context.record;
    const Timestamp now = context.now;

    report.plan_revision = plan.revision;
    report.plan_digest = plan.digest;
    report.evaluated_at = now;
    report.facility_epoch = facility.facility_epoch;
    report.control_epoch = facility.control_epoch;
    report.policy_generation = facility.policy_generation;
    report.dependency_generation = facility.dependency_generation;
    report.capacity_generation = facility.capacity_generation;
    report.topology_generation = facility.topology_generation;
    report.maintenance_generation = facility.maintenance_generation;
    report.facility_digest = facility.digest;
    report.policy_digest = facility.policy.compute_digest();
    report.dependency_digest = facility.dependency_digest;

    std::vector<PendingCondition> pending;
    const auto add = [&pending](ConditionResult condition, TargetRef target) {
        pending.push_back(PendingCondition{std::move(condition), std::move(target)});
    };

    // --- protected obligations (hard by default) -----------------------------
    for (const auto& protection_id : violated_protections(facility, plan)) {
        const auto found = std::find_if(facility.protected_obligations.begin(), facility.protected_obligations.end(),
                                        [&protection_id](const ProtectedObligation& protection) {
                                            return protection.id == protection_id;
                                        });
        const TargetRef target =
            found == facility.protected_obligations.end() || found->targets.empty()
                ? TargetRef{}
                : found->targets.front();
        const std::string description =
            found == facility.protected_obligations.end() ? std::string{} : found->description;
        add(make_condition(ErrorCode::ProtectedObligationViolated, false,
                           found == facility.protected_obligations.end() ? true : found->hard_interlock, true, target,
                           "maintenance removes redundancy that the protected obligation requires: " + description,
                           0, found == facility.protected_obligations.end()
                                  ? 0
                                  : static_cast<std::int64_t>(found->required_units),
                           true),
            target);
    }

    // --- active incidents -----------------------------------------------------
    for (const auto& incident_id : facility.incidents_covering(plan.scope, true)) {
        const auto found = std::find_if(facility.incidents.begin(), facility.incidents.end(),
                                        [&incident_id](const IncidentRecord& incident) {
                                            return incident.id == incident_id;
                                        });
        if (found == facility.incidents.end()) {
            continue;
        }
        const TargetRef target = found->targets.empty() ? TargetRef{} : found->targets.front();
        add(make_condition(ErrorCode::ActiveIncident, false, found->hard_block, true, target,
                           "an incident is active in this scope: " + found->summary, 0, 0, true),
            target);
    }

    // --- protected windows and blackout periods --------------------------------
    for (const auto& blackout_id : facility.blackouts_covering(plan.scope, plan.window)) {
        const auto found = std::find_if(facility.blackouts.begin(), facility.blackouts.end(),
                                        [&blackout_id](const BlackoutPeriod& blackout) {
                                            return blackout.id == blackout_id;
                                        });
        if (found == facility.blackouts.end()) {
            continue;
        }
        const bool facility_wide = found->targets.empty();
        const TargetRef target = facility_wide ? TargetRef{} : found->targets.front();
        // A facility-wide protected window and a scoped blackout are different
        // facts and are reported with different codes.
        const ErrorCode code = facility_wide ? ErrorCode::ProtectedWindowActive : ErrorCode::BlackoutPeriodActive;
        add(make_condition(code, false, found->hard, true, target,
                           "the requested window falls inside a blackout period: " + found->reason, 0, 0, true),
            target);
    }

    // --- lifecycle -------------------------------------------------------------
    for (const auto& asset_id : facility.assets_in_scope(plan.scope)) {
        const AssetRecord* asset = facility.find_asset(asset_id);
        if (asset == nullptr) {
            continue;
        }
        if (is_lifecycle_serviceable(asset->lifecycle)) {
            continue;
        }
        const TargetRef target = target_of(TargetKind::Asset, asset->id.name());
        add(make_condition(ErrorCode::LifecycleStateInvalid, false, true, false, target,
                           "asset is in lifecycle state '" + std::string(to_string(asset->lifecycle)) +
                               "', which cannot be the target of maintenance",
                           0, 0, true),
            target);
    }

    // --- redundancy ------------------------------------------------------------
    const RedundancyView redundancy = redundancy_after_maintenance(facility, plan);
    for (const auto& group : redundancy.groups) {
        const RedundancyGroup* definition = facility.find_redundancy_group(group.id);
        if (definition == nullptr) {
            continue;
        }
        const std::uint32_t floor = group.required_units + facility.policy.min_redundancy_margin_units;
        const TargetRef target = target_of(TargetKind::Site, definition->site.name());
        const bool satisfied = group.surviving_units >= floor;
        const std::string prefix = "redundancy group " + group.id.name() + ": ";
        add(make_condition(ErrorCode::RedundancyInsufficient, satisfied, false, true, target,
                           prefix + (satisfied ? "enough units survive the maintenance"
                                               : "too few units survive the maintenance"),
                           static_cast<std::int64_t>(group.surviving_units),
                           static_cast<std::int64_t>(floor), true),
            target);
    }

    // --- capacity --------------------------------------------------------------
    for (const auto& pool_id : facility.capacity_pools_in_scope(plan.scope)) {
        const CapacityPool* pool = facility.find_capacity_pool(pool_id);
        if (pool == nullptr) {
            continue;
        }
        const std::int64_t spare = static_cast<std::int64_t>(pool->units_available) -
                                   static_cast<std::int64_t>(pool->units_protected);
        const auto required = static_cast<std::int64_t>(
            std::max<std::int64_t>(0, facility.policy.min_spare_capacity_units));
        const TargetRef target = target_of(TargetKind::Site, pool->site.name());
        add(make_condition(ErrorCode::SpareCapacityInsufficient, spare >= required, false, true, target,
                           spare >= required ? "spare capacity is above the floor"
                                             : "spare capacity is below the policy floor",
                           spare, required, true),
            target);
    }

    // --- power and cooling ------------------------------------------------------
    for (const auto& domain_id : facility.power_domains_in_scope(plan.scope)) {
        const PowerDomain* domain = facility.find_power_domain(domain_id);
        if (domain == nullptr) {
            continue;
        }
        const TargetRef target = target_of(TargetKind::PowerDomain, domain->id.name());
        if (domain->interlocked) {
            add(make_condition(ErrorCode::HardInterlock, false, true, false, target,
                               "power domain is interlocked: " + domain->interlock_reason, 0, 0, true),
                target);
            continue;
        }
        const auto required = static_cast<std::int64_t>(facility.policy.min_power_headroom_milliwatts);
        const std::int64_t margin = domain->headroom.margin_units();
        const bool satisfied = domain->headroom.satisfies(required);
        const std::string detail = !domain->headroom.measured ? "power headroom was not measured"
                                   : satisfied               ? "power headroom is above the policy floor"
                                                             : "power headroom is below the policy floor";
        add(make_condition(ErrorCode::PowerHeadroomInsufficient, satisfied, false, true, target, detail,
                           domain->headroom.measured ? margin : 0, required, domain->headroom.measured),
            target);
    }
    for (const auto& zone_id : facility.cooling_zones_in_scope(plan.scope)) {
        const CoolingZone* zone = facility.find_cooling_zone(zone_id);
        if (zone == nullptr) {
            continue;
        }
        const TargetRef target = target_of(TargetKind::CoolingZone, zone->id.name());
        if (zone->interlocked) {
            add(make_condition(ErrorCode::HardInterlock, false, true, false, target,
                               "cooling zone is interlocked: " + zone->interlock_reason, 0, 0, true),
                target);
            continue;
        }
        const auto required = static_cast<std::int64_t>(facility.policy.min_cooling_headroom_units);
        const std::int64_t margin = zone->headroom.margin_units();
        const bool satisfied = zone->headroom.satisfies(required);
        const std::string detail = !zone->headroom.measured ? "cooling headroom was not measured"
                                   : satisfied               ? "cooling headroom is above the policy floor"
                                                             : "cooling headroom is below the policy floor";
        add(make_condition(ErrorCode::CoolingHeadroomInsufficient, satisfied, false, true, target, detail,
                           zone->headroom.measured ? margin : 0, required, zone->headroom.measured),
            target);
    }

    // --- service class ----------------------------------------------------------
    for (const auto& asset_id : facility.assets_in_scope(plan.scope)) {
        const AssetRecord* asset = facility.find_asset(asset_id);
        if (asset == nullptr || asset->service_class != ServiceClass::MissionCritical) {
            continue;
        }
        if (!is_invasive(plan.activity) || plan.risk >= ServiceRiskClass::High) {
            continue;
        }
        const TargetRef target = target_of(TargetKind::Asset, asset->id.name());
        add(make_condition(ErrorCode::ServiceClassViolation, false, false, true, target,
                           "invasive work on a mission-critical asset needs a high or critical risk "
                           "classification",
                           0, 0, true),
            target);
    }

    // --- concurrent maintenance --------------------------------------------------
    for (const PlanRecord* other : context.other_active_plans) {
        if (other == nullptr || other->plan.id == plan.id) {
            continue;
        }
        if (!is_started(other->phase) && other->phase != PlanPhase::Ready) {
            continue;
        }
        if (!facility.scope_conflicts(plan.scope, other->plan.scope)) {
            continue;
        }
        const TargetRef target = plan.scope.targets().empty() ? TargetRef{} : plan.scope.targets().front();
        add(make_condition(ErrorCode::ConcurrentMaintenanceConflict, false, true, false, target,
                           "plan '" + other->plan.id.name() + "' already holds this scope", 0,
                           static_cast<std::int64_t>(facility.policy.max_concurrent_windows_per_rack), true),
            target);
    }

    // --- waivers ---------------------------------------------------------------
    for (auto& entry : pending) {
        if (entry.result.satisfied) {
            continue;
        }
        for (const auto& grant : record.exceptions) {
            if (!entry.result.waivable) {
                break;
            }
            if (!entry.target.empty() && grant.covers(entry.result.condition, entry.target, now)) {
                entry.result.waiver = grant.id;
                report.applied_exceptions.push_back(grant.id);
                break;
            }
        }
    }

    bool satisfied = true;
    bool satisfied_without_exceptions = true;
    for (auto& entry : pending) {
        const bool waived = !entry.result.waiver.empty();
        if (!entry.result.satisfied && !waived) {
            satisfied = false;
        }
        if (!entry.result.satisfied) {
            satisfied_without_exceptions = false;
        }
        if (waived) {
            entry.result.detail += " (waived by exception " + entry.result.waiver.name() + ")";
        }
        report.conditions.push_back(std::move(entry.result));
    }
    report.satisfied = satisfied;
    report.satisfied_without_exceptions = satisfied_without_exceptions;
    report.finalize();
    return report;
}

// ---------------------------------------------------------------------------
// Obligations
// ---------------------------------------------------------------------------

ObligationSet derive_obligations(const GateContext& context) {
    ObligationSet set;
    if (context.facility == nullptr || context.plan == nullptr || context.record == nullptr) {
        set.finalize();
        return set;
    }
    const FacilitySnapshot& facility = *context.facility;
    const MaintenancePlan& plan = *context.plan;

    const std::vector<AssetId> scoped_assets = facility.assets_in_scope(plan.scope);
    const std::vector<RedundancyGroupId> groups = facility.redundancy_groups_in_scope(plan.scope);
    const std::vector<PowerDomainId> power_domains = facility.power_domains_in_scope(plan.scope);
    const std::vector<CoolingZoneId> cooling_zones = facility.cooling_zones_in_scope(plan.scope);
    const std::vector<FabricSegmentId> fabric_segments = facility.fabric_segments_in_scope(plan.scope);
    const TargetRef anchor = plan.scope.targets().empty() ? TargetRef{} : plan.scope.targets().front();

    const bool invasive = is_invasive(plan.activity);
    const bool power = needs_power_isolation(plan.activity);
    const bool cooling = needs_cooling_adjustment(plan.activity);
    const bool fabric = needs_fabric_work(plan.activity);
    const bool lifecycle = needs_lifecycle_transition(plan.activity);

    struct Spec {
        ObligationKind kind;
        TargetRef target;
        std::string requirement;
        ErrorCode guarded_by;
    };
    std::vector<Spec> specs;
    const auto push = [&specs](ObligationKind kind, const TargetRef& target, std::string requirement,
                               ErrorCode guarded_by) {
        specs.push_back(Spec{kind, target, std::move(requirement), guarded_by});
    };
    const auto asset_target = [](const AssetId& id) { return target_of(TargetKind::Asset, id.name()); };
    const auto rack_target = [&facility](const AssetId& id) {
        const AssetRecord* asset = facility.find_asset(id);
        return asset == nullptr ? TargetRef{} : target_of(TargetKind::Rack, asset->rack.name());
    };

    if (invasive) {
        for (const auto& asset : scoped_assets) {
            push(ObligationKind::AsiDrain, asset_target(asset),
                 "ASI drains the workload from this asset and reports the observed drain", ErrorCode::Ok);
            if (plan.activity != MaintenanceActivity::NetworkWork) {
                push(ObligationKind::AsiQuiesce, asset_target(asset),
                     "ASI quiesces the accelerator state on this asset before it is isolated", ErrorCode::Ok);
            }
        }
        for (const auto& segment : fabric_segments) {
            push(ObligationKind::DfiDrainTraffic, target_of(TargetKind::FabricSegment, segment.name()),
                 "DFI drains tenant traffic from this fabric segment", ErrorCode::Ok);
        }
        if (fabric_segments.empty()) {
            push(ObligationKind::DfiDrainTraffic, anchor,
                 "DFI drains tenant traffic from the maintenance scope", ErrorCode::Ok);
        }
    }
    if (fabric) {
        for (const auto& segment : fabric_segments) {
            push(ObligationKind::DfiIsolatePath, target_of(TargetKind::FabricSegment, segment.name()),
                 "DFI isolates the network path so that work cannot affect live traffic", ErrorCode::Ok);
        }
        if (fabric_segments.empty()) {
            push(ObligationKind::DfiIsolatePath, anchor, "DFI isolates the network path for the maintenance scope",
                 ErrorCode::Ok);
        }
    }
    if (power) {
        for (const auto& domain : power_domains) {
            push(ObligationKind::PowerIsolate, target_of(TargetKind::PowerDomain, domain.name()),
                 "the plant proves the power path is isolated and locked out", ErrorCode::Ok);
        }
        if (power_domains.empty()) {
            push(ObligationKind::PowerIsolate, anchor,
                 "the plant proves the power path for this scope is isolated", ErrorCode::Ok);
        }
    }
    if (cooling) {
        for (const auto& zone : cooling_zones) {
            push(ObligationKind::CoolingAdjust, target_of(TargetKind::CoolingZone, zone.name()),
                 "the plant adjusts cooling and reports the observed setpoint", ErrorCode::Ok);
        }
        if (cooling_zones.empty()) {
            push(ObligationKind::CoolingAdjust, anchor, "the plant adjusts cooling for the maintenance scope",
                 ErrorCode::Ok);
        }
    }
    if (lifecycle) {
        for (const auto& asset : scoped_assets) {
            push(ObligationKind::LifecycleTransition, rack_target(asset),
                 "the inventory owner moves the target to the maintenance lifecycle state",
                 ErrorCode::LifecycleStateInvalid);
        }
    }
    if (facility.policy.require_personnel_evidence) {
        push(ObligationKind::PersonnelOnSite, anchor,
             "an accountable operator confirms attendance and the work permit", ErrorCode::PersonnelEvidenceMissing);
    }
    if (facility.policy.require_dual_approval) {
        push(ObligationKind::ApprovalWitness, anchor, "a second approver witnesses and signs the maintenance window",
             ErrorCode::ApprovalEvidenceMissing);
    }

    if (invasive) {
        if (facility.policy.require_work_stop_evidence) {
            push(ObligationKind::WorkStopConfirmed, anchor,
                 "work is confirmed stopped, with no activity still in flight", ErrorCode::Ok);
        }
        for (const auto& asset : scoped_assets) {
            push(ObligationKind::DrainRelease, asset_target(asset),
                 "ASI confirms the drain is released and the asset is serving again", ErrorCode::Ok);
        }
    }
    if (power) {
        for (const auto& domain : power_domains) {
            push(ObligationKind::PowerRestore, target_of(TargetKind::PowerDomain, domain.name()),
                 "the plant confirms power is restored and the lockout is removed", ErrorCode::Ok);
        }
        if (power_domains.empty()) {
            push(ObligationKind::PowerRestore, anchor, "the plant confirms power is restored for this scope",
                 ErrorCode::Ok);
        }
    }
    if (cooling) {
        for (const auto& zone : cooling_zones) {
            push(ObligationKind::CoolingRestore, target_of(TargetKind::CoolingZone, zone.name()),
                 "the plant confirms cooling is restored to its protected setpoint", ErrorCode::Ok);
        }
        if (cooling_zones.empty()) {
            push(ObligationKind::CoolingRestore, anchor, "the plant confirms cooling is restored for this scope",
                 ErrorCode::Ok);
        }
    }
    if (invasive || fabric) {
        for (const auto& segment : fabric_segments) {
            push(ObligationKind::TrafficRestore, target_of(TargetKind::FabricSegment, segment.name()),
                 "DFI confirms traffic is restored on this fabric segment", ErrorCode::Ok);
        }
        if (fabric_segments.empty()) {
            push(ObligationKind::TrafficRestore, anchor, "DFI confirms traffic is restored for this scope",
                 ErrorCode::Ok);
        }
    }
    if (invasive && !groups.empty()) {
        for (const auto& group : groups) {
            push(ObligationKind::RedundancyRestore, target_of(TargetKind::Site, group.name()),
                 "the plant confirms redundancy in this group is restored to its protected level", ErrorCode::Ok);
        }
    }
    push(ObligationKind::MaintenanceVerified, anchor,
         "the completed work is verified against the change record that authorised it", ErrorCode::Ok);

    std::vector<Obligation> obligations;
    obligations.reserve(specs.size());
    std::size_t index = 0;
    for (const auto& spec : specs) {
        Obligation obligation;
        std::string id = "obligation-";
        id += std::to_string(++index);
        id += '-';
        id += std::string(to_string(spec.kind));
        auto parsed = ObligationId::parse(id);
        if (!parsed.ok()) {
            continue;
        }
        obligation.id = parsed.value();
        obligation.kind = spec.kind;
        obligation.authority = authority_of(spec.kind);
        obligation.target = spec.target;
        obligation.requirement = spec.requirement;
        obligation.mandatory = true;
        obligation.requires_observation = true;
        obligation.guarded_by = spec.guarded_by;
        obligations.push_back(std::move(obligation));
    }
    set.obligations = std::move(obligations);
    set.finalize();
    return set;
}

bool receipt_matches_obligation(ObligationKind obligation, ReceiptKind receipt) noexcept {
    if (receipt == ReceiptKind::FailureObserved) {
        return true;
    }
    if (receipt == satisfying_receipt(obligation)) {
        return true;
    }
    if (obligation == ObligationKind::AsiDrain) {
        return receipt == ReceiptKind::DrainRequested || receipt == ReceiptKind::DrainAcknowledged;
    }
    return false;
}

ObligationStatusReport derive_obligation_status(const PlanRecord& record, Timestamp now) {
    ObligationStatusReport report;
    report.plan_revision = record.plan.revision;
    report.obligation_set_digest = record.obligations.digest;

    for (const auto& obligation : record.obligations.obligations) {
        ObligationStatus status;
        status.id = obligation.id;
        status.kind = obligation.kind;
        status.authority = obligation.authority;
        status.stage = stage_of(obligation.kind);
        status.mandatory = obligation.mandatory;
        status.target = obligation.target;
        status.disposition = Disposition::Outstanding;

        const std::vector<const Receipt*> receipts = record.receipts_for(obligation.id);
        for (const Receipt* receipt : receipts) {
            status.supporting_receipts.push_back(receipt->id);
        }
        const Receipt* latest = nullptr;
        for (const Receipt* receipt : receipts) {
            if (latest == nullptr || latest->observation_sequence < receipt->observation_sequence) {
                latest = receipt;
            }
        }
        if (latest == nullptr) {
            status.detail = "no evidence has been recorded";
        } else if (latest->kind == satisfying_receipt(obligation.kind)) {
            status.disposition = Disposition::Satisfied;
            status.detail = "satisfied by observed evidence from " + latest->issuer;
        } else if (latest->kind == ReceiptKind::FailureObserved) {
            status.disposition = Disposition::Failed;
            status.detail = "the authority reported a failure: " + latest->evidence;
        } else if (is_request_or_acknowledgement(latest->kind)) {
            status.disposition = Disposition::Acknowledged;
            status.detail = "requested or acknowledged, which is not an observed effect";
        } else {
            status.detail = "evidence recorded, but not the observation this obligation requires";
        }

        if ((status.disposition == Disposition::Outstanding || status.disposition == Disposition::Acknowledged) &&
            obligation.guarded_by != ErrorCode::Ok) {
            for (const auto& grant : record.exceptions) {
                if (grant.covers(obligation.guarded_by, obligation.target, now)) {
                    status.disposition = Disposition::Waived;
                    status.waiver = grant.id;
                    status.detail = "waived by exception " + grant.id.name();
                    break;
                }
            }
        }
        report.statuses.push_back(std::move(status));
    }
    report.finalize();

    const auto done = [](Disposition disposition) {
        return disposition == Disposition::Satisfied || disposition == Disposition::Waived ||
               disposition == Disposition::NotApplicable;
    };
    bool all = true;
    bool pre_drain = true;
    bool isolation = true;
    for (const auto& status : report.statuses) {
        const bool satisfied = done(status.disposition);
        if (!satisfied) {
            all = false;
        }
        if (status.stage == ObligationStage::PreDrain && !satisfied) {
            pre_drain = false;
            isolation = false;
        }
        if (status.stage == ObligationStage::Isolation && !satisfied) {
            isolation = false;
        }
    }
    report.all_satisfied = all;
    report.ready_for_pre_drain = pre_drain;
    report.ready_for_isolation = isolation;
    report.ready_for_completion = all;
    report.finalize();
    return report;
}

// ---------------------------------------------------------------------------
// Fencing and outcomes
// ---------------------------------------------------------------------------

Status check_approval_fence(const ApprovalRecord& approval, const PreconditionReport& evaluation,
                            const FacilitySnapshot& facility) {
    Status primary;
    const auto mismatch = [&primary](bool differs, ErrorCode code, std::string subject, std::string detail) {
        if (differs) {
            primary.merge(fail(code, "approval no longer describes the facility it was granted against",
                               std::move(subject), std::move(detail)));
        }
    };
    mismatch(!(approval.control_epoch == facility.control_epoch), ErrorCode::StaleAuthority, "control-epoch",
             "facility control epoch changed");
    mismatch(!(approval.policy_generation == facility.policy_generation), ErrorCode::StalePolicyGeneration,
             "policy-generation", "facility policy generation changed");
    mismatch(!(approval.dependency_generation == facility.dependency_generation),
             ErrorCode::StaleDependencyGeneration, "dependency-generation", "dependency generation changed");
    mismatch(!(approval.capacity_generation == facility.capacity_generation), ErrorCode::StaleCapacityGeneration,
             "capacity-generation", "capacity generation changed");
    mismatch(!(approval.topology_generation == facility.topology_generation), ErrorCode::StaleTopologyGeneration,
             "topology-generation", "topology generation changed");
    mismatch(!(approval.maintenance_generation == facility.maintenance_generation),
             ErrorCode::StaleMaintenanceGeneration, "maintenance-generation", "maintenance generation changed");
    mismatch(!(approval.facility_epoch == facility.facility_epoch), ErrorCode::StaleFacilityEpoch, "facility-epoch",
             "facility epoch changed");
    mismatch(!(approval.policy_digest == evaluation.policy_digest), ErrorCode::StaleApproval, "policy-digest",
             "the policy document changed inside the same generation");
    mismatch(!(approval.dependency_digest == facility.dependency_digest), ErrorCode::StaleApproval,
             "dependency-digest", "the dependency projection changed inside the same generation");
    mismatch(!(approval.facility_digest == evaluation.facility_digest), ErrorCode::StaleApproval, "facility-digest",
             "the facility view changed inside the same generation");
    mismatch(!(approval.evaluation_digest == evaluation.digest), ErrorCode::StaleApproval, "evaluation-digest",
             "the recorded evaluation is not the one that was approved");
    for (const auto& condition : evaluation.blocking()) {
        if (!condition.waiver.empty()) {
            continue;
        }
        primary.merge(fail(condition.condition, "a condition that blocks this plan was re-observed",
                           condition.subject, condition.detail));
    }
    return primary;
}

Status primary_blocker(const PreconditionReport& report) {
    Status primary;
    for (const auto& condition : report.conditions) {
        if (condition.satisfied || !condition.waiver.empty()) {
            continue;
        }
        primary.merge(fail(condition.condition, "maintenance precondition is not satisfied", condition.subject,
                           condition.detail));
    }
    return primary;
}

Digest outcome_digest_of(std::string_view command, const PlanRecord& record, CommitSequence sequence) {
    Writer writer;
    writer.blob(command);
    writer.digest(record.digest);
    writer.generation(sequence);
    return mc::digest_of(purpose::kOutcome, writer.data());
}

Digest outcome_digest_of(std::string_view command, const FacilitySnapshot& facility, CommitSequence sequence) {
    Writer writer;
    writer.blob(command);
    writer.digest(facility.digest);
    writer.generation(sequence);
    return mc::digest_of(purpose::kOutcome, writer.data());
}

Digest receipt_set_digest(const std::vector<Receipt>& receipts) {
    std::vector<Digest> digests;
    digests.reserve(receipts.size());
    for (const auto& receipt : receipts) {
        digests.push_back(receipt.digest);
    }
    return digest_of_digests(purpose::kReceiptSet, std::move(digests));
}

Digest ledger_outcome_digest(std::string_view command, const LedgerState& ledger) {
    Writer writer;
    writer.blob(command);
    writer.generation(ledger.sequence);
    writer.u32(static_cast<std::uint32_t>(ledger.plans.size()));
    for (const auto& entry : ledger.plans) {
        writer.ident(entry.first);
        writer.digest(entry.second.digest);
    }
    if (ledger.facility.has_value()) {
        writer.boolean(true);
        writer.digest(ledger.facility->digest);
    } else {
        writer.boolean(false);
    }
    return mc::digest_of(purpose::kOutcome, writer.data());
}

Digest evidence_digest_of(std::string_view evidence) {
    return mc::digest_of(purpose::kEvidenceDigest, evidence);
}

}  // namespace mc::detail

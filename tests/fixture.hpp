// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// A canonical synthetic facility used by the tests.
//
// The plant itself is synthetic: no physical data-centre hardware is attached
// to this repository, and every number here is chosen by the test author.  The
// processes, files, locks and crashes that the tests exercise are real.

#ifndef MC_TEST_FIXTURE_HPP
#define MC_TEST_FIXTURE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "mc/engine.hpp"
#include "mc/store.hpp"

namespace mc::test {

inline constexpr Timestamp kFixtureObservedAt = 1'700'000'000'000'000'000LL;
inline constexpr Timestamp kWindowStart = 1'700'000'600'000'000'000LL;
inline constexpr Timestamp kWindowEnd = kWindowStart + (8LL * kNanosPerHour);

inline SiteId site_id(const char* name) { return SiteId::parse(name).value(); }
inline RackId rack_id(const char* name) { return RackId::parse(name).value(); }
inline AssetId asset_id(const char* name) { return AssetId::parse(name).value(); }
inline PlanId plan_id(const char* name) { return PlanId::parse(name).value(); }
inline AttemptId attempt_id(const char* name) { return AttemptId::parse(name).value(); }
inline ObligationId obligation_id(const char* name) { return ObligationId::parse(name).value(); }
inline ReceiptId receipt_id(const char* name) { return ReceiptId::parse(name).value(); }
inline ProtectionId protection_id(const char* name) { return ProtectionId::parse(name).value(); }
inline ExceptionId exception_id(const char* name) { return ExceptionId::parse(name).value(); }

inline TargetRef target(TargetKind kind, const char* name) { return TargetRef::make(kind, name).value(); }

// Builds a facility with two racks, four assets, one redundancy group, one
// capacity pool, one power domain, one cooling zone and one fabric segment.
// `epoch` advances the facility epoch so that a test can install a "later"
// model; the other generations move with it unless overridden.
inline FacilitySnapshot make_facility(std::uint64_t epoch = 1, std::uint64_t dependency_generation = 1,
                                     std::uint64_t capacity_generation = 1, std::uint64_t topology_generation = 1,
                                     std::uint64_t maintenance_generation = 1, std::uint64_t policy_generation = 1,
                                     std::uint64_t control_epoch = 1) {
    FacilitySnapshot facility;
    facility.revision = Revision::from_value(1U);
    facility.facility_epoch = FacilityEpoch::from_value(epoch);
    facility.policy_generation = PolicyGeneration::from_value(policy_generation);
    facility.dependency_generation = DependencyGeneration::from_value(dependency_generation);
    facility.capacity_generation = CapacityGeneration::from_value(capacity_generation);
    facility.topology_generation = TopologyGeneration::from_value(topology_generation);
    facility.maintenance_generation = MaintenanceGeneration::from_value(maintenance_generation);
    facility.control_epoch = ControlEpoch::from_value(control_epoch);
    facility.observed_at = kFixtureObservedAt;

    facility.policy.id = PolicyId::parse("policy-facility-default").value();
    facility.policy.revision = Revision::from_value(1U);
    facility.policy.generation = PolicyGeneration::from_value(policy_generation);
    facility.policy.min_redundancy_margin_units = 0;
    facility.policy.min_spare_capacity_units = 1;
    facility.policy.min_power_headroom_milliwatts = 1;
    facility.policy.min_cooling_headroom_units = 1;
    facility.policy.approval_validity_nanos = 8LL * kNanosPerHour;
    facility.policy.max_window_duration_nanos = 7LL * kNanosPerDay;
    facility.policy.max_concurrent_windows_per_rack = 1;
    facility.policy.require_personnel_evidence = true;
    facility.policy.require_dual_approval = false;
    facility.policy.require_work_stop_evidence = true;
    facility.policy.require_restoration_evidence = true;
    facility.policy.finalize();

    const SiteId site = site_id("site-a");
    const RackId rack_one = rack_id("rack-01");
    const RackId rack_two = rack_id("rack-02");
    const char* const names[4] = {"asset-01", "asset-02", "asset-03", "asset-04"};
    for (int index = 0; index < 4; ++index) {
        AssetRecord asset;
        asset.id = asset_id(names[index]);
        asset.rack = index < 2 ? rack_one : rack_two;
        asset.site = site;
        asset.lifecycle = LifecycleState::InService;
        asset.service_class = ServiceClass::BusinessCritical;
        asset.lifecycle_generation = LifecycleGeneration::from_value(1U);
        asset.hardware_generation = HardwareGeneration::from_value(1U);
        asset.firmware_generation = FirmwareGeneration::from_value(1U);
        asset.capacity_units = 1;
        asset.isolatable = true;
        asset.drainable = true;
        asset.healthy = true;
        facility.assets.push_back(asset);
    }

    RedundancyGroup group;
    group.id = RedundancyGroupId::parse("rg-a").value();
    group.site = site;
    group.mode = RedundancyMode::NPlusTwo;
    group.required_units = 2;
    group.observed_available_units = 4;
    group.members = {asset_id("asset-01"), asset_id("asset-02"), asset_id("asset-03"), asset_id("asset-04")};
    facility.redundancy_groups.push_back(group);

    CapacityPool pool;
    pool.id = CapacityPoolId::parse("pool-a").value();
    pool.site = site;
    pool.units_total = 10;
    pool.units_available = 8;
    pool.units_protected = 2;
    facility.capacity_pools.push_back(pool);

    PowerDomain power;
    power.id = PowerDomainId::parse("pd-a").value();
    power.site = site;
    power.headroom.measured = true;
    power.headroom.available_units = 20;
    power.headroom.required_units = 5;
    facility.power_domains.push_back(power);

    CoolingZone cooling;
    cooling.id = CoolingZoneId::parse("cz-a").value();
    cooling.site = site;
    cooling.headroom.measured = true;
    cooling.headroom.available_units = 20;
    cooling.headroom.required_units = 5;
    facility.cooling_zones.push_back(cooling);

    FabricSegment segment;
    segment.id = FabricSegmentId::parse("fs-a").value();
    segment.site = site;
    segment.available_paths = 4;
    segment.required_paths = 2;
    segment.drainable = true;
    facility.fabric_segments.push_back(segment);

    facility.finalize();
    return facility;
}

// A scope that removes exactly two of the four assets: the facility then holds
// its required units with no margin, which is the boundary the tests care about.
inline MaintenanceScope rack_scope(const char* rack = "rack-01") {
    MaintenanceScope scope({target(TargetKind::Rack, rack)});
    scope.canonicalize();
    return scope;
}

inline WindowSpec fixture_window() {
    WindowSpec window;
    window.start = kWindowStart;
    window.end = kWindowEnd;
    window.flexible = false;
    return window;
}

// A clock placed inside the fixture window, so window gating is exercised
// rather than accidentally bypassed.
inline Timestamp mid_window() { return kWindowStart + (2LL * kNanosPerHour); }

// Records every event it is handed.  Used to prove that events are emitted and
// that no sink is invoked while the coordinator lock is held.
class RecordingSink final : public EventSink {
public:
    void on_event(const EngineEvent& event) override { events.push_back(event); }

    std::vector<EngineEvent> events;
};

}  // namespace mc::test

#endif  // MC_TEST_FIXTURE_HPP

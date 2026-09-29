// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// A small, honest benchmark of completed operations.
//
// Methodology, stated once and not repeated per row:
//   * every number is the wall-clock time of *completed* operations, measured
//     around the call that performs them; nothing here measures submission or
//     enqueue latency, because nothing here is queued;
//   * durable operations are measured with the flush enabled, and the same
//     operations are measured again with the flush disabled so that the cost of
//     the commit point is visible instead of assumed;
//   * wall time comes from std::chrono::steady_clock, the process is a single
//     thread, and each measurement is taken once on one host: these are not
//     statistically controlled numbers and are not a before/after claim;
//   * the facility model is SYNTHETIC (four assets on two racks); the store,
//     the journal, the snapshot, the flush and the lock are REAL.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "mc/engine.hpp"
#include "mc/store.hpp"
#include "mc/time.hpp"

using namespace mc;

namespace {

constexpr Timestamp kObservedAt = 1'700'000'000'000'000'000LL;
constexpr Timestamp kWindowStart = kObservedAt + 600'000'000'000LL;
constexpr Timestamp kWindowEnd = kWindowStart + (8LL * kNanosPerHour);

struct Row {
    std::string operation;
    std::uint64_t count{0};
    double seconds{0.0};
    bool durable{false};
};

std::vector<Row> rows;

[[nodiscard]] SiteId site_id(const char* name) { return SiteId::parse(name).value(); }
[[nodiscard]] RackId rack_id(const char* name) { return RackId::parse(name).value(); }
[[nodiscard]] AssetId asset_id(const char* name) { return AssetId::parse(name).value(); }

[[nodiscard]] FacilitySnapshot make_facility() {
    FacilitySnapshot facility;
    facility.revision = Revision::from_value(1U);
    facility.facility_epoch = FacilityEpoch::from_value(1U);
    facility.policy_generation = PolicyGeneration::from_value(1U);
    facility.dependency_generation = DependencyGeneration::from_value(1U);
    facility.capacity_generation = CapacityGeneration::from_value(1U);
    facility.topology_generation = TopologyGeneration::from_value(1U);
    facility.maintenance_generation = MaintenanceGeneration::from_value(1U);
    facility.control_epoch = ControlEpoch::from_value(1U);
    facility.observed_at = kObservedAt;
    facility.policy.id = PolicyId::parse("policy-bench").value();
    facility.policy.revision = Revision::from_value(1U);
    facility.policy.generation = PolicyGeneration::from_value(1U);
    facility.policy.min_power_headroom_milliwatts = 1;
    facility.policy.min_cooling_headroom_units = 1;
    facility.policy.finalize();
    const SiteId site = site_id("site-a");
    const char* const names[4] = {"asset-01", "asset-02", "asset-03", "asset-04"};
    for (int index = 0; index < 4; ++index) {
        AssetRecord asset;
        asset.id = asset_id(names[index]);
        asset.rack = rack_id(index < 2 ? "rack-01" : "rack-02");
        asset.site = site;
        asset.lifecycle = LifecycleState::InService;
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
    group.required_units = 2;
    group.observed_available_units = 4;
    for (const char* name : names) {
        group.members.push_back(asset_id(name));
    }
    facility.redundancy_groups.push_back(group);
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
    facility.finalize();
    return facility;
}

[[nodiscard]] ProposeRequest proposal(const char* reason) {
    ProposeRequest request;
    request.reason = reason;
    request.requested_by = "bench-operator";
    request.activity = MaintenanceActivity::HardwareRepair;
    request.priority = PlanPriority::Routine;
    request.risk = ServiceRiskClass::High;
    MaintenanceScope scope({TargetRef::make(TargetKind::Rack, "rack-01").value()});
    scope.canonicalize();
    request.scope = scope;
    request.window.start = kWindowStart;
    request.window.end = kWindowEnd;
    return request;
}

void report() {
    std::printf("\n%-46s %8s %12s %12s\n", "operation", "count", "seconds", "ops/second");
    for (const auto& row : rows) {
        const double rate = row.seconds > 0.0 ? static_cast<double>(row.count) / row.seconds : 0.0;
        std::printf("%-46s %8llu %12.4f %12.1f%s\n", row.operation.c_str(),
                    static_cast<unsigned long long>(row.count), row.seconds, rate,
                    row.durable ? "  [durable]" : "");
    }
}

[[nodiscard]] int run(bool fsync) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (fsync ? "mc-bench-durable" : "mc-bench-nondurable");
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    ManualClock clock(kWindowStart + kNanosPerHour);
    StoreOptions options;
    options.directory = directory;
    options.fsync = fsync;
    auto opened = Store::open(options);
    if (!opened.ok()) {
        std::printf("could not open the benchmark store: %s\n", std::string(opened.status().render()).c_str());
        return 1;
    }
    auto store = std::move(opened).value();
    Coordinator coordinator(*store, clock, nullptr);
    const std::filesystem::path store_directory = directory;

    InstallFacilityRequest install;
    install.facility = make_facility();
    install.header.attempt = AttemptId::parse("bench-install").value();
    install.header.intent_digest = intent_digest_of(install);
    auto installed = coordinator.install_facility(install);
    if (!installed.ok()) {
        std::printf("facility install failed: %s\n", std::string(installed.status().render()).c_str());
        return 1;
    }

    constexpr int kPlans = 200;
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < kPlans; ++index) {
        ProposeRequest request = proposal("benchmark proposal");
        const std::string attempt = "bench-" + std::to_string(index);
        request.header.attempt = AttemptId::parse(attempt).value();
        request.header.intent_digest = intent_digest_of(request);
        auto proposed = coordinator.propose(request);
        if (!proposed.ok()) {
            std::printf("propose failed: %s\n", std::string(proposed.status().render()).c_str());
            return 1;
        }
    }
    const double propose_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    rows.push_back(Row{"propose (one durable commit each)", kPlans, propose_seconds, fsync});

    // A read of the authoritative ledger, which copies it under the store lock.
    constexpr int kReads = 20000;
    const auto read_start = std::chrono::steady_clock::now();
    std::uint64_t seen = 0;
    for (int index = 0; index < kReads; ++index) {
        auto snapshot = store->snapshot();
        if (!snapshot.ok()) {
            return 1;
        }
        seen += snapshot.value().plans.size();
    }
    const double read_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - read_start).count();
    rows.push_back(Row{"ledger snapshot (in-process read)", kReads, read_seconds, false});
    if (seen == 0U) {
        return 1;
    }

    // Explaining a plan re-derives every obligation status.
    ShowRequest show;
    show.plan = store->cached_ledger().plans.begin()->first;
    constexpr int kExplains = 2000;
    const auto explain_start = std::chrono::steady_clock::now();
    for (int index = 0; index < kExplains; ++index) {
        auto explained = coordinator.explain(ExplainRequest{show.plan});
        if (!explained.ok()) {
            return 1;
        }
    }
    const double explain_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - explain_start).count();
    rows.push_back(Row{"explain (re-derive all obligations)", kExplains, explain_seconds, false});

    // Publishing a verified snapshot, which is where the ledger digest is taken.
    const auto compact_start = std::chrono::steady_clock::now();
    auto compacted = store->compact();
    const double compact_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - compact_start).count();
    if (!compacted.ok()) {
        return 1;
    }
    rows.push_back(Row{"compact (publish verified snapshot)", 1, compact_seconds, true});

    return 0;
}

// Reopening the store recovers the whole ledger from the snapshot and the
// journal.  It runs after the writer that owns the lock has been destroyed,
// because a second writer must be refused while the first holds it - which the
// durability suite proves separately.
[[nodiscard]] int measure_recovery(const std::filesystem::path& directory) {
    constexpr int kOpens = 5;
    const auto open_start = std::chrono::steady_clock::now();
    for (int index = 0; index < kOpens; ++index) {
        StoreOptions reopen_options;
        reopen_options.directory = directory;
        auto reopened = Store::open(reopen_options);
        if (!reopened.ok()) {
            std::printf("reopen failed: %s\n", std::string(reopened.status().render()).c_str());
            return 1;
        }
    }
    const double open_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - open_start).count();
    rows.push_back(Row{"open + recover (snapshot and journal)", kOpens, open_seconds, false});
    return 0;
}

}  // namespace

int main() {
    std::printf("Maintenance Coordinator benchmark\n");
    std::printf("facility model: SYNTHETIC (4 assets, 2 racks, 1 redundancy group)\n");
    std::printf("storage: REAL host filesystem through the durable store\n");
    std::printf("plans per run: 200, single thread, one measurement per row\n");
    const std::filesystem::path durable = std::filesystem::temp_directory_path() / "mc-bench-durable";
    const std::filesystem::path nondurable = std::filesystem::temp_directory_path() / "mc-bench-nondurable";
    if (run(true) != 0) {
        return 1;
    }
    if (measure_recovery(durable) != 0) {
        return 1;
    }
    std::printf("\n-- with the commit flush enabled (the real durable cost) --\n");
    report();
    rows.clear();
    if (run(false) != 0) {
        return 1;
    }
    if (measure_recovery(nondurable) != 0) {
        return 1;
    }
    std::printf("\n-- with the commit flush disabled (for comparison only) --\n");
    report();
    return 0;
}
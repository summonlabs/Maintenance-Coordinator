// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Concurrency, lock exclusion and callback discipline.
//
// The coordinator serialises commands with one state mutex, and it must never
// run a sink while holding that mutex.  The re-entrancy test below proves the
// second rule directly: its sink calls back into the same Coordinator, which
// could only work if the event is delivered after the lock has been released.

#include "../fixture.hpp"
#include "../mc_test.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mc/engine.hpp"

using namespace mc;        // NOLINT(google-build-using-namespace)
using namespace mc::test;  // NOLINT(google-build-using-namespace)

namespace {

// Calls back into the coordinator from inside the event callback.  This is only
// safe because events are emitted after the coordinator lock is released, so it
// is exactly the property under test.
class ReentrantSink final : public EventSink {
public:
    void attach(Coordinator* coordinator) { coordinator_ = coordinator; }

    void on_event(const EngineEvent& event) override {
        const std::lock_guard<std::mutex> guard(mutex_);
        events.push_back(event);
        if (coordinator_ != nullptr && event.plan.empty() == false) {
            ExplainRequest explain;
            explain.plan = event.plan;
            auto explained = coordinator_->explain(explain);
            if (explained.ok()) {
                ++reentrant_reads;
            } else {
                ++reentrant_failures;
            }
        }
    }

    std::mutex mutex_;
    std::vector<EngineEvent> events;
    Coordinator* coordinator_{nullptr};
    int reentrant_reads{0};
    int reentrant_failures{0};
};

ProposeRequest planned_proposal(const std::string& reason) {
    ProposeRequest request;
    request.reason = reason;
    request.requested_by = "operator-1";
    request.activity = MaintenanceActivity::Inspection;  // the least demanding activity
    request.priority = PlanPriority::Routine;
    request.risk = ServiceRiskClass::Low;
    request.scope = rack_scope("rack-02");
    request.window = fixture_window();
    return request;
}

}  // namespace

MC_TEST(parallel_writers_serialise_without_losing_a_command) {
    const std::filesystem::path directory = make_temp_dir("conc-writers");
    ManualClock clock(mid_window());
    StoreOptions options;
    options.directory = directory;
    auto opened = Store::open(options);
    MC_REQUIRE_OK(opened);
    auto store = std::move(opened).value();
    RecordingSink sink;
    Coordinator coordinator(*store, clock, &sink);

    InstallFacilityRequest install;
    install.facility = make_facility();
    install.header.attempt = attempt_id("conc-install");
    install.header.intent_digest = intent_digest_of(install);
    MC_REQUIRE_OK(coordinator.install_facility(install));

    constexpr int kThreads = 8;
    constexpr int kPerThread = 6;
    std::atomic<int> succeeded{0};
    std::atomic<int> refused{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int worker = 0; worker < kThreads; ++worker) {
        workers.emplace_back([&coordinator, worker, &succeeded, &refused] {
            for (int index = 0; index < kPerThread; ++index) {
                ProposeRequest request = planned_proposal("parallel-" + std::to_string(worker) + "-" +
                                                          std::to_string(index));
                request.header.attempt =
                    attempt_id(("conc-" + std::to_string(worker) + "-" + std::to_string(index)).c_str());
                request.header.intent_digest = intent_digest_of(request);
                auto result = coordinator.propose(request);
                if (result.ok()) {
                    ++succeeded;
                } else if (result.status().code() == ErrorCode::DuplicateIdentity) {
                    // Distinct attempts can never collide on identity; treat a
                    // collision as a refusal so the count check below is exact.
                    ++refused;
                } else {
                    ++refused;
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    MC_CHECK_EQ(succeeded.load(), kThreads * kPerThread);
    MC_CHECK_EQ(refused.load(), 0);

    ListRequest list;
    auto listed = coordinator.list(list);
    MC_REQUIRE_OK(listed);
    MC_CHECK_EQ(listed.value().plans.size(), static_cast<std::size_t>(kThreads * kPerThread) + 0U);

    // Every commit sequence is unique and the ledger is internally consistent.
    auto ledger = store->snapshot();
    MC_REQUIRE_OK(ledger);
    std::vector<std::uint64_t> sequences;
    sequences.reserve(ledger.value().plans.size());
    for (const auto& entry : ledger.value().plans) {
        MC_REQUIRE_OK(entry.second.validate());
        if (entry.second.sequence.is_set()) {
            sequences.push_back(entry.second.sequence.value());
        }
    }
    std::sort(sequences.begin(), sequences.end());
    MC_CHECK(std::adjacent_find(sequences.begin(), sequences.end()) == sequences.end());
    MC_CHECK(ledger.value().sequence.is_set());
}

MC_TEST(a_sink_may_call_back_into_the_coordinator) {
    const std::filesystem::path directory = make_temp_dir("conc-reentrant");
    ManualClock clock(mid_window());
    StoreOptions options;
    options.directory = directory;
    auto opened = Store::open(options);
    MC_REQUIRE_OK(opened);
    auto store = std::move(opened).value();
    ReentrantSink sink;
    Coordinator coordinator(*store, clock, &sink);
    sink.attach(&coordinator);

    InstallFacilityRequest install;
    install.facility = make_facility();
    install.header.attempt = attempt_id("reentrant-install");
    install.header.intent_digest = intent_digest_of(install);
    MC_REQUIRE_OK(coordinator.install_facility(install));

    ProposeRequest request = planned_proposal("reentrant");
    request.header.attempt = attempt_id("reentrant-propose");
    request.header.intent_digest = intent_digest_of(request);
    MC_REQUIRE_OK(coordinator.propose(request));

    const std::lock_guard<std::mutex> guard(sink.mutex_);
    MC_CHECK(sink.events.empty() == false);
    MC_CHECK_EQ(sink.reentrant_failures, 0);
}

MC_TEST(readers_run_while_a_writer_commits) {
    const std::filesystem::path directory = make_temp_dir("conc-readers");
    ManualClock clock(mid_window());
    StoreOptions options;
    options.directory = directory;
    auto opened = Store::open(options);
    MC_REQUIRE_OK(opened);
    auto store = std::move(opened).value();
    Coordinator coordinator(*store, clock, nullptr);

    InstallFacilityRequest install;
    install.facility = make_facility();
    install.header.attempt = attempt_id("readers-install");
    install.header.intent_digest = intent_digest_of(install);
    MC_REQUIRE_OK(coordinator.install_facility(install));

    ProposeRequest first = planned_proposal("observed plan");
    first.header.attempt = attempt_id("readers-propose");
    first.header.intent_digest = intent_digest_of(first);
    auto proposed = coordinator.propose(first);
    MC_REQUIRE_OK(proposed);
    const PlanId observed = proposed.value().plan.plan.id;

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::atomic<int> torn{0};
    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&coordinator, &stop, &reads, &torn, &observed] {
            while (!stop.load()) {
                ShowRequest show;
                show.plan = observed;
                auto shown = coordinator.show(show);
                if (!shown.ok()) {
                    ++torn;
                    continue;
                }
                if (!shown.value().plan.validate().ok()) {
                    ++torn;
                    continue;
                }
                ++reads;
            }
        });
    }

    while (reads.load() == 0) {
        std::this_thread::yield();
    }

    for (int index = 0; index < 40; ++index) {
        ProposeRequest request = planned_proposal("writer-" + std::to_string(index));
        request.header.attempt = attempt_id(("readers-w-" + std::to_string(index)).c_str());
        request.header.intent_digest = intent_digest_of(request);
        auto result = coordinator.propose(request);
        MC_CHECK(result.ok());
    }
    stop.store(true);
    for (auto& reader : readers) {
        reader.join();
    }
    MC_CHECK(reads.load() > 0);
    MC_CHECK_EQ(torn.load(), 0);
}

MC_TEST(a_store_snapshot_is_never_observed_half_applied) {
    const std::filesystem::path directory = make_temp_dir("conc-snapshot");
    ManualClock clock(mid_window());
    StoreOptions options;
    options.directory = directory;
    auto opened = Store::open(options);
    MC_REQUIRE_OK(opened);
    auto store = std::move(opened).value();
    Coordinator coordinator(*store, clock, nullptr);

    InstallFacilityRequest install;
    install.facility = make_facility();
    install.header.attempt = attempt_id("snap-install");
    install.header.intent_digest = intent_digest_of(install);
    MC_REQUIRE_OK(coordinator.install_facility(install));

    std::atomic<bool> stop{false};
    std::atomic<int> inconsistent{0};
    std::atomic<int> observations{0};
    std::atomic<int> iterations{0};
    std::thread observer([&store, &stop, &inconsistent, &observations, &iterations] {
        while (!stop.load()) {
            ++iterations;
            auto snapshot = store->snapshot();
            if (!snapshot.ok()) {
                ++inconsistent;
                continue;
            }
            for (const auto& entry : snapshot.value().plans) {
                if (!entry.second.validate().ok() ||
                    (entry.second.sequence.is_set() && snapshot.value().sequence.is_set() &&
                     snapshot.value().sequence < entry.second.sequence)) {
                    ++inconsistent;
                }
            }
            ++observations;
        }
    });

    // Wait for the observer to be genuinely running rather than relying on the
    // scheduler: a claim about concurrent observation must be earned, not timed.
    while (iterations.load() == 0) {
        std::this_thread::yield();
    }

    for (int index = 0; index < 60; ++index) {
        ProposeRequest request = planned_proposal("snapshot-" + std::to_string(index));
        request.header.attempt = attempt_id(("snap-w-" + std::to_string(index)).c_str());
        request.header.intent_digest = intent_digest_of(request);
        MC_CHECK(coordinator.propose(request).ok());
    }
    stop.store(true);
    observer.join();
    MC_CHECK_MSG(iterations.load() > 0,
                 "observer iterations " + std::to_string(iterations.load()) + ", observations " +
                     std::to_string(observations.load()) + ", inconsistent " +
                     std::to_string(inconsistent.load()));
    MC_CHECK(observations.load() > 0);
    MC_CHECK_EQ(inconsistent.load(), 0);
}

MC_TEST_MAIN()

// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// A store, a deterministic clock, an event sink and a coordinator wired
// together, plus the helpers the lifecycle tests share.

#ifndef MC_TEST_HARNESS_HPP
#define MC_TEST_HARNESS_HPP

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fixture.hpp"
#include "mc/engine.hpp"
#include "mc_test.hpp"
#include "mc/store.hpp"

namespace mc::test {

class Harness {
public:
    // `location_is_directory` lets a test own the directory itself, which is what
    // the restart tests need: the same store is opened twice by two processes.
    Harness(const std::string& location, FacilitySnapshot facility, bool location_is_directory = false)
        : directory_(location_is_directory ? std::filesystem::path(location) : make_temp_dir(location)) {
        if (location_is_directory) {
            std::filesystem::create_directories(directory_);
        }
        clock_ = std::make_unique<ManualClock>(mid_window());
        StoreOptions options;
        options.directory = directory_;
        auto opened = Store::open(options);
        if (!opened.ok()) {
            throw std::runtime_error(opened.status().render());
        }
        store_ = std::move(opened).value();
        coordinator_ = std::make_unique<Coordinator>(*store_, *clock_, &sink_);
        InstallFacilityRequest request;
        request.header.actor = "operator-1";
        request.facility = std::move(facility);
        install(request, "install-1");
    }

    [[nodiscard]] Coordinator& coordinator() { return *coordinator_; }
    [[nodiscard]] Store& store() { return *store_; }
    [[nodiscard]] ManualClock& clock() { return *clock_; }
    [[nodiscard]] const std::vector<EngineEvent>& events() const { return sink_.events; }
    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

    template <typename Request>
    Request with_attempt(Request request, const std::string& attempt) {
        request.header.attempt = attempt_id(attempt.c_str());
        request.header.actor = "operator-1";
        request.header.intent_digest = intent_digest_of(request);
        return request;
    }

    Result<CommandResult> install(InstallFacilityRequest& request, const std::string& attempt) {
        request.header.attempt = attempt_id(attempt.c_str());
        request.header.actor = "operator-1";
        request.header.intent_digest = intent_digest_of(request);
        return coordinator_->install_facility(request);
    }

private:
    std::filesystem::path directory_;
    std::unique_ptr<ManualClock> clock_;
    std::unique_ptr<Store> store_;
    std::unique_ptr<Coordinator> coordinator_;
    RecordingSink sink_;
};

}  // namespace mc::test

#endif  // MC_TEST_HARNESS_HPP

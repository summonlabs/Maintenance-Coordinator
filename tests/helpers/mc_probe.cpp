// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// A test-only helper process.
//
// It exists so that claims about crash consistency, cross-process exclusion and
// concurrent readers can be made against real, independent operating-system
// processes rather than against threads inside one address space.  It is built
// only when the tests are built and is never installed.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../fixture.hpp"
#include "mc/engine.hpp"
#include "mc/store.hpp"

using namespace mc;        // NOLINT(google-build-using-namespace)
using namespace mc::test;  // NOLINT(google-build-using-namespace)

namespace {

int usage() {
    std::cout << "usage: mc_probe <command> [arguments]\n"
                 "  hold-lock <dir> <milliseconds>\n"
                 "  try-open <dir>\n"
                 "  crash <dir> <after-mutation|after-commit|after-snapshot-stage|after-snapshot-publish>\n"
                 "  write-loop <dir> <count>\n"
                 "  read-loop <dir> <count>\n"
                 "  verify <dir>\n"
                 "  compact <dir>\n"
                 "  corrupt <dir> <offset> <value>\n"
                 "  truncate <dir> <bytes>\n";
    return 1;
}

StoreOptions::CrashPoint parse_crash_point(const std::string& name, bool& known) {
    known = true;
    if (name == "after-mutation") {
        return StoreOptions::CrashPoint::AfterMutationRecord;
    }
    if (name == "after-commit") {
        return StoreOptions::CrashPoint::AfterCommitRecord;
    }
    if (name == "after-snapshot-stage") {
        return StoreOptions::CrashPoint::AfterSnapshotStage;
    }
    if (name == "after-snapshot-publish") {
        return StoreOptions::CrashPoint::AfterSnapshotPublish;
    }
    known = false;
    return StoreOptions::CrashPoint::None;
}

int report(const Status& status) {
    std::cout << "error: " << status.render() << "\n";
    return status.code() == ErrorCode::LockHeld ? 3 : 2;
}

template <typename Request>
Request attempt(Request request, const std::string& id) {
    request.header.attempt = AttemptId::parse(id).value();
    request.header.actor = "probe";
    request.header.intent_digest = intent_digest_of(request);
    return request;
}

ProposeRequest proposal(const std::string& plan_name, std::size_t index) {
    ProposeRequest request;
    request.reason = "probe maintenance " + std::to_string(index);
    request.requested_by = "probe";
    request.activity = MaintenanceActivity::HardwareRepair;
    request.priority = PlanPriority::Routine;
    request.risk = ServiceRiskClass::High;
    request.scope = rack_scope();
    request.window = fixture_window();
    if (!plan_name.empty()) {
        request.plan_id = PlanId::parse(plan_name).value();
    }
    return request;
}

int command_hold_lock(const std::string& directory, unsigned milliseconds) {
    StoreOptions options;
    options.directory = directory;
    auto opened = Store::open(options);
    if (!opened.ok()) {
        return report(opened.status());
    }
    std::cout << "locked\n" << std::flush;
    Sleep(milliseconds);
    return 0;
}

int command_try_open(const std::string& directory) {
    StoreOptions options;
    options.directory = directory;
    auto opened = Store::open(options);
    if (!opened.ok()) {
        std::cout << "denied\n";
        return report(opened.status());
    }
    std::cout << "acquired " << opened.value()->cached_ledger().sequence.value() << "\n";
    return 0;
}

int command_crash(const std::string& directory, const std::string& point) {
    bool known = false;
    const StoreOptions::CrashPoint crash_point = parse_crash_point(point, known);
    if (!known) {
        return usage();
    }
    StoreOptions options;
    options.directory = directory;
    options.crash_point = crash_point;
    auto opened = Store::open(options);
    if (!opened.ok()) {
        return report(opened.status());
    }
    auto store = std::move(opened).value();
    ManualClock clock(mid_window());
    Coordinator coordinator(*store, clock, nullptr);

    // Attempt identities carry the current sequence so that a probe run against
    // a store that already holds data performs fresh commands rather than
    // replayed ones.
    const std::string tag = std::to_string(store->cached_ledger().sequence.value());
    if (!store->cached_ledger().facility.has_value()) {
        InstallFacilityRequest install;
        install.facility = make_facility();
        auto installed = coordinator.install_facility(attempt(install, "probe-install-" + tag));
        if (!installed.ok()) {
            return report(installed.status());
        }
    }
    auto proposed = coordinator.propose(attempt(proposal("", 1), "probe-propose-" + tag));
    if (!proposed.ok()) {
        return report(proposed.status());
    }
    CompactRequest compact;
    compact.header.attempt =
        AttemptId::parse("probe-compact-" + std::to_string(store->cached_ledger().sequence.value())).value();
    compact.header.intent_digest = intent_digest_of(compact);
    auto compacted = coordinator.compact(compact);
    if (!compacted.ok()) {
        return report(compacted.status());
    }
    std::cout << "no crash point reached\n";
    return 0;
}

int command_write_loop(const std::string& directory, int count) {
    StoreOptions options;
    options.directory = directory;
    auto opened = Store::open(options);
    if (!opened.ok()) {
        return report(opened.status());
    }
    auto store = std::move(opened).value();
    ManualClock clock(mid_window());
    Coordinator coordinator(*store, clock, nullptr);
    if (!store->cached_ledger().facility.has_value()) {
        InstallFacilityRequest install;
        install.facility = make_facility();
        auto installed = coordinator.install_facility(attempt(install, "loop-install"));
        if (!installed.ok()) {
            return report(installed.status());
        }
    }
    // Identities carry the sequence the session started from, so a later
    // session writes new plans instead of replaying the earlier session's
    // attempts.
    const std::uint64_t tag = store->cached_ledger().sequence.is_set()
                                  ? store->cached_ledger().sequence.value()
                                  : 0U;
    for (int index = 0; index < count; ++index) {
        const std::string name = "loop-" + std::to_string(tag) + "-" + std::to_string(index);
        auto proposed = coordinator.propose(attempt(proposal(name, static_cast<std::size_t>(index)), "loop-" + name));
        if (!proposed.ok()) {
            return report(proposed.status());
        }
    }
    std::cout << "wrote " << count << " plans at sequence " << store->cached_ledger().sequence.value() << "\n";
    return 0;
}

int command_read_loop(const std::string& directory, int count) {
    for (int index = 0; index < count; ++index) {
        StoreOptions options;
        options.directory = directory;
        options.read_only = true;
        auto opened = Store::open(options);
        if (!opened.ok()) {
            return report(opened.status());
        }
        auto store = std::move(opened).value();
        const LedgerState& ledger = store->cached_ledger();
        for (const auto& entry : ledger.plans) {
            if (auto status = entry.second.validate(); !status.ok()) {
                std::cout << "torn generation observed: " << status.render() << "\n";
                return 2;
            }
        }
    }
    std::cout << "read " << count << " times\n";
    return 0;
}

int command_verify(const std::string& directory) {
    StoreOptions options;
    options.directory = directory;
    options.read_only = true;
    auto opened = Store::open(options);
    if (!opened.ok()) {
        return report(opened.status());
    }
    auto store = std::move(opened).value();
    const LedgerState& ledger = store->cached_ledger();
    std::size_t recovery_required = 0;
    for (const auto& entry : ledger.plans) {
        if (entry.second.recovery_required) {
            ++recovery_required;
        }
    }
    std::cout << "sequence=" << ledger.sequence.value() << " plans=" << ledger.plans.size()
              << " facility=" << (ledger.facility.has_value() ? 1 : 0)
              << " recovery_required=" << recovery_required << "\n";
    return 0;
}

int command_compact(const std::string& directory) {
    StoreOptions options;
    options.directory = directory;
    auto opened = Store::open(options);
    if (!opened.ok()) {
        return report(opened.status());
    }
    auto store = std::move(opened).value();
    auto compacted = store->compact();
    if (!compacted.ok()) {
        return report(compacted.status());
    }
    std::cout << "compacted at " << compacted.value().value() << "\n";
    return 0;
}

int command_corrupt(const std::string& directory, long offset, int value) {
    const std::filesystem::path path = std::filesystem::path(directory) / "journal.mcl";
    std::vector<char> bytes;
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            std::cout << "error: no journal\n";
            return 2;
        }
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    if (offset < 0 || static_cast<std::size_t>(offset) >= bytes.size()) {
        std::cout << "error: offset outside the journal\n";
        return 2;
    }
    bytes[static_cast<std::size_t>(offset)] = static_cast<char>(value & 0xFF);
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    std::cout << "corrupted byte " << offset << "\n";
    return 0;
}

int command_truncate(const std::string& directory, long bytes_to_keep) {
    const std::filesystem::path path = std::filesystem::path(directory) / "journal.mcl";
    std::error_code error;
    if (bytes_to_keep < 0) {
        std::cout << "error: negative size\n";
        return 2;
    }
    std::filesystem::resize_file(path, static_cast<std::uintmax_t>(bytes_to_keep), error);
    if (error) {
        std::cout << "error: " << error.message() << "\n";
        return 2;
    }
    std::cout << "truncated to " << bytes_to_keep << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }
    const std::string command = argv[1];
    if (command == "hold-lock" && argc == 4) {
        return command_hold_lock(argv[2], static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10)));
    }
    if (command == "try-open" && argc == 3) {
        return command_try_open(argv[2]);
    }
    if (command == "crash" && argc == 4) {
        return command_crash(argv[2], argv[3]);
    }
    if (command == "write-loop" && argc == 4) {
        return command_write_loop(argv[2], static_cast<int>(std::strtol(argv[3], nullptr, 10)));
    }
    if (command == "read-loop" && argc == 4) {
        return command_read_loop(argv[2], static_cast<int>(std::strtol(argv[3], nullptr, 10)));
    }
    if (command == "verify" && argc == 3) {
        return command_verify(argv[2]);
    }
    if (command == "compact" && argc == 3) {
        return command_compact(argv[2]);
    }
    if (command == "corrupt" && argc == 5) {
        return command_corrupt(argv[2], std::strtol(argv[3], nullptr, 10),
                               static_cast<int>(std::strtol(argv[4], nullptr, 10)));
    }
    if (command == "truncate" && argc == 4) {
        return command_truncate(argv[2], std::strtol(argv[3], nullptr, 10));
    }
    return usage();
}

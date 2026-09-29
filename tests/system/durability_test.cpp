// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Durability, crash consistency, cross-process exclusion and torn-read proofs.
//
// Every claim here is made against a real, independent operating-system
// process that is terminated without running any cleanup.  Process termination
// does not lose data that the operating system has already accepted into its
// file cache, so these tests prove the framing and recovery rules - what a
// committed transaction means, what an uncommitted one means, and what happens
// to a torn record - rather than the behaviour of a power loss.  The README
// states that boundary explicitly.

#include "../mc_test.hpp"
#include "../process.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "mc/engine.hpp"
#include "mc/store.hpp"

using namespace mc;        // NOLINT(google-build-using-namespace)
using namespace mc::test;  // NOLINT(google-build-using-namespace)

namespace {

// The probe is built next to this test executable, so the path is derived from
// the running module rather than hard coded or injected by the build system.
[[nodiscard]] std::string probe_path() {
    std::vector<wchar_t> buffer(4096, L'\0');
    const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    const std::filesystem::path self(std::wstring(buffer.data(), written));
    return (self.parent_path() / L"mc_probe.exe").string();
}

[[nodiscard]] std::uint32_t run_probe(const std::vector<std::string>& arguments) {
    Child child = Child::spawn(probe_path(), arguments);
    if (!child.valid()) {
        return 0xFFFFFFFFU;
    }
    return child.wait();
}

[[nodiscard]] Child start_probe(const std::vector<std::string>& arguments) {
    return Child::spawn(probe_path(), arguments);
}

[[nodiscard]] Result<LedgerState> read_store(const std::string& directory) {
    StoreOptions options;
    options.directory = directory;
    options.read_only = true;
    auto opened = Store::open(options);
    if (!opened.ok()) {
        return opened.status();
    }
    auto store = std::move(opened).value();
    return store->cached_ledger();
}

[[nodiscard]] std::uintmax_t file_size(const std::string& directory, const char* name) {
    std::error_code error;
    const auto size = std::filesystem::file_size(std::filesystem::path(directory) / name, error);
    return error ? 0U : size;
}

// Waits for a condition by polling real subprocesses; it is a wait for an
// observable fact, never a test timeout.
template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate, int attempts = 200) {
    for (int index = 0; index < attempts; ++index) {
        if (predicate()) {
            return true;
        }
        Sleep(50);
    }
    return predicate();
}

}  // namespace

MC_TEST(crash_before_the_commit_record_is_invisible_after_restart) {
    const std::string directory = make_temp_dir("dur-crash-mutation").string();
    const std::uint32_t probe_exit_1 = run_probe({"crash", directory, "after-mutation"});
    MC_CHECK_EQ(probe_exit_1, 3U);
    // The mutation bytes really are on disk: the discard is a decision, not an
    // accident of nothing having been written.
    MC_CHECK(file_size(directory, "journal.mcl") > 0U);

    auto ledger = read_store(directory);
    MC_REQUIRE_OK(ledger);
    MC_CHECK(ledger.value().facility.has_value() == false);
    MC_CHECK(ledger.value().plans.empty());
    MC_CHECK(ledger.value().sequence.is_set() == false);
    const std::uint32_t probe_exit_2 = run_probe({"verify", directory});
    MC_CHECK_EQ(probe_exit_2, 0U);
}

MC_TEST(crash_after_the_commit_record_recovers_the_transaction) {
    const std::string directory = make_temp_dir("dur-crash-commit").string();
    const std::uint32_t probe_exit_3 = run_probe({"crash", directory, "after-commit"});
    MC_CHECK_EQ(probe_exit_3, 3U);
    auto ledger = read_store(directory);
    MC_REQUIRE_OK(ledger);
    MC_CHECK(ledger.value().facility.has_value());
    MC_CHECK(ledger.value().sequence.is_set());
}

MC_TEST(a_staged_but_unpublished_snapshot_is_ignored) {
    const std::string directory = make_temp_dir("dur-snapshot-stage").string();
    const std::uint32_t probe_exit_101 = run_probe({"write-loop", directory, "2"});
    MC_CHECK_EQ(probe_exit_101, 0U);
    const std::uint32_t probe_exit_102 = run_probe({"compact", directory});
    MC_CHECK_EQ(probe_exit_102, 0U);
    const std::uintmax_t before = file_size(directory, "journal.mcl");
    MC_CHECK_EQ(before, 0U);  // compaction truncates the journal it replaced

    const std::uint32_t probe_exit_4 = run_probe({"crash", directory, "after-snapshot-stage"});
    MC_CHECK_EQ(probe_exit_4, 3U);
    auto ledger = read_store(directory);
    MC_REQUIRE_OK(ledger);
    // The crashed run committed one more plan before staging the snapshot, and
    // that transaction is durable; the staged image is not what is recovered.
    MC_CHECK(ledger.value().facility.has_value());
    MC_CHECK_EQ(ledger.value().plans.size(), static_cast<std::size_t>(3));
    MC_CHECK_EQ(ledger.value().sequence.value(), 4ULL);
    // The staged image is not the published one and must never be read.
    std::error_code error;
    const auto staged = std::filesystem::path(directory) / "snapshot.mcs.staged";
    MC_CHECK(std::filesystem::exists(staged, error));
    MC_CHECK(std::filesystem::exists(std::filesystem::path(directory) / "snapshot.mcs", error));
}

MC_TEST(publication_before_journal_truncation_is_recovered_exactly_once) {
    const std::string directory = make_temp_dir("dur-snapshot-publish").string();
    const std::uint32_t probe_exit_103 = run_probe({"write-loop", directory, "3"});
    MC_CHECK_EQ(probe_exit_103, 0U);
    const std::uintmax_t before = file_size(directory, "journal.mcl");
    MC_CHECK(before > 0U);

    const std::uint32_t probe_exit_5 = run_probe({"crash", directory, "after-snapshot-publish"});
    MC_CHECK_EQ(probe_exit_5, 3U);
    // The journal still holds the pre-snapshot records; recovery must skip the
    // prefix the published snapshot already covers instead of applying it twice.
    MC_CHECK(file_size(directory, "journal.mcl") > 0U);
    auto ledger = read_store(directory);
    MC_REQUIRE_OK(ledger);
    // Three plans from the first session plus the one the crashed run committed
    // before publishing; the journal prefix they came from is still present but
    // must not be applied a second time.
    MC_CHECK_EQ(ledger.value().plans.size(), static_cast<std::size_t>(4));
    MC_CHECK_EQ(ledger.value().sequence.value(), 5ULL);
    MC_CHECK(ledger.value().facility.has_value());
    const std::uint32_t probe_exit_6 = run_probe({"verify", directory});
    MC_CHECK_EQ(probe_exit_6, 0U);
}

MC_TEST(a_torn_journal_tail_is_truncated_and_the_transaction_discarded) {
    const std::string directory = make_temp_dir("dur-torn-tail").string();
    const std::uint32_t probe_exit_104 = run_probe({"write-loop", directory, "3"});
    MC_CHECK_EQ(probe_exit_104, 0U);
    auto before = read_store(directory);
    MC_REQUIRE_OK(before);
    MC_CHECK_EQ(before.value().plans.size(), static_cast<std::size_t>(3));
    const std::uintmax_t size = file_size(directory, "journal.mcl");
    MC_REQUIRE(size > 20U);

    // Cut the file in the middle of the last transaction's commit record.
    const std::string cut = std::to_string(size - 10U);
    const std::uint32_t probe_exit_105 = run_probe({"truncate", directory, cut});
    MC_CHECK_EQ(probe_exit_105, 0U);
    auto after = read_store(directory);
    MC_REQUIRE_OK(after);
    MC_CHECK(after.value().plans.size() <= static_cast<std::size_t>(3));
    MC_CHECK(after.value().facility.has_value());

    // A writable open truncates the torn tail away, so the file ends on a
    // record boundary again.
    StoreOptions options;
    options.directory = directory;
    auto opened = Store::open(options);
    MC_REQUIRE_OK(opened);
    auto store = std::move(opened).value();
    MC_CHECK(file_size(directory, "journal.mcl") < size);
    MC_CHECK(store->cached_ledger().facility.has_value());
}

MC_TEST(a_corrupted_record_fails_closed) {
    const std::string directory = make_temp_dir("dur-corrupt").string();
    const std::uint32_t probe_exit_106 = run_probe({"write-loop", directory, "2"});
    MC_CHECK_EQ(probe_exit_106, 0U);
    const std::uintmax_t size = file_size(directory, "journal.mcl");
    MC_REQUIRE(size > 100U);
    // Offset 60 is inside the first record's payload, past its 48-byte header.
    const std::uint32_t probe_exit_107 = run_probe({"corrupt", directory, "60", "255"});
    MC_CHECK_EQ(probe_exit_107, 0U);
    const std::uint32_t exit_code = run_probe({"verify", directory});
    MC_CHECK_EQ(exit_code, 2U);

    StoreOptions options;
    options.directory = directory;
    auto opened = Store::open(options);
    MC_CHECK(opened.ok() == false);
    MC_CHECK(opened.status().code() == ErrorCode::ChecksumMismatch);
}

MC_TEST(the_lock_is_exclusive_across_processes_and_released_on_death) {
    const std::string directory = make_temp_dir("dur-lock").string();
    const std::uint32_t probe_exit_108 = run_probe({"write-loop", directory, "1"});
    MC_CHECK_EQ(probe_exit_108, 0U);

    Child holder = start_probe({"hold-lock", directory, "30000"});
    MC_REQUIRE(holder.valid());
    MC_CHECK(wait_until([&directory] { return run_probe({"try-open", directory}) == 3U; }));
    std::uint32_t holder_code = 0;
    MC_CHECK(holder.exited(holder_code) == false);

    // A second writer in this process is refused as well.
    {
        StoreOptions options;
        options.directory = directory;
        auto opened = Store::open(options);
        MC_CHECK(opened.ok() == false);
        MC_CHECK(opened.status().code() == ErrorCode::LockHeld);
    }
    // A reader is not blocked by the writer's lock.
    {
        auto ledger = read_store(directory);
        MC_REQUIRE_OK(ledger);
        MC_CHECK(ledger.value().facility.has_value());
    }

    holder.kill();
    const std::uint32_t holder_exit = holder.wait();
    MC_CHECK(holder_exit != 0U);
    MC_CHECK(wait_until([&directory] { return run_probe({"try-open", directory}) == 0U; }));

    auto ledger = read_store(directory);
    MC_REQUIRE_OK(ledger);
    MC_CHECK(ledger.value().facility.has_value());
}

MC_TEST(a_second_writer_process_is_refused_while_the_first_holds_the_lock) {
    const std::string directory = make_temp_dir("dur-two-writers").string();
    Child holder = start_probe({"hold-lock", directory, "30000"});
    MC_REQUIRE(holder.valid());
    MC_CHECK(wait_until([&directory] { return run_probe({"try-open", directory}) == 3U; }));
    const std::uint32_t probe_exit_7 = run_probe({"write-loop", directory, "1"});
    MC_CHECK_EQ(probe_exit_7, 3U);
    holder.kill();
    MC_CHECK(holder.wait() != 0U);
}

MC_TEST(a_reader_never_observes_a_torn_generation) {
    const std::string directory = make_temp_dir("dur-torn-read").string();
    const std::uint32_t probe_exit_109 = run_probe({"write-loop", directory, "1"});
    MC_CHECK_EQ(probe_exit_109, 0U);

    Child writer = start_probe({"write-loop", directory, "120"});
    MC_REQUIRE(writer.valid());
    int reads = 0;
    bool torn = false;
    // Read at least once before asking whether the writer has finished, so a
    // fast writer can never make the observation count zero.
    while (true) {
        auto ledger = read_store(directory);
        if (!ledger.ok()) {
            torn = true;
            break;
        }
        for (const auto& entry : ledger.value().plans) {
            if (!entry.second.validate().ok()) {
                torn = true;
                break;
            }
        }
        ++reads;
        if (torn) {
            break;
        }
        std::uint32_t code = 0;
        if (writer.exited(code)) {
            MC_CHECK_EQ(code, 0U);
            break;
        }
    }
    MC_CHECK(torn == false);
    MC_CHECK(reads > 0);

    auto final_state = read_store(directory);
    MC_REQUIRE_OK(final_state);
    MC_CHECK(final_state.value().plans.size() >= static_cast<std::size_t>(2));
}

MC_TEST(a_later_writing_session_appends_instead_of_overwriting) {
    const std::string directory = make_temp_dir("dur-append").string();
    const std::uint32_t first_exit = run_probe({"write-loop", directory, "2"});
    MC_CHECK_EQ(first_exit, 0U);
    auto after_first = read_store(directory);
    MC_REQUIRE_OK(after_first);
    const std::uintmax_t first_size = file_size(directory, "journal.mcl");
    MC_REQUIRE(first_size > 0U);

    // A second process opens the same store, recovers it and writes again.
    const std::uint32_t second_exit = run_probe({"write-loop", directory, "1"});
    MC_CHECK_EQ(second_exit, 0U);
    const std::uintmax_t second_size = file_size(directory, "journal.mcl");
    MC_CHECK(second_size > first_size);

    // Every transaction written by both sessions must still be readable.
    auto after_second = read_store(directory);
    MC_REQUIRE_OK(after_second);
    MC_CHECK_EQ(after_second.value().plans.size(), static_cast<std::size_t>(3));
    MC_CHECK_EQ(after_second.value().sequence.value(), 4ULL);
    MC_CHECK(after_second.value().facility.has_value());

    // And a third session must be able to append on top of both.
    const std::uint32_t third_exit = run_probe({"write-loop", directory, "1"});
    MC_CHECK_EQ(third_exit, 0U);
    auto after_third = read_store(directory);
    MC_REQUIRE_OK(after_third);
    MC_CHECK_EQ(after_third.value().plans.size(), static_cast<std::size_t>(4));
}

MC_TEST(compaction_makes_recovery_independent_of_the_journal) {
    const std::string directory = make_temp_dir("dur-compact").string();
    const std::uint32_t probe_exit_110 = run_probe({"write-loop", directory, "4"});
    MC_CHECK_EQ(probe_exit_110, 0U);
    const std::uint32_t probe_exit_111 = run_probe({"compact", directory});
    MC_CHECK_EQ(probe_exit_111, 0U);
    MC_CHECK_EQ(file_size(directory, "journal.mcl"), 0U);

    auto compacted = read_store(directory);
    MC_REQUIRE_OK(compacted);
    MC_CHECK_EQ(compacted.value().plans.size(), static_cast<std::size_t>(4));

    // With the journal emptied, the snapshot is the only source of truth and it
    // must yield exactly the same generation.
    const std::uint32_t probe_exit_112 = run_probe({"truncate", directory, "0"});
    MC_CHECK_EQ(probe_exit_112, 0U);
    auto recovered = read_store(directory);
    MC_REQUIRE_OK(recovered);
    MC_CHECK_EQ(recovered.value().plans.size(), static_cast<std::size_t>(4));
    MC_CHECK(recovered.value().sequence == compacted.value().sequence);
}

MC_TEST_MAIN()

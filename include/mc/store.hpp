// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Durable ledger: versioned, bounded, integrity checked records with an
// explicit atomic commit point and a real OS-level single-writer lock.
//
// Storage layout inside the store directory:
//
//   mc.lock       single-writer lock file (LockFileEx, exclusive, released by
//                 the kernel if the owning process dies)
//   journal.mcl   append-only framed records; the commit point is the commit
//                 record of a transaction, made durable by FlushFileBuffers
//   snapshot.mcs  compacted state image, published by write-stage-verify-rename
//
// Recovery reads the snapshot, then replays only the journal records whose
// commit sequence is strictly greater than the snapshot's, and discards an
// incomplete trailing transaction.  Exactly one generation is ever
// authoritative: a transaction that did not reach its commit record is invisible
// after restart, and a record whose integrity check fails is an error rather
// than a guess.

#ifndef MC_STORE_HPP
#define MC_STORE_HPP

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mc/digest.hpp"
#include "mc/facility.hpp"
#include "mc/ident.hpp"
#include "mc/plan.hpp"
#include "mc/status.hpp"
#include "mc/version.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// Ledger
// ---------------------------------------------------------------------------

struct AttemptRecord {
    Digest intent_digest;
    Digest outcome_digest;
    CommitSequence sequence;
    PlanId plan;
    Timestamp applied_at{kNoTimestamp};

    [[nodiscard]] friend bool operator==(const AttemptRecord&, const AttemptRecord&) noexcept = default;
};

class LedgerState {
public:
    CommitSequence sequence;
    std::uint64_t next_plan_number{1};
    IncarnationId incarnation;
    std::optional<FacilitySnapshot> facility;
    std::map<PlanId, PlanRecord> plans;
    std::map<AttemptId, AttemptRecord> attempts;
    Digest digest;

    void finalize();
    [[nodiscard]] const PlanRecord* find_plan(const PlanId& id) const noexcept;
    [[nodiscard]] const AttemptRecord* find_attempt(const AttemptId& id) const noexcept;

    [[nodiscard]] friend bool operator==(const LedgerState&, const LedgerState&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

enum class RecordKind : std::uint8_t {
    FacilitySnapshot = 1,  // a complete facility model was installed
    PlanRecord = 2,        // one plan record reached a new committed state
    Commit = 3,            // the atomic commit point of a transaction
    SnapshotMarker = 4,    // a compacted snapshot was published at a sequence
};

[[nodiscard]] std::string_view to_string(RecordKind kind) noexcept;

struct StoreOptions {
    std::filesystem::path directory;
    bool read_only{false};
    bool create_if_missing{true};
    // Durable cost is measured with this on; the durability tests also run with
    // it on, because a benchmark that skips the flush is not a durable cost.
    bool fsync{true};
    std::uint32_t max_record_bytes{kMaxRecordPayloadBytes};

    // Failure injection for the crash-consistency proof.  Every point below is
    // exercised by a real process that terminates itself with TerminateProcess,
    // so no destructor, flush or cleanup runs.
    enum class CrashPoint : std::uint8_t {
        None = 0,
        AfterMutationRecord,     // mutation is on disk (unflushed), no commit record
        AfterCommitRecord,       // commit record written, not yet flushed
        BeforeFlush,             // alias of AfterCommitRecord kept for clarity
        AfterSnapshotStage,      // snapshot staged and verified, not published
        AfterSnapshotPublish,    // snapshot renamed into place, journal not truncated
        BeforeJournalTruncate,   // alias of AfterSnapshotPublish
    };
    CrashPoint crash_point{CrashPoint::None};
};

// An attempt record is stored under the identity of the attempt it answers, so
// that a lost response can be replayed idempotently after a restart.
struct AttemptCommit {
    AttemptId id;
    AttemptRecord record;
};

// One atomic transaction: at most one plan record, at most one facility
// snapshot, at most one attempt record, and an optional counter advance.
struct Transaction {
    std::optional<PlanRecord> plan;
    std::optional<FacilitySnapshot> facility;
    std::optional<AttemptCommit> attempt;
    std::uint64_t next_plan_number{0};  // 0 means "unchanged"

    [[nodiscard]] bool empty() const noexcept { return !plan && !facility && !attempt && next_plan_number == 0; }
};

// Recovery facts, reported so the caller can see exactly what was rebuilt.
struct RecoverySummary {
    bool snapshot_loaded{false};
    CommitSequence snapshot_sequence;
    std::uint64_t journal_records_replayed{0};
    std::uint64_t journal_bytes_truncated{0};
    std::uint64_t transactions_discarded{0};
    std::uint64_t plans_recovered{0};
    std::uint64_t plans_requiring_recovery{0};
    IncarnationId prior_incarnation;
    IncarnationId current_incarnation;
};

struct AuditEntry {
    RecordKind kind{RecordKind::PlanRecord};
    CommitSequence sequence;
    std::uint64_t offset{0};
    std::uint32_t payload_bytes{0};
    PlanId plan;
    Digest payload_digest;
};

class Store {
public:
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;
    ~Store();

    // Opens or creates the store, acquires the single-writer lock unless
    // read_only, recovers the ledger, and (when writable) advances the process
    // incarnation exactly once.
    [[nodiscard]] static Result<std::unique_ptr<Store>> open(const StoreOptions& options);

    [[nodiscard]] bool read_only() const noexcept;
    [[nodiscard]] const std::filesystem::path& directory() const noexcept;

    // A copy of the authoritative ledger.  Taken under the store lock, so a
    // caller never observes a half-applied transaction.
    [[nodiscard]] Result<LedgerState> snapshot() const;

    // Commits one transaction.  Returns the new commit sequence.  On failure the
    // in-memory ledger is left exactly as it was, so a caller can retry or fail
    // closed without re-reading.
    [[nodiscard]] Result<CommitSequence> commit(const Transaction& transaction);

    // Publishes a compacted snapshot and truncates the replayed journal prefix.
    [[nodiscard]] Result<CommitSequence> compact();

    // Streams the durable journal and reports the framing of each record.
    [[nodiscard]] Result<std::vector<AuditEntry>> audit() const;

    [[nodiscard]] RecoverySummary recovery() const noexcept;
    [[nodiscard]] const LedgerState& cached_ledger() const noexcept;

private:
    Store();
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mc

#endif  // MC_STORE_HPP

// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Canonical serialization and domain-separated digests.
//
// Two obligations drive this file:
//
//  1. Digest stability.  A digest recorded today must still verify after a
//     process restart, a build change or a toolchain change.  Serialization is
//     therefore fully explicit: fixed-width little-endian integers, length
//     prefixed byte strings, no padding, no enum-size dependence, no struct
//     layout dependence, no locale dependence.
//
//  2. Domain separation.  The same byte stream hashed under two different
//     purposes must produce two different digests, otherwise an attacker (or a
//     bug) could replay evidence recorded for one purpose as evidence for
//     another.  Every digest is prefixed with a purpose tag.
//
// SHA-256 is implemented in-tree (FIPS 180-4) so that the repository stays
// dependency-light while still using a collision-resistant digest for the
// bindings that fence authority.  CRC-32 (IEEE 802.3, reflected) is used only
// for the cheap per-record framing check of the durable formats, where a
// non-cryptographic check is sufficient and fast.

#ifndef MC_DIGEST_HPP
#define MC_DIGEST_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mc/ident.hpp"
#include "mc/status.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------------

// Streaming SHA-256.  Never throws; all state is value state owned by the
// object, so two hashers can be used concurrently from different threads.
class Sha256 {
public:
    Sha256() noexcept { reset(); }

    void reset() noexcept;
    void update(const std::uint8_t* data, std::size_t size) noexcept;
    void update(std::string_view text) noexcept;

    // Finalizes a copy of the state; the object may keep being used afterwards.
    [[nodiscard]] Digest finish() const noexcept;

    // Total bytes consumed since the last reset.
    [[nodiscard]] std::uint64_t byte_count() const noexcept { return bit_length_ / 8U; }

private:
    void process_block(const std::uint8_t* block) noexcept;

    std::uint32_t state_[8];
    std::uint64_t bit_length_;
    std::uint8_t buffer_[64];
    std::size_t buffer_length_;
};

// Convenience wrapper: the same streaming interface with explicit little-endian
// integer encodings, used by canonical object encoders.
class Hasher {
public:
    Hasher() noexcept = default;

    void update(const std::uint8_t* data, std::size_t size) noexcept { sha_.update(data, size); }
    void update(std::string_view text) noexcept { sha_.update(text); }
    void update_u8(std::uint8_t value) noexcept;
    void update_u16(std::uint16_t value) noexcept;
    void update_u32(std::uint32_t value) noexcept;
    void update_u64(std::uint64_t value) noexcept;
    void update_i64(std::int64_t value) noexcept;
    void update_bytes(std::string_view text) noexcept;

    [[nodiscard]] Digest finish() const noexcept { return sha_.finish(); }

private:
    Sha256 sha_;
};

// 32-bit CRC (IEEE 802.3 polynomial, reflected) used by the durable record
// format for fast corruption detection with a stable, specified algorithm.
[[nodiscard]] std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t seed = 0) noexcept;
[[nodiscard]] std::uint32_t crc32(std::string_view text, std::uint32_t seed = 0) noexcept;

// ---------------------------------------------------------------------------
// Canonical writer / reader
// ---------------------------------------------------------------------------

class Writer {
public:
    void u8(std::uint8_t value) { buffer_.push_back(value); }
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value);
    void boolean(bool value) { u8(value ? 1U : 0U); }
    void bytes(const std::uint8_t* data, std::size_t size);
    // Length prefixed (u32) byte string; rejects sizes above kCanonicalMaxBlob.
    void blob(std::string_view value);
    void text(std::string_view value) { blob(value); }
    void digest(const Digest& value) { bytes(value.bytes().data(), value.bytes().size()); }

    template <typename T>
    void generation(const Generation<T>& value) {
        boolean(value.is_set());
        u64(value.value());
    }

    template <typename Tag>
    void ident(const Ident<Tag>& value) {
        blob(value.view());
    }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<std::uint8_t> take() && { return std::move(buffer_); }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
    void clear() noexcept { buffer_.clear(); }

private:
    std::vector<std::uint8_t> buffer_;
};

inline constexpr std::size_t kCanonicalMaxBlob = 64U * 1024U * 1024U;

class Reader {
public:
    explicit Reader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}
    explicit Reader(const std::vector<std::uint8_t>& buffer) noexcept
        : data_(buffer.data()), size_(buffer.size()) {}

    [[nodiscard]] Status u8(std::uint8_t& out);
    [[nodiscard]] Status u16(std::uint16_t& out);
    [[nodiscard]] Status u32(std::uint32_t& out);
    [[nodiscard]] Status u64(std::uint64_t& out);
    [[nodiscard]] Status i64(std::int64_t& out);
    [[nodiscard]] Status boolean(bool& out);
    [[nodiscard]] Status bytes(std::uint8_t* out, std::size_t count);
    [[nodiscard]] Status blob(std::string& out);
    [[nodiscard]] Status text(std::string& out) { return blob(out); }
    [[nodiscard]] Status digest(Digest& out);

    template <typename T>
    [[nodiscard]] Status generation(Generation<T>& out) {
        bool set = false;
        std::uint64_t raw = 0;
        if (auto s = boolean(set); !s.ok()) {
            return s;
        }
        if (auto s = u64(raw); !s.ok()) {
            return s;
        }
        out = set ? Generation<T>::from_value(raw) : Generation<T>{};
        return Status::success();
    }

    template <typename Tag>
    [[nodiscard]] Status ident(Ident<Tag>& out) {
        std::string name;
        if (auto s = blob(name); !s.ok()) {
            return s;
        }
        auto parsed = Ident<Tag>::parse(name);
        if (!parsed.ok()) {
            return parsed.status();
        }
        out = std::move(parsed.value());
        return Status::success();
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    // True when every input byte was consumed.  Record parsers treat a false
    // result as ErrorCode::TrailingBytes.
    [[nodiscard]] bool fully_consumed() const noexcept { return offset_ == size_; }

private:
    [[nodiscard]] Status need(std::size_t count, std::string_view what) const;

    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
    std::size_t offset_{0};
};

// ---------------------------------------------------------------------------
// Purpose tags: never reuse a tag for a different meaning.
// ---------------------------------------------------------------------------

namespace purpose {
inline constexpr std::string_view kPolicy = "mc.policy.v1";
inline constexpr std::string_view kDependencySnapshot = "mc.dependency-snapshot.v1";
inline constexpr std::string_view kProtectedObligation = "mc.protected-obligation.v1";
inline constexpr std::string_view kPlanIntent = "mc.plan-intent.v1";
inline constexpr std::string_view kPlanDigest = "mc.plan.v1";
inline constexpr std::string_view kReceipt = "mc.receipt.v1";
inline constexpr std::string_view kReceiptSet = "mc.receipt-set.v1";
inline constexpr std::string_view kObligation = "mc.obligation.v1";
inline constexpr std::string_view kObligationSet = "mc.obligation-set.v1";
inline constexpr std::string_view kFacilitySnapshot = "mc.facility-snapshot.v1";
inline constexpr std::string_view kExceptionGrant = "mc.exception-grant.v1";
inline constexpr std::string_view kApproval = "mc.approval.v1";
inline constexpr std::string_view kOperationIntent = "mc.operation-intent.v1";
inline constexpr std::string_view kOutcome = "mc.outcome.v1";
inline constexpr std::string_view kPlanRecord = "mc.plan-record.v1";
inline constexpr std::string_view kLedgerState = "mc.ledger-state.v1";
inline constexpr std::string_view kRecordPayload = "mc.record-payload.v1";
inline constexpr std::string_view kSnapshotImage = "mc.snapshot-image.v1";
inline constexpr std::string_view kJournalEntry = "mc.journal-entry.v1";
inline constexpr std::string_view kPhaseTransition = "mc.phase-transition.v1";
inline constexpr std::string_view kProgressEvent = "mc.progress-event.v1";
inline constexpr std::string_view kPreconditionReport = "mc.precondition-report.v1";
inline constexpr std::string_view kEvidenceDigest = "mc.evidence.v1";
}  // namespace purpose

// Hashes `body` under `tag`.  Equivalent to Hasher{}.update(tag).update(body).
[[nodiscard]] Digest digest_of(std::string_view tag, const std::vector<std::uint8_t>& body);
[[nodiscard]] Digest digest_of(std::string_view tag, std::string_view body);

// Hex helpers shared by the durable formats and the CLI.
[[nodiscard]] std::string to_hex(const std::uint8_t* data, std::size_t size);
[[nodiscard]] bool from_hex(std::string_view hex, std::vector<std::uint8_t>& out);

}  // namespace mc

#endif  // MC_DIGEST_HPP

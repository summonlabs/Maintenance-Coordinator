// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// The durable ledger for Windows.
//
// Platform boundary: the store is implemented on the Win32 file API because the
// claims this repository makes about crash consistency, atomic publication and
// cross-process exclusion are claims about the operating system, and they are
// only worth making where they have been proven on the host.  Every other part
// of the library is portable C++20.  A build for another platform fails here
// with a clear message rather than silently degrading to something unproven.

#if !defined(_WIN32)
#error "The Maintenance Coordinator durable store is implemented for Windows (Win32 file API)."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "mc/store.hpp"
#include "mc/version.hpp"
#include "serialize.hpp"

namespace mc {
namespace {

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

// 48 byte header:
//   u32 magic, u16 format version, u16 reserved (must be zero),
//   u32 payload length, u32 payload CRC-32, 32 byte SHA-256 of the payload.
constexpr std::size_t kFrameHeaderBytes = 4U + 2U + 2U + 4U + 4U + kDigestBytes;

// A journal larger than this is rejected rather than loaded: the bound is part
// of the format contract, not an implementation detail.
constexpr std::uint64_t kMaxDurableFileBytes = 512ULL * 1024ULL * 1024ULL;

const std::wstring kExtendedPrefix{L'\\', L'\\', L'?', L'\\'};
const std::wstring kUncPrefix{L'\\', L'\\', L'?', L'\\', L'U', L'N', L'C', L'\\'};
const std::wstring kUncCallerPrefix{L'\\', L'\\'};

[[nodiscard]] std::wstring extended_path(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    std::wstring text = absolute.lexically_normal().wstring();
    if (text.compare(0, kExtendedPrefix.size(), kExtendedPrefix) == 0) {
        return text;
    }
    if (text.compare(0, kUncCallerPrefix.size(), kUncCallerPrefix) == 0) {
        return kUncPrefix + text.substr(kUncCallerPrefix.size());
    }
    return kExtendedPrefix + text;
}

[[nodiscard]] Status win32_failure(std::string_view what, DWORD code) {
    return fail(ErrorCode::IoError, std::string(what), "io", "win32 error " + std::to_string(code));
}

void store_u16(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    out[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void store_u32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        out[offset + index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU);
    }
}

[[nodiscard]] std::uint16_t load_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8U);
}

[[nodiscard]] std::uint32_t load_u32(const std::uint8_t* data) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(data[index]) << (8U * index);
    }
    return value;
}

struct Frame {
    std::uint8_t kind{0};
    std::vector<std::uint8_t> body;
    Digest payload_digest;
    std::size_t next_offset{0};
};

[[nodiscard]] std::vector<std::uint8_t> build_frame(std::uint8_t kind, const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> payload;
    payload.reserve(body.size() + 1U);
    payload.push_back(kind);
    payload.insert(payload.end(), body.begin(), body.end());

    Sha256 hasher;
    hasher.update(payload.data(), payload.size());
    const Digest digest = hasher.finish();

    std::vector<std::uint8_t> frame(kFrameHeaderBytes + payload.size(), 0U);
    store_u32(frame, 0U, kJournalRecordMagic);
    store_u16(frame, 4U, kJournalFormatVersion);
    store_u16(frame, 6U, 0U);
    store_u32(frame, 8U, static_cast<std::uint32_t>(payload.size()));
    store_u32(frame, 12U, crc32(payload.data(), payload.size()));
    std::memcpy(frame.data() + 16U, digest.bytes().data(), kDigestBytes);
    std::memcpy(frame.data() + kFrameHeaderBytes, payload.data(), payload.size());
    return frame;
}

// Reads one frame from `data[offset...]`.  `truncated` reports that the file ends
// inside the frame, which recovery treats as an interrupted append and
// truncates; any other fault is corruption and fails closed.
[[nodiscard]] Status read_frame(const std::uint8_t* data, std::size_t size, std::size_t offset, std::uint32_t max_payload,
                                Frame& out, bool& truncated) {
    truncated = false;
    if (size - offset < kFrameHeaderBytes) {
        truncated = true;
        return Status::success();
    }
    const std::uint32_t magic = load_u32(data + offset);
    if (magic != kJournalRecordMagic) {
        return fail(ErrorCode::RecordCorrupt, "durable record does not start with the expected magic", "journal",
                    "offset " + std::to_string(offset));
    }
    const std::uint16_t version = load_u16(data + offset + 4U);
    if (version != kJournalFormatVersion) {
        return fail(ErrorCode::UnsupportedFormatVersion, "durable record uses an unsupported format version",
                    "journal", std::to_string(version));
    }
    if (load_u16(data + offset + 6U) != 0U) {
        return fail(ErrorCode::ReservedFieldViolation, "durable record sets a field reserved to zero", "journal",
                    "offset " + std::to_string(offset));
    }
    const std::uint32_t payload_length = load_u32(data + offset + 8U);
    if (payload_length == 0U || payload_length > max_payload) {
        return fail(ErrorCode::LimitExceeded, "durable record declares an impossible payload length", "journal",
                    std::to_string(payload_length));
    }
    if (size - offset - kFrameHeaderBytes < payload_length) {
        truncated = true;
        return Status::success();
    }
    const std::uint8_t* payload = data + offset + kFrameHeaderBytes;
    const std::uint32_t expected_crc = load_u32(data + offset + 12U);
    const std::uint32_t actual_crc = crc32(payload, payload_length);
    if (expected_crc != actual_crc) {
        return fail(ErrorCode::ChecksumMismatch, "durable record failed its CRC-32 check", "journal",
                    "offset " + std::to_string(offset));
    }
    Sha256 hasher;
    hasher.update(payload, payload_length);
    const Digest actual_digest = hasher.finish();
    std::array<std::uint8_t, kDigestBytes> stored_bytes{};
    std::memcpy(stored_bytes.data(), data + offset + 16U, kDigestBytes);
    const Digest stored_digest = Digest::from_bytes(stored_bytes);
    if (!(actual_digest == stored_digest)) {
        return fail(ErrorCode::ChecksumMismatch, "durable record failed its SHA-256 check", "journal",
                    "offset " + std::to_string(offset));
    }
    out.kind = payload[0];
    out.body.assign(payload + 1, payload + payload_length);
    out.payload_digest = actual_digest;
    out.next_offset = offset + kFrameHeaderBytes + payload_length;
    return Status::success();
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = INVALID_HANDLE_VALUE; }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    ~UniqueHandle() { close(); }

    void close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE release() noexcept {
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return handle;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] Result<UniqueHandle> open_file(const std::filesystem::path& path, DWORD access, DWORD share, DWORD creation) {
    const std::wstring native = extended_path(path);
    HANDLE handle = CreateFileW(native.c_str(), access, share, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return fail(ErrorCode::IoError, "file does not exist", "io", path.filename().string());
        }
        return win32_failure("failed to open " + path.filename().string(), code);
    }
    return UniqueHandle(handle);
}

// Existence, type and creation all go through the extended-length prefix, so a
// store inside a deep directory tree behaves exactly like one at the root.
[[nodiscard]] Status ensure_directory(const std::filesystem::path& path, bool create) {
    const std::wstring native = extended_path(path);
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return fail(ErrorCode::PathInvalid, "store path exists but is not a directory", "store", path.string());
        }
        return Status::success();
    }
    const DWORD code = GetLastError();
    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
        return win32_failure("failed to inspect the store directory", code);
    }
    if (!create) {
        return fail(ErrorCode::IoError, "store directory does not exist", "store", path.string());
    }
    // Iterating a Windows path yields the root name ("C:") and the root
    // directory as separate components; both already exist and neither can be
    // created.
    const std::filesystem::path root = path.root_path();
    const std::filesystem::path root_name = path.root_name();
    const std::filesystem::path root_directory = path.root_directory();
    std::filesystem::path accumulated = root;
    for (const auto& component : path.lexically_normal()) {
        if (component == root_name || component == root_directory) {
            continue;
        }
        accumulated /= component;
        const std::wstring partial = extended_path(accumulated);
        if (CreateDirectoryW(partial.c_str(), nullptr) == 0) {
            const DWORD create_code = GetLastError();
            if (create_code != ERROR_ALREADY_EXISTS) {
                return fail(ErrorCode::IoError, "failed to create the store directory", "io",
                            accumulated.string() + " (win32 error " + std::to_string(create_code) + ")");
            }
        }
    }
    return Status::success();
}

[[nodiscard]] Status read_file_bytes(const std::filesystem::path& path, std::string& out, bool& exists) {
    exists = false;
    out.clear();
    auto opened = open_file(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
    if (!opened.ok()) {
        if (opened.status().message() == "file does not exist") {
            return Status::success();
        }
        return opened.status();
    }
    UniqueHandle handle = std::move(opened).value();
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle.get(), &size) == 0) {
        return win32_failure("failed to size " + path.filename().string(), GetLastError());
    }
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > kMaxDurableFileBytes) {
        return fail(ErrorCode::LimitExceeded, "durable file exceeds the accepted size", "io",
                    std::to_string(size.QuadPart));
    }
    exists = true;
    out.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < out.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(out.size() - offset, 1U << 20U));
        DWORD read = 0;
        if (ReadFile(handle.get(), out.data() + offset, chunk, &read, nullptr) == 0) {
            return win32_failure("failed to read " + path.filename().string(), GetLastError());
        }
        if (read == 0) {
            break;
        }
        offset += read;
    }
    out.resize(offset);
    return Status::success();
}

[[nodiscard]] Status write_all(HANDLE handle, const void* data, std::size_t size) {
    const auto* cursor = static_cast<const std::uint8_t*>(data);
    std::size_t written = 0;
    while (written < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - written, 1U << 20U));
        DWORD count = 0;
        if (WriteFile(handle, cursor + written, chunk, &count, nullptr) == 0) {
            return win32_failure("failed to write", GetLastError());
        }
        if (count == 0) {
            return fail(ErrorCode::IoError, "write reported no progress", "io");
        }
        written += count;
    }
    return Status::success();
}

[[nodiscard]] Status flush_handle(HANDLE handle) {
    if (FlushFileBuffers(handle) == 0) {
        return win32_failure("failed to flush", GetLastError());
    }
    return Status::success();
}

[[noreturn]] void terminate_now() {
    // A real, unclean process death: no destructors, no CRT shutdown, no flush.
    TerminateProcess(GetCurrentProcess(), 3U);
    // TerminateProcess does not return; the loop keeps the compiler honest.
    for (;;) {
        ::ExitProcess(3U);
    }
}

void crash_if(StoreOptions::CrashPoint configured, StoreOptions::CrashPoint point) {
    if (configured == point) {
        terminate_now();
    }
}

// ---------------------------------------------------------------------------
// Payload composition
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::uint8_t> encode_plan_payload(const Transaction& transaction) {
    Writer writer;
    detail::encode(writer, *transaction.plan);
    writer.boolean(transaction.attempt.has_value());
    if (transaction.attempt.has_value()) {
        writer.ident(transaction.attempt->id);
        detail::encode(writer, transaction.attempt->record);
    }
    writer.u64(transaction.next_plan_number);
    return std::move(writer).take();
}

[[nodiscard]] std::vector<std::uint8_t> encode_facility_payload(const Transaction& transaction) {
    Writer writer;
    detail::encode(writer, *transaction.facility);
    writer.u64(transaction.next_plan_number);
    return std::move(writer).take();
}

[[nodiscard]] Status decode_plan_payload(const std::vector<std::uint8_t>& body, Transaction& transaction) {
    Reader reader(body);
    PlanRecord record;
    if (auto status = detail::decode(reader, record); !status.ok()) {
        return status;
    }
    bool has_attempt = false;
    if (auto status = reader.boolean(has_attempt); !status.ok()) {
        return status;
    }
    if (has_attempt) {
        AttemptCommit commit;
        if (auto status = reader.ident(commit.id); !status.ok()) {
            return status;
        }
        if (auto status = detail::decode(reader, commit.record); !status.ok()) {
            return status;
        }
        transaction.attempt = std::move(commit);
    }
    if (auto status = reader.u64(transaction.next_plan_number); !status.ok()) {
        return status;
    }
    if (!reader.fully_consumed()) {
        return fail(ErrorCode::TrailingBytes, "plan record payload has trailing bytes", "journal");
    }
    transaction.plan = std::move(record);
    return Status::success();
}

[[nodiscard]] Status decode_facility_payload(const std::vector<std::uint8_t>& body, Transaction& transaction) {
    Reader reader(body);
    FacilitySnapshot snapshot;
    if (auto status = detail::decode(reader, snapshot); !status.ok()) {
        return status;
    }
    if (auto status = reader.u64(transaction.next_plan_number); !status.ok()) {
        return status;
    }
    if (!reader.fully_consumed()) {
        return fail(ErrorCode::TrailingBytes, "facility payload has trailing bytes", "journal");
    }
    transaction.facility = std::move(snapshot);
    return Status::success();
}

[[nodiscard]] Status decode_commit_payload(const std::vector<std::uint8_t>& body, detail::CommitPayload& out) {
    Reader reader(body);
    if (auto status = detail::decode(reader, out); !status.ok()) {
        return status;
    }
    if (!reader.fully_consumed()) {
        return fail(ErrorCode::TrailingBytes, "commit payload has trailing bytes", "journal");
    }
    return Status::success();
}

[[nodiscard]] bool is_known_payload_kind(std::uint8_t raw) noexcept {
    return raw >= static_cast<std::uint8_t>(RecordKind::FacilitySnapshot) &&
           raw <= static_cast<std::uint8_t>(RecordKind::SnapshotMarker);
}

}  // namespace

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

class Store::Impl {
public:
    StoreOptions options;
    std::filesystem::path directory;
    std::filesystem::path journal_path;
    std::filesystem::path snapshot_path;
    std::filesystem::path lock_path;

    UniqueHandle lock_handle;
    UniqueHandle journal_handle;

    LedgerState ledger;
    RecoverySummary recovery_summary;
    mutable std::mutex mutex;

    [[nodiscard]] static Result<std::unique_ptr<Impl>> open_store(const StoreOptions& options);
    [[nodiscard]] Result<std::vector<std::uint8_t>> encode_ledger_image() const;
    [[nodiscard]] Status load_snapshot();
    [[nodiscard]] Status replay_journal();
    [[nodiscard]] Status apply_transaction(const Transaction& transaction, CommitSequence sequence);
    [[nodiscard]] Status commit_locked(const Transaction& transaction);
    [[nodiscard]] Status compact_locked();
};

Result<std::unique_ptr<Store::Impl>> Store::Impl::open_store(const StoreOptions& options) {
    if (options.directory.empty()) {
        return fail(ErrorCode::InvalidArgument, "store directory was not provided", "store");
    }
    for (const auto& component : options.directory) {
        if (component == "..") {
            return fail(ErrorCode::PathTraversalRejected, "store directory contains a parent reference", "store",
                        options.directory.string());
        }
    }
    if (options.max_record_bytes == 0U) {
        return fail(ErrorCode::InvalidArgument, "store maximum record size must be positive", "store");
    }

    auto impl = std::unique_ptr<Impl>(new Impl());
    impl->options = options;
    impl->directory = options.directory;
    impl->journal_path = impl->directory / "journal.mcl";
    impl->snapshot_path = impl->directory / "snapshot.mcs";
    impl->lock_path = impl->directory / "mc.lock";

    if (auto status = ensure_directory(impl->directory, options.create_if_missing && !options.read_only);
        !status.ok()) {
        return status;
    }

    if (!options.read_only) {
        auto opened = open_file(impl->lock_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                OPEN_ALWAYS);
        if (!opened.ok()) {
            return opened.status();
        }
        impl->lock_handle = std::move(opened).value();
        OVERLAPPED overlapped{};
        if (LockFileEx(impl->lock_handle.get(),
                       LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) == 0) {
            const DWORD code = GetLastError();
            if (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING) {
                return fail(ErrorCode::LockHeld, "another process holds the single-writer lock", "store",
                            impl->lock_path.string());
            }
            // A lock that cannot even be attempted is a lock error, not a
            // contention result: the caller must not retry it as one.
            return fail(ErrorCode::LockError, "the single-writer lock could not be taken", "store",
                        "win32 error " + std::to_string(code));
        }
    }

    if (auto status = impl->load_snapshot(); !status.ok()) {
        return status;
    }
    if (auto status = impl->replay_journal(); !status.ok()) {
        return status;
    }
    impl->recovery_summary.prior_incarnation = impl->ledger.incarnation;
    impl->ledger.incarnation = impl->ledger.incarnation.next();
    impl->recovery_summary.current_incarnation = impl->ledger.incarnation;
    for (const auto& entry : impl->ledger.plans) {
        ++impl->recovery_summary.plans_recovered;
        if (entry.second.recovery_required) {
            ++impl->recovery_summary.plans_requiring_recovery;
        }
    }
    return impl;
}

Result<std::vector<std::uint8_t>> Store::Impl::encode_ledger_image() const {
    Writer writer;
    detail::encode(writer, ledger);
    return std::move(writer).take();
}

Status Store::Impl::load_snapshot() {
    std::string bytes;
    bool exists = false;
    if (auto status = read_file_bytes(snapshot_path, bytes, exists); !status.ok()) {
        return status;
    }
    if (!exists) {
        return Status::success();
    }
    if (bytes.size() < kFrameHeaderBytes) {
        return fail(ErrorCode::RecordTruncated, "snapshot file is shorter than one frame header", "snapshot");
    }
    Frame frame;
    bool truncated = false;
    if (auto status = read_frame(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), 0U,
                                 options.max_record_bytes, frame, truncated);
        !status.ok()) {
        return status;
    }
    if (truncated) {
        return fail(ErrorCode::RecordTruncated, "snapshot file is incomplete", "snapshot");
    }
    if (frame.next_offset != bytes.size()) {
        return fail(ErrorCode::TrailingBytes, "snapshot file has trailing bytes", "snapshot");
    }
    if (frame.kind != static_cast<std::uint8_t>(RecordKind::SnapshotMarker)) {
        return fail(ErrorCode::LayoutInvalid, "snapshot file does not hold a ledger image", "snapshot");
    }
    LedgerState restored;
    Reader reader(frame.body);
    if (auto status = detail::decode(reader, restored); !status.ok()) {
        return status;
    }
    if (!reader.fully_consumed()) {
        return fail(ErrorCode::TrailingBytes, "snapshot image has trailing bytes", "snapshot");
    }
    recovery_summary.snapshot_loaded = true;
    recovery_summary.snapshot_sequence = restored.sequence;
    ledger = std::move(restored);
    return Status::success();
}

Status Store::Impl::replay_journal() {
    std::string bytes;
    bool exists = false;
    if (auto status = read_file_bytes(journal_path, bytes, exists); !status.ok()) {
        return status;
    }
    if (!exists || bytes.empty()) {
        return Status::success();
    }

    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::size_t size = bytes.size();
    std::size_t offset = 0;
    std::size_t truncate_at = size;
    bool have_pending = false;
    std::uint8_t pending_kind = 0;
    std::vector<std::uint8_t> pending_body;
    Digest pending_digest;
    std::size_t pending_offset = 0;
    CommitSequence last_applied;
    bool any_applied = false;

    while (offset < size) {
        Frame frame;
        bool truncated = false;
        if (auto status = read_frame(data, size, offset, options.max_record_bytes, frame, truncated); !status.ok()) {
            return status;
        }
        if (truncated) {
            truncate_at = offset;
            break;
        }
        if (!is_known_payload_kind(frame.kind)) {
            return fail(ErrorCode::ReservedFieldViolation, "durable record holds an unknown record kind", "journal",
                        std::to_string(frame.kind));
        }
        const auto kind = static_cast<RecordKind>(frame.kind);
        if (kind == RecordKind::Commit) {
            if (!have_pending) {
                return fail(ErrorCode::LayoutInvalid, "commit record is not preceded by a mutation record",
                            "journal", "offset " + std::to_string(offset));
            }
            detail::CommitPayload commit;
            if (auto status = decode_commit_payload(frame.body, commit); !status.ok()) {
                return status;
            }
            if (!commit.sequence.is_set()) {
                return fail(ErrorCode::MissingGeneration, "commit record carries no commit sequence", "journal");
            }
            if (!(commit.transaction_digest == pending_digest)) {
                return fail(ErrorCode::ChecksumMismatch,
                            "commit record does not digest the mutation it commits", "journal",
                            "offset " + std::to_string(pending_offset));
            }
            if (commit.mutation_bytes != pending_body.size()) {
                return fail(ErrorCode::LayoutInvalid, "commit record disagrees with the mutation size", "journal",
                            "offset " + std::to_string(pending_offset));
            }
            if (any_applied && !(last_applied < commit.sequence)) {
                return fail(ErrorCode::CommitSequenceRegression, "commit sequences are not strictly increasing",
                            "journal", std::to_string(commit.sequence.value()));
            }
            bool already_in_snapshot = false;
            if (recovery_summary.snapshot_loaded) {
                already_in_snapshot = !(recovery_summary.snapshot_sequence < commit.sequence);
            }
            if (!already_in_snapshot) {
                Transaction transaction;
                if (pending_kind == static_cast<std::uint8_t>(RecordKind::PlanRecord)) {
                    if (auto status = decode_plan_payload(pending_body, transaction); !status.ok()) {
                        return status;
                    }
                } else if (pending_kind == static_cast<std::uint8_t>(RecordKind::FacilitySnapshot)) {
                    if (auto status = decode_facility_payload(pending_body, transaction); !status.ok()) {
                        return status;
                    }
                } else {
                    return fail(ErrorCode::LayoutInvalid, "journal holds a mutating record kind that cannot be applied",
                                "journal", std::to_string(pending_kind));
                }
                if (auto status = apply_transaction(transaction, commit.sequence); !status.ok()) {
                    return status;
                }
                last_applied = commit.sequence;
                any_applied = true;
                ++recovery_summary.journal_records_replayed;
            }
            have_pending = false;
            pending_body.clear();
            offset = frame.next_offset;
            ++recovery_summary.journal_records_replayed;
            continue;
        }
        if (kind == RecordKind::SnapshotMarker) {
            if (have_pending) {
                return fail(ErrorCode::LayoutInvalid, "snapshot marker interrupts a transaction", "journal");
            }
            offset = frame.next_offset;
            continue;
        }
        if (have_pending) {
            return fail(ErrorCode::LayoutInvalid, "two mutation records appear without a commit record", "journal",
                        "offset " + std::to_string(pending_offset));
        }
        have_pending = true;
        pending_kind = frame.kind;
        pending_body = frame.body;
        pending_digest = frame.payload_digest;
        pending_offset = offset;
        offset = frame.next_offset;
    }

    if (have_pending) {
        // The transaction never reached its commit record: it is not durable
        // state and must not be merged into the recovered generation.
        truncate_at = pending_offset;
        ++recovery_summary.transactions_discarded;
    }

    if (truncate_at < size) {
        recovery_summary.journal_bytes_truncated = static_cast<std::uint64_t>(size - truncate_at);
        if (!options.read_only) {
            auto opened = open_file(journal_path, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING);
            if (!opened.ok()) {
                return opened.status();
            }
            UniqueHandle handle = std::move(opened).value();
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(truncate_at);
            if (SetFilePointerEx(handle.get(), position, nullptr, FILE_BEGIN) == 0) {
                return win32_failure("failed to position the journal for truncation", GetLastError());
            }
            if (SetEndOfFile(handle.get()) == 0) {
                return win32_failure("failed to truncate the journal", GetLastError());
            }
            if (auto status = flush_handle(handle.get()); !status.ok()) {
                return status;
            }
        }
    }
    return Status::success();
}

Status Store::Impl::apply_transaction(const Transaction& transaction, CommitSequence sequence) {
    if (transaction.plan.has_value()) {
        const PlanRecord& record = *transaction.plan;
        if (auto status = record.validate(); !status.ok()) {
            return status;
        }
        const auto existing = ledger.plans.find(record.plan.id);
        if (existing != ledger.plans.end() && existing->second.sequence.is_set() && record.sequence.is_set() &&
            !(existing->second.sequence < record.sequence)) {
            // A record that is not newer than the one already held is ignored:
            // replaying is idempotent and never rewinds a plan.
            return Status::success();
        }
        ledger.plans[record.plan.id] = record;
    }
    if (transaction.facility.has_value()) {
        const FacilitySnapshot& snapshot = *transaction.facility;
        if (auto status = snapshot.validate(); !status.ok()) {
            return status;
        }
        ledger.facility = snapshot;
    }
    if (transaction.attempt.has_value()) {
        ledger.attempts[transaction.attempt->id] = transaction.attempt->record;
    }
    if (transaction.next_plan_number != 0U) {
        ledger.next_plan_number = transaction.next_plan_number;
    }
    ledger.sequence = sequence;
    return Status::success();
}

Status Store::Impl::commit_locked(const Transaction& transaction) {
    if (options.read_only) {
        return fail(ErrorCode::ReadOnlyStore, "store was opened read-only", "store", directory.string());
    }
    if (transaction.empty()) {
        return fail(ErrorCode::InvalidArgument, "transaction carries no mutation", "store");
    }
    if (!journal_handle.valid()) {
        // FILE_APPEND_DATA, not GENERIC_WRITE: a fresh handle starts at offset
        // zero, so opening for ordinary write access would overwrite every
        // earlier transaction from the beginning of the file.
        auto opened = open_file(journal_path, FILE_APPEND_DATA | GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS);
        if (!opened.ok()) {
            return opened.status();
        }
        journal_handle = std::move(opened).value();
    }

    CommitSequence next = ledger.sequence.is_set() ? ledger.sequence.next() : CommitSequence::from_value(1U);

    // The commit sequence is assigned before the payload is encoded, so the
    // record that reaches the disk already names the transaction it belongs to;
    // a record that lost its sequence on the way to the journal would look like
    // a plan that had never been committed.
    Transaction stamped = transaction;
    if (stamped.plan.has_value()) {
        stamped.plan->sequence = next;
        stamped.plan->finalize();
    }

    std::vector<std::uint8_t> mutation_body;
    std::uint8_t mutation_kind = 0;
    if (stamped.plan.has_value()) {
        mutation_kind = static_cast<std::uint8_t>(RecordKind::PlanRecord);
        mutation_body = encode_plan_payload(stamped);
    } else if (stamped.facility.has_value()) {
        mutation_kind = static_cast<std::uint8_t>(RecordKind::FacilitySnapshot);
        mutation_body = encode_facility_payload(transaction);
    } else {
        return fail(ErrorCode::InvalidArgument,
                    "transaction holds an attempt or a counter without a record to store", "store");
    }

    const std::vector<std::uint8_t> mutation_frame = build_frame(mutation_kind, mutation_body);
    // The transaction digest covers the payload exactly as it was framed
    // (kind byte included), so recovery can compare it with the digest the
    // frame carries without re-deriving the framing rules.
    Sha256 hasher;
    hasher.update(mutation_frame.data() + kFrameHeaderBytes, mutation_frame.size() - kFrameHeaderBytes);
    const Digest transaction_digest = hasher.finish();

    if (auto status = write_all(journal_handle.get(), mutation_frame.data(), mutation_frame.size()); !status.ok()) {
        return status;
    }
    crash_if(options.crash_point, StoreOptions::CrashPoint::AfterMutationRecord);

    detail::CommitPayload commit;
    commit.sequence = next;
    commit.transaction_digest = transaction_digest;
    commit.mutation_bytes = static_cast<std::uint32_t>(mutation_body.size());
    Writer commit_writer;
    detail::encode(commit_writer, commit);
    const std::vector<std::uint8_t> commit_frame =
        build_frame(static_cast<std::uint8_t>(RecordKind::Commit), commit_writer.data());

    if (auto status = write_all(journal_handle.get(), commit_frame.data(), commit_frame.size()); !status.ok()) {
        return status;
    }
    crash_if(options.crash_point, StoreOptions::CrashPoint::AfterCommitRecord);
    crash_if(options.crash_point, StoreOptions::CrashPoint::BeforeFlush);

    if (options.fsync) {
        if (auto status = flush_handle(journal_handle.get()); !status.ok()) {
            return status;
        }
    }

    // The durable write has reached its commit point; only now does the
    // in-memory ledger advance, so a caller that sees a failure never sees a
    // state the disk does not have.  The in-memory record is byte-identical to
    // the one just written, which is what makes a restart invisible to a client.
    return apply_transaction(stamped, next);
}

Status Store::Impl::compact_locked() {
    if (options.read_only) {
        return fail(ErrorCode::ReadOnlyStore, "store was opened read-only", "store", directory.string());
    }
    ledger.finalize();
    auto image = encode_ledger_image();
    if (!image.ok()) {
        return image.status();
    }
    const std::vector<std::uint8_t> frame =
        build_frame(static_cast<std::uint8_t>(RecordKind::SnapshotMarker), image.value());

    const std::filesystem::path staged = std::filesystem::path(snapshot_path.string() + ".staged");
    {
        auto opened = open_file(staged, GENERIC_WRITE, 0, CREATE_ALWAYS);
        if (!opened.ok()) {
            return opened.status();
        }
        UniqueHandle handle = std::move(opened).value();
        if (auto status = write_all(handle.get(), frame.data(), frame.size()); !status.ok()) {
            return status;
        }
        if (options.fsync) {
            if (auto status = flush_handle(handle.get()); !status.ok()) {
                return status;
            }
        }
    }

    crash_if(options.crash_point, StoreOptions::CrashPoint::AfterSnapshotStage);

    // Read back the staged image and verify it before it can replace the
    // published snapshot: an unverified image is never published.
    std::string staged_bytes;
    bool exists = false;
    if (auto status = read_file_bytes(staged, staged_bytes, exists); !status.ok()) {
        return status;
    }
    if (!exists || staged_bytes.size() != frame.size() ||
        std::memcmp(staged_bytes.data(), frame.data(), frame.size()) != 0) {
        return fail(ErrorCode::PublicationFailed, "staged snapshot did not verify on read back", "snapshot");
    }

    const std::wstring from = extended_path(staged);
    const std::wstring to = extended_path(snapshot_path);
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return win32_failure("failed to publish the snapshot", GetLastError());
    }
    recovery_summary.snapshot_loaded = true;
    recovery_summary.snapshot_sequence = ledger.sequence;

    crash_if(options.crash_point, StoreOptions::CrashPoint::AfterSnapshotPublish);
    crash_if(options.crash_point, StoreOptions::CrashPoint::BeforeJournalTruncate);

    journal_handle.close();
    {
        auto opened = open_file(journal_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                OPEN_ALWAYS);
        if (!opened.ok()) {
            return opened.status();
        }
        UniqueHandle handle = std::move(opened).value();
        if (SetEndOfFile(handle.get()) == 0) {
            return win32_failure("failed to truncate the journal after publication", GetLastError());
        }
        if (options.fsync) {
            if (auto status = flush_handle(handle.get()); !status.ok()) {
                return status;
            }
        }
    }
    std::error_code error;
    std::filesystem::remove(staged, error);
    return Status::success();
}

Store::Store() : impl_(std::make_unique<Impl>()) {}

Store::~Store() = default;

Result<std::unique_ptr<Store>> Store::open(const StoreOptions& options) {
    auto impl = Impl::open_store(options);
    if (!impl.ok()) {
        return impl.status();
    }
    auto store = std::unique_ptr<Store>(new Store());
    store->impl_ = std::move(impl).value();
    return store;
}

bool Store::read_only() const noexcept {
    return impl_->options.read_only;
}

const std::filesystem::path& Store::directory() const noexcept {
    return impl_->directory;
}

Result<LedgerState> Store::snapshot() const {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->ledger;
}

const LedgerState& Store::cached_ledger() const noexcept {
    return impl_->ledger;
}

RecoverySummary Store::recovery() const noexcept {
    return impl_->recovery_summary;
}

Result<CommitSequence> Store::commit(const Transaction& transaction) {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    if (auto status = impl_->commit_locked(transaction); !status.ok()) {
        return status;
    }
    return impl_->ledger.sequence;
}

Result<CommitSequence> Store::compact() {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    if (auto status = impl_->compact_locked(); !status.ok()) {
        return status;
    }
    return impl_->ledger.sequence;
}

Result<std::vector<AuditEntry>> Store::audit() const {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    std::string bytes;
    bool exists = false;
    if (auto status = read_file_bytes(impl_->journal_path, bytes, exists); !status.ok()) {
        return status;
    }
    std::vector<AuditEntry> entries;
    if (!exists) {
        return entries;
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        Frame frame;
        bool truncated = false;
        if (auto status = read_frame(data, bytes.size(), offset, impl_->options.max_record_bytes, frame, truncated);
            !status.ok()) {
            return status;
        }
        if (truncated) {
            break;
        }
        AuditEntry entry;
        entry.kind = static_cast<RecordKind>(frame.kind);
        entry.offset = offset;
        entry.payload_bytes = static_cast<std::uint32_t>(frame.body.size());
        entry.payload_digest = frame.payload_digest;
        if (entry.kind == RecordKind::PlanRecord) {
            Transaction transaction;
            if (auto status = decode_plan_payload(frame.body, transaction); status.ok()) {
                if (transaction.plan.has_value()) {
                    entry.plan = transaction.plan->plan.id;
                    entry.sequence = transaction.plan->sequence;
                }
            }
        } else if (entry.kind == RecordKind::Commit) {
            detail::CommitPayload commit;
            if (auto status = decode_commit_payload(frame.body, commit); status.ok()) {
                entry.sequence = commit.sequence;
            }
        }
        entries.push_back(std::move(entry));
        offset = frame.next_offset;
    }
    return entries;
}

// ---------------------------------------------------------------------------
// Ledger
// ---------------------------------------------------------------------------

void LedgerState::finalize() {
    digest = detail::digest_of(*this);
}

const PlanRecord* LedgerState::find_plan(const PlanId& id) const noexcept {
    const auto found = plans.find(id);
    return found == plans.end() ? nullptr : &found->second;
}

const AttemptRecord* LedgerState::find_attempt(const AttemptId& id) const noexcept {
    const auto found = attempts.find(id);
    return found == attempts.end() ? nullptr : &found->second;
}

std::string_view to_string(RecordKind kind) noexcept {
    switch (kind) {
        case RecordKind::FacilitySnapshot:
            return "facility-snapshot";
        case RecordKind::PlanRecord:
            return "plan-record";
        case RecordKind::Commit:
            return "commit";
        case RecordKind::SnapshotMarker:
            return "snapshot-marker";
        default:
            return "unknown";
    }
}

}  // namespace mc

// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Version and on-disk format constants.
//
// Format versions are part of the compatibility contract: a reader that meets
// an unknown version must reject the input rather than guess at its meaning.

#ifndef MC_VERSION_HPP
#define MC_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace mc {

inline constexpr std::string_view kProductName = "Maintenance Coordinator";
inline constexpr std::string_view kProductSlug = "maintenance-coordinator";
inline constexpr std::string_view kLibraryVersion = "1.0.0";
inline constexpr std::string_view kCopyrightLine = "Copyright 2026 Summon Software Labs";

// Durable formats.  Both are bounded and integrity checked; see store.hpp.
inline constexpr std::uint16_t kJournalFormatVersion = 1;
inline constexpr std::uint16_t kSnapshotFormatVersion = 1;

// Canonical record framing magic values ("MCJ1", "MCS1").
inline constexpr std::uint32_t kJournalRecordMagic = 0x314A434DU;
inline constexpr std::uint32_t kSnapshotMagic = 0x3153434DU;

// Upper bound on a single framed record payload.  Larger inputs are rejected
// with ErrorCode::LimitExceeded rather than allocated.
inline constexpr std::uint32_t kMaxRecordPayloadBytes = 64U * 1024U * 1024U;

// Upper bound on a JSON document accepted by the CLI entry points.
inline constexpr std::size_t kMaxTextDocumentBytes = 32U * 1024U * 1024U;

}  // namespace mc

#endif  // MC_VERSION_HPP

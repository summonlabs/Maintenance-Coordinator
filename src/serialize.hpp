// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// INTERNAL canonical codec.
//
// Every domain value has exactly one canonical encoding: fixed width little
// endian integers, length prefixed byte strings, explicit enum bytes, explicit
// optional presence, no padding and no dependence on struct layout, compiler
// enum size or locale.  Digests are computed over these encodings and the
// durable formats embed them, so an encoding change is a format change.
//
// Derived digest members are deliberately excluded from the encoding; the
// durable frame carries a SHA-256 over the whole payload, which covers those
// fields too, and every digest is recomputed on decode.

#ifndef MC_INTERNAL_SERIALIZE_HPP
#define MC_INTERNAL_SERIALIZE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "mc/approval.hpp"
#include "mc/digest.hpp"
#include "mc/facility.hpp"
#include "mc/obligation.hpp"
#include "mc/plan.hpp"
#include "mc/scope.hpp"
#include "mc/store.hpp"

namespace mc::detail {

inline constexpr std::uint32_t kMaxCollectionItems = 1048576U;

// ---------------------------------------------------------------------------
// Encoders / decoders
// ---------------------------------------------------------------------------

void encode(Writer& writer, const TargetRef& value);
[[nodiscard]] Status decode(Reader& reader, TargetRef& value);

void encode(Writer& writer, const MaintenanceScope& value);
[[nodiscard]] Status decode(Reader& reader, MaintenanceScope& value);

void encode(Writer& writer, const WindowSpec& value);
[[nodiscard]] Status decode(Reader& reader, WindowSpec& value);

void encode(Writer& writer, const Headroom& value);
[[nodiscard]] Status decode(Reader& reader, Headroom& value);

void encode(Writer& writer, const PolicyDocument& value);
[[nodiscard]] Status decode(Reader& reader, PolicyDocument& value);

void encode(Writer& writer, const AssetRecord& value);
[[nodiscard]] Status decode(Reader& reader, AssetRecord& value);

void encode(Writer& writer, const RedundancyGroup& value);
[[nodiscard]] Status decode(Reader& reader, RedundancyGroup& value);

void encode(Writer& writer, const CapacityPool& value);
[[nodiscard]] Status decode(Reader& reader, CapacityPool& value);

void encode(Writer& writer, const PowerDomain& value);
[[nodiscard]] Status decode(Reader& reader, PowerDomain& value);

void encode(Writer& writer, const CoolingZone& value);
[[nodiscard]] Status decode(Reader& reader, CoolingZone& value);

void encode(Writer& writer, const FabricSegment& value);
[[nodiscard]] Status decode(Reader& reader, FabricSegment& value);

void encode(Writer& writer, const BlackoutPeriod& value);
[[nodiscard]] Status decode(Reader& reader, BlackoutPeriod& value);

void encode(Writer& writer, const IncidentRecord& value);
[[nodiscard]] Status decode(Reader& reader, IncidentRecord& value);

void encode(Writer& writer, const ProtectedObligation& value);
[[nodiscard]] Status decode(Reader& reader, ProtectedObligation& value);

void encode(Writer& writer, const FacilitySnapshot& value);
[[nodiscard]] Status decode(Reader& reader, FacilitySnapshot& value);

void encode(Writer& writer, const Obligation& value);
[[nodiscard]] Status decode(Reader& reader, Obligation& value);

void encode(Writer& writer, const ObligationSet& value);
[[nodiscard]] Status decode(Reader& reader, ObligationSet& value);

void encode(Writer& writer, const Receipt& value);
[[nodiscard]] Status decode(Reader& reader, Receipt& value);

void encode(Writer& writer, const ObligationStatus& value);
[[nodiscard]] Status decode(Reader& reader, ObligationStatus& value);

void encode(Writer& writer, const ObligationStatusReport& value);
[[nodiscard]] Status decode(Reader& reader, ObligationStatusReport& value);

void encode(Writer& writer, const ConditionResult& value);
[[nodiscard]] Status decode(Reader& reader, ConditionResult& value);

void encode(Writer& writer, const PreconditionReport& value);
[[nodiscard]] Status decode(Reader& reader, PreconditionReport& value);

void encode(Writer& writer, const ApprovalRecord& value);
[[nodiscard]] Status decode(Reader& reader, ApprovalRecord& value);

void encode(Writer& writer, const ExceptionGrant& value);
[[nodiscard]] Status decode(Reader& reader, ExceptionGrant& value);

void encode(Writer& writer, const MaintenancePlan& value);
[[nodiscard]] Status decode(Reader& reader, MaintenancePlan& value);

void encode(Writer& writer, const PhaseTransition& value);
[[nodiscard]] Status decode(Reader& reader, PhaseTransition& value);

void encode(Writer& writer, const ProgressEvent& value);
[[nodiscard]] Status decode(Reader& reader, ProgressEvent& value);

void encode(Writer& writer, const PlanRecord& value);
[[nodiscard]] Status decode(Reader& reader, PlanRecord& value);

void encode(Writer& writer, const RestorationReport& value);
[[nodiscard]] Status decode(Reader& reader, RestorationReport& value);

void encode(Writer& writer, const CompletionReport& value);
[[nodiscard]] Status decode(Reader& reader, CompletionReport& value);

void encode(Writer& writer, const RecoveryReport& value);
[[nodiscard]] Status decode(Reader& reader, RecoveryReport& value);

void encode(Writer& writer, const Explanation& value);
[[nodiscard]] Status decode(Reader& reader, Explanation& value);

void encode(Writer& writer, const AttemptRecord& value);
[[nodiscard]] Status decode(Reader& reader, AttemptRecord& value);

void encode(Writer& writer, const LedgerState& value);
[[nodiscard]] Status decode(Reader& reader, LedgerState& value);

// ---------------------------------------------------------------------------
// Canonical digests
// ---------------------------------------------------------------------------

[[nodiscard]] Digest digest_of(const MaintenanceScope& value);
[[nodiscard]] Digest digest_of(const PolicyDocument& value);
[[nodiscard]] Digest digest_of(const FacilitySnapshot& value);
[[nodiscard]] Digest dependency_digest_of(const FacilitySnapshot& value);
[[nodiscard]] Digest digest_of(const Obligation& value);
[[nodiscard]] Digest digest_of(const ObligationSet& value);
[[nodiscard]] Digest digest_of(const Receipt& value);
[[nodiscard]] Digest digest_of(const ObligationStatusReport& value);
[[nodiscard]] Digest digest_of(const PreconditionReport& value);
[[nodiscard]] Digest digest_of(const ApprovalRecord& value);
[[nodiscard]] Digest digest_of(const ExceptionGrant& value);
[[nodiscard]] Digest digest_of(const MaintenancePlan& value);
[[nodiscard]] Digest digest_of(const PhaseTransition& value);
[[nodiscard]] Digest digest_of(const ProgressEvent& value);
[[nodiscard]] Digest digest_of(const PlanRecord& value);
[[nodiscard]] Digest digest_of(const RestorationReport& value);
[[nodiscard]] Digest digest_of(const CompletionReport& value);
[[nodiscard]] Digest digest_of(const RecoveryReport& value);
[[nodiscard]] Digest digest_of(const Explanation& value);
[[nodiscard]] Digest digest_of(const LedgerState& value);

// Canonical digest of an arbitrary set of digests, order independent.
[[nodiscard]] Digest digest_of_digests(std::string_view purpose, std::vector<Digest> digests);

// ---------------------------------------------------------------------------
// Record payloads: the durable format for one journal mutation
// ---------------------------------------------------------------------------

enum class PayloadKind : std::uint8_t {
    FacilityInstalled = 1,
    PlanStored = 2,
    Commit = 3,
    SnapshotPublished = 4,
};

struct CommitPayload {
    CommitSequence sequence;
    Digest transaction_digest;  // SHA-256 over the mutation payload bytes
    std::uint32_t mutation_bytes{0};
};

void encode(Writer& writer, const CommitPayload& value);
[[nodiscard]] Status decode(Reader& reader, CommitPayload& value);

}  // namespace mc::detail

#endif  // MC_INTERNAL_SERIALIZE_HPP

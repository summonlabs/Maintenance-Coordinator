// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Identities, generations and digests.
//
// Every identity, generation, epoch and counter in this repository is a
// distinct strong type.  Mixing a LifecycleGeneration with a HardwareGeneration,
// or an AssetId with a RackId, must not compile.  Generations carry an explicit
// "unset" state: an unknown generation is never silently treated as generation
// zero, and no default-constructed generation compares equal to a real one.

#ifndef MC_IDENT_HPP
#define MC_IDENT_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "mc/status.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// Lexical identities.
//
// These are opaque names issued by adjacent authorities (DCCP inventory, ASI,
// DFI) or by operators.  Validation is deliberately strict and ASCII-only so
// that two different spellings can never be confused for one another: no
// Unicode normalisation, no case folding, no trimming.  A name is either
// exactly right or rejected.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxIdentifierLength = 128;

// Rejects empty names, names longer than kMaxIdentifierLength, leading or
// trailing whitespace, and any byte outside [A-Za-z0-9._:@/+-].
[[nodiscard]] bool is_valid_identifier(std::string_view name) noexcept;

// Throws nothing; returns a Status instead so callers keep the error model.
[[nodiscard]] Status validate_identifier(std::string_view name, std::string_view what);

template <typename Tag>
class Ident {
public:
    using tag_type = Tag;

    Ident() = default;

    // Strict parse: the name must be a valid identifier.  Returns
    // ErrorCode::InvalidArgument otherwise, naming `what` in the status.
    [[nodiscard]] static Result<Ident> parse(std::string_view name, std::string_view what = Tag::name) {
        if (auto status = validate_identifier(name, what); !status.ok()) {
            return status;
        }
        return Ident(std::string(name));
    }

    [[nodiscard]] bool empty() const noexcept { return name_.empty(); }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::string_view view() const noexcept { return name_; }

    [[nodiscard]] friend bool operator==(const Ident&, const Ident&) noexcept = default;
    [[nodiscard]] friend std::strong_ordering operator<=>(const Ident& lhs, const Ident& rhs) noexcept {
        return lhs.name_ <=> rhs.name_;
    }

private:
    explicit Ident(std::string name) : name_(std::move(name)) {}

    std::string name_;
};

// Identity tags.  Each also carries the human facing label used in status
// messages so that errors name the same noun the CLI accepts.
#define MC_DEFINE_IDENT_TAG(TypeName, LabelText)     \
    struct TypeName##Tag {                            \
        static constexpr std::string_view name = LabelText; \
    };                                                \
    using TypeName = Ident<TypeName##Tag>

MC_DEFINE_IDENT_TAG(SiteId, "site");
MC_DEFINE_IDENT_TAG(RackId, "rack");
MC_DEFINE_IDENT_TAG(AssetId, "asset");
MC_DEFINE_IDENT_TAG(PowerDomainId, "power-domain");
MC_DEFINE_IDENT_TAG(CoolingZoneId, "cooling-zone");
MC_DEFINE_IDENT_TAG(FabricSegmentId, "fabric-segment");
MC_DEFINE_IDENT_TAG(RedundancyGroupId, "redundancy-group");
MC_DEFINE_IDENT_TAG(CapacityPoolId, "capacity-pool");
MC_DEFINE_IDENT_TAG(ProtectionId, "protected-obligation");
MC_DEFINE_IDENT_TAG(IncidentId, "incident");
MC_DEFINE_IDENT_TAG(BlackoutId, "blackout");
MC_DEFINE_IDENT_TAG(PlanId, "plan");
MC_DEFINE_IDENT_TAG(WindowId, "window");
MC_DEFINE_IDENT_TAG(ObligationId, "obligation");
MC_DEFINE_IDENT_TAG(ReceiptId, "receipt");
MC_DEFINE_IDENT_TAG(ApprovalId, "approval");
MC_DEFINE_IDENT_TAG(ExceptionId, "exception");
MC_DEFINE_IDENT_TAG(AttemptId, "attempt");
MC_DEFINE_IDENT_TAG(OperatorId, "operator");
MC_DEFINE_IDENT_TAG(PolicyId, "policy");
MC_DEFINE_IDENT_TAG(EvidenceId, "evidence");

#undef MC_DEFINE_IDENT_TAG

// ---------------------------------------------------------------------------
// Digests.
//
// A digest is the 32-byte output of SHA-256 over a purpose tag followed by the
// canonical encoding of the object being bound (see digest.hpp).  Digests bind
// plans, approvals and receipts to the exact policy revision, dependency
// snapshot, obligation set and evidence they were produced against.  A digest
// is an integrity and identity binding, never a signature: nothing in this
// repository treats digest secrecy as an authority decision.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kDigestBytes = 32;

class Digest {
public:
    Digest() = default;

    [[nodiscard]] static Digest from_bytes(const std::array<std::uint8_t, kDigestBytes>& bytes) noexcept {
        Digest d;
        d.bytes_ = bytes;
        return d;
    }

    // Parses exactly 64 lowercase or uppercase hex characters.
    [[nodiscard]] static bool parse(std::string_view hex, Digest& out) noexcept;

    [[nodiscard]] const std::array<std::uint8_t, kDigestBytes>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::string hex() const;
    [[nodiscard]] bool is_zero() const noexcept;

    [[nodiscard]] friend bool operator==(const Digest& a, const Digest& b) noexcept {
        return a.bytes_ == b.bytes_;
    }
    [[nodiscard]] friend bool operator!=(const Digest& a, const Digest& b) noexcept { return !(a == b); }
    [[nodiscard]] friend bool operator<(const Digest& a, const Digest& b) noexcept {
        return a.bytes_ < b.bytes_;
    }

private:
    std::array<std::uint8_t, kDigestBytes> bytes_{};
};

// ---------------------------------------------------------------------------
// Generations, epochs and monotonic counters.
// ---------------------------------------------------------------------------

template <typename Tag>
class Generation {
public:
    using value_type = std::uint64_t;

    constexpr Generation() noexcept = default;
    constexpr explicit Generation(value_type value) noexcept : value_(value), set_(true) {}

    [[nodiscard]] static constexpr Generation from_value(value_type value) noexcept {
        return Generation(value);
    }

    [[nodiscard]] constexpr bool is_set() const noexcept { return set_; }
    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }

    // Returns the value only when it is set.  Callers that must not invent a
    // generation use this and propagate MissingGeneration instead.
    [[nodiscard]] bool try_value(value_type& out) const noexcept {
        if (!set_) {
            return false;
        }
        out = value_;
        return true;
    }

    [[nodiscard]] constexpr Generation next() const noexcept {
        return Generation(value_ + 1U);
    }

    [[nodiscard]] friend constexpr bool operator==(Generation a, Generation b) noexcept {
        return a.set_ == b.set_ && a.value_ == b.value_;
    }
    [[nodiscard]] friend constexpr bool operator!=(Generation a, Generation b) noexcept { return !(a == b); }
    [[nodiscard]] friend constexpr bool operator<(Generation a, Generation b) noexcept {
        if (a.set_ != b.set_) {
            return !a.set_;
        }
        return a.value_ < b.value_;
    }

private:
    value_type value_{0};
    bool set_{false};
};

struct LifecycleGenerationTag {};
struct HardwareGenerationTag {};
struct FirmwareGenerationTag {};
struct ControlEpochTag {};
struct IncarnationTag {};
struct PolicyGenerationTag {};
struct DependencyGenerationTag {};
struct CapacityGenerationTag {};
struct TopologyGenerationTag {};
struct MaintenanceGenerationTag {};
struct FacilityEpochTag {};
struct RevisionTag {};
struct CommitSequenceTag {};
struct ObservationSequenceTag {};
struct PlanCounterTag {};

using LifecycleGeneration = Generation<LifecycleGenerationTag>;
using HardwareGeneration = Generation<HardwareGenerationTag>;
using FirmwareGeneration = Generation<FirmwareGenerationTag>;
using ControlEpoch = Generation<ControlEpochTag>;
using IncarnationId = Generation<IncarnationTag>;
using PolicyGeneration = Generation<PolicyGenerationTag>;
using DependencyGeneration = Generation<DependencyGenerationTag>;
using CapacityGeneration = Generation<CapacityGenerationTag>;
using TopologyGeneration = Generation<TopologyGenerationTag>;
using MaintenanceGeneration = Generation<MaintenanceGenerationTag>;
using FacilityEpoch = Generation<FacilityEpochTag>;
using Revision = Generation<RevisionTag>;
using CommitSequence = Generation<CommitSequenceTag>;
using ObservationSequence = Generation<ObservationSequenceTag>;

// ---------------------------------------------------------------------------
// Timestamps: nanoseconds since the Unix epoch, always explicitly set.
// ---------------------------------------------------------------------------

using Timestamp = std::int64_t;

// The unset sentinel is the smallest representable instant, not zero: zero is
// the Unix epoch and a real instant, so it must be representable and must be
// distinguishable from "no timestamp at all".
inline constexpr Timestamp kNoTimestamp = (std::numeric_limits<std::int64_t>::min)();

[[nodiscard]] constexpr bool is_set(Timestamp value) noexcept { return value != kNoTimestamp; }

}  // namespace mc

namespace std {

template <typename Tag>
struct hash<mc::Ident<Tag>> {
    [[nodiscard]] std::size_t operator()(const mc::Ident<Tag>& value) const noexcept {
        return std::hash<std::string>{}(value.name());
    }
};

template <>
struct hash<mc::Digest> {
    [[nodiscard]] std::size_t operator()(const mc::Digest& value) const noexcept {
        std::size_t seed = 1469598103934665603ULL;
        for (const auto byte : value.bytes()) {
            seed ^= static_cast<std::size_t>(byte);
            seed *= 1099511628211ULL;
        }
        return seed;
    }
};

template <typename Tag>
struct hash<mc::Generation<Tag>> {
    [[nodiscard]] std::size_t operator()(const mc::Generation<Tag>& value) const noexcept {
        std::size_t seed = std::hash<bool>{}(value.is_set());
        seed ^= std::hash<std::uint64_t>{}(value.value()) + 0x9E3779B97F4A7C15ULL + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

}  // namespace std

#endif  // MC_IDENT_HPP

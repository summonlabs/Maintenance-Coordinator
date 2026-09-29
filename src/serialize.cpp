// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.

#include "serialize.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mc/version.hpp"

namespace mc::detail {
namespace {

constexpr std::uint8_t kMaxTargetKind = 5;
constexpr std::uint8_t kMaxLifecycleState = 8;
constexpr std::uint8_t kMaxSeverity = 3;
constexpr std::uint8_t kMaxServiceClass = 3;
constexpr std::uint8_t kMaxRedundancyMode = 4;
constexpr std::uint8_t kMaxActivity = 9;
constexpr std::uint8_t kMaxPriority = 2;
constexpr std::uint8_t kMaxRisk = 3;
constexpr std::uint8_t kMaxPlanPhase = 11;
constexpr std::uint8_t kMaxProgressKind = 4;
constexpr std::uint8_t kMaxObligationKind = 15;
constexpr std::uint8_t kMaxObligationAuthority = 4;
constexpr std::uint8_t kMaxObligationStage = 2;
constexpr std::uint8_t kMaxDisposition = 5;
constexpr std::uint8_t kMaxReceiptKind = 18;

template <typename Enum>
void encode_enum(Writer& writer, Enum value) {
    writer.u8(static_cast<std::uint8_t>(value));
}

template <typename Enum>
[[nodiscard]] Status decode_enum(Reader& reader, Enum& value, std::uint8_t max_value, std::string_view what) {
    std::uint8_t raw = 0;
    if (auto status = reader.u8(raw); !status.ok()) {
        return status;
    }
    if (raw > max_value) {
        return fail(ErrorCode::ReservedFieldViolation, "enum field holds a value outside its defined range",
                    std::string(what), std::to_string(raw));
    }
    value = static_cast<Enum>(raw);
    return Status::success();
}

template <typename Tag>
void encode_ident(Writer& writer, const Ident<Tag>& value) {
    writer.ident(value);
}

template <typename Tag>
[[nodiscard]] Status decode_ident(Reader& reader, Ident<Tag>& value) {
    return reader.ident(value);
}

// Identities that are legitimately absent carry an explicit presence flag: an
// empty identity is never encoded as an empty name, because "no identity" and
// "an identity whose name is empty" must not be the same byte string.
template <typename Tag>
void encode_maybe_ident(Writer& writer, const Ident<Tag>& value) {
    writer.boolean(!value.empty());
    if (!value.empty()) {
        writer.ident(value);
    }
}

template <typename Tag>
[[nodiscard]] Status decode_maybe_ident(Reader& reader, Ident<Tag>& value) {
    bool present = false;
    if (auto status = reader.boolean(present); !status.ok()) {
        return status;
    }
    if (!present) {
        value = Ident<Tag>{};
        return Status::success();
    }
    return reader.ident(value);
}

template <typename Tag>
void encode_generation(Writer& writer, const Generation<Tag>& value) {
    writer.generation(value);
}

template <typename Tag>
[[nodiscard]] Status decode_generation(Reader& reader, Generation<Tag>& value) {
    return reader.generation(value);
}

template <typename T, typename EncodeFn>
void encode_list(Writer& writer, const std::vector<T>& items, EncodeFn encode_item) {
    writer.u32(static_cast<std::uint32_t>(items.size()));
    for (const auto& item : items) {
        encode_item(writer, item);
    }
}

template <typename T, typename DecodeFn>
[[nodiscard]] Status decode_list(Reader& reader, std::vector<T>& items, std::string_view what, DecodeFn decode_item) {
    std::uint32_t count = 0;
    if (auto status = reader.u32(count); !status.ok()) {
        return status;
    }
    if (count > kMaxCollectionItems) {
        return fail(ErrorCode::LimitExceeded, "collection exceeds the accepted element count", std::string(what),
                    std::to_string(count));
    }
    items.clear();
    items.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        T item{};
        if (auto status = decode_item(reader, item); !status.ok()) {
            return status;
        }
        items.push_back(std::move(item));
    }
    return Status::success();
}

template <typename T, typename EncodeFn>
void encode_optional(Writer& writer, const std::optional<T>& value, EncodeFn encode_item) {
    writer.boolean(value.has_value());
    if (value.has_value()) {
        encode_item(writer, *value);
    }
}

template <typename T, typename DecodeFn>
[[nodiscard]] Status decode_optional(Reader& reader, std::optional<T>& value, DecodeFn decode_item) {
    bool present = false;
    if (auto status = reader.boolean(present); !status.ok()) {
        return status;
    }
    if (!present) {
        value.reset();
        return Status::success();
    }
    T item{};
    if (auto status = decode_item(reader, item); !status.ok()) {
        return status;
    }
    value = std::move(item);
    return Status::success();
}

}  // namespace

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------

void encode(Writer& writer, const TargetRef& value) {
    encode_enum(writer, value.kind());
    writer.blob(value.name());
}

Status decode(Reader& reader, TargetRef& value) {
    TargetKind kind{TargetKind::Site};
    if (auto status = decode_enum(reader, kind, kMaxTargetKind, "TargetRef.kind"); !status.ok()) {
        return status;
    }
    std::string name;
    if (auto status = reader.blob(name); !status.ok()) {
        return status;
    }
    auto made = TargetRef::make(kind, name);
    if (!made.ok()) {
        return made.status();
    }
    value = std::move(made).value();
    return Status::success();
}

void encode(Writer& writer, const MaintenanceScope& value) {
    encode_list(writer, value.targets(),
                [](Writer& out, const TargetRef& item) { encode(out, item); });
}

Status decode(Reader& reader, MaintenanceScope& value) {
    std::vector<TargetRef> targets;
    if (auto status = decode_list(reader, targets, "MaintenanceScope.targets",
                                  [](Reader& in, TargetRef& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    value = MaintenanceScope(std::move(targets));
    value.canonicalize();
    return Status::success();
}

void encode(Writer& writer, const WindowSpec& value) {
    writer.i64(value.start);
    writer.i64(value.end);
    writer.boolean(value.flexible);
}

Status decode(Reader& reader, WindowSpec& value) {
    WindowSpec parsed;
    if (auto status = reader.i64(parsed.start); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.end); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.flexible); !status.ok()) {
        return status;
    }
    value = parsed;
    return Status::success();
}

void encode(Writer& writer, const Headroom& value) {
    writer.boolean(value.measured);
    writer.i64(value.available_units);
    writer.i64(value.required_units);
}

Status decode(Reader& reader, Headroom& value) {
    Headroom parsed;
    if (auto status = reader.boolean(parsed.measured); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.available_units); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.required_units); !status.ok()) {
        return status;
    }
    value = parsed;
    return Status::success();
}

// ---------------------------------------------------------------------------
// Facility
// ---------------------------------------------------------------------------

void encode(Writer& writer, const PolicyDocument& value) {
    encode_ident(writer, value.id);
    encode_generation(writer, value.revision);
    encode_generation(writer, value.generation);
    writer.u32(value.min_redundancy_margin_units);
    writer.u32(value.min_spare_capacity_units);
    writer.i64(value.min_power_headroom_milliwatts);
    writer.i64(value.min_cooling_headroom_units);
    writer.i64(value.approval_validity_nanos);
    writer.i64(value.max_window_duration_nanos);
    writer.u32(value.max_concurrent_windows_per_rack);
    writer.boolean(value.require_personnel_evidence);
    writer.boolean(value.require_dual_approval);
    writer.boolean(value.require_work_stop_evidence);
    writer.boolean(value.require_restoration_evidence);
    writer.blob(value.service_class_ceiling_note);
}

Status decode(Reader& reader, PolicyDocument& value) {
    PolicyDocument parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.revision); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.generation); !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.min_redundancy_margin_units); !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.min_spare_capacity_units); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.min_power_headroom_milliwatts); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.min_cooling_headroom_units); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.approval_validity_nanos); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.max_window_duration_nanos); !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.max_concurrent_windows_per_rack); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.require_personnel_evidence); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.require_dual_approval); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.require_work_stop_evidence); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.require_restoration_evidence); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.service_class_ceiling_note); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const AssetRecord& value) {
    encode_ident(writer, value.id);
    encode_ident(writer, value.rack);
    encode_ident(writer, value.site);
    encode_enum(writer, value.lifecycle);
    encode_enum(writer, value.service_class);
    encode_generation(writer, value.lifecycle_generation);
    encode_generation(writer, value.hardware_generation);
    encode_generation(writer, value.firmware_generation);
    writer.u32(value.capacity_units);
    writer.boolean(value.isolatable);
    writer.boolean(value.drainable);
    writer.boolean(value.healthy);
}

Status decode(Reader& reader, AssetRecord& value) {
    AssetRecord parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_ident(reader, parsed.rack); !status.ok()) {
        return status;
    }
    if (auto status = decode_ident(reader, parsed.site); !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.lifecycle, kMaxLifecycleState, "AssetRecord.lifecycle");
        !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.service_class, kMaxServiceClass, "AssetRecord.service_class");
        !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.lifecycle_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.hardware_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.firmware_generation); !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.capacity_units); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.isolatable); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.drainable); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.healthy); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const RedundancyGroup& value) {
    encode_ident(writer, value.id);
    encode_ident(writer, value.site);
    encode_enum(writer, value.mode);
    writer.u32(value.required_units);
    writer.u32(value.observed_available_units);
    encode_list(writer, value.members, [](Writer& out, const AssetId& item) { encode_ident(out, item); });
}

Status decode(Reader& reader, RedundancyGroup& value) {
    RedundancyGroup parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_ident(reader, parsed.site); !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.mode, kMaxRedundancyMode, "RedundancyGroup.mode"); !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.required_units); !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.observed_available_units); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.members, "RedundancyGroup.members",
                                  [](Reader& in, AssetId& item) { return decode_ident(in, item); });
        !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const CapacityPool& value) {
    encode_ident(writer, value.id);
    encode_ident(writer, value.site);
    writer.u32(value.units_total);
    writer.u32(value.units_available);
    writer.u32(value.units_protected);
}

Status decode(Reader& reader, CapacityPool& value) {
    CapacityPool parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_ident(reader, parsed.site); !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.units_total); !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.units_available); !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.units_protected); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const PowerDomain& value) {
    encode_ident(writer, value.id);
    encode_ident(writer, value.site);
    encode(writer, value.headroom);
    writer.boolean(value.interlocked);
    writer.blob(value.interlock_reason);
}

Status decode(Reader& reader, PowerDomain& value) {
    PowerDomain parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_ident(reader, parsed.site); !status.ok()) {
        return status;
    }
    if (auto status = decode(reader, parsed.headroom); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.interlocked); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.interlock_reason); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const CoolingZone& value) {
    encode_ident(writer, value.id);
    encode_ident(writer, value.site);
    encode(writer, value.headroom);
    writer.boolean(value.interlocked);
    writer.blob(value.interlock_reason);
}

Status decode(Reader& reader, CoolingZone& value) {
    CoolingZone parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_ident(reader, parsed.site); !status.ok()) {
        return status;
    }
    if (auto status = decode(reader, parsed.headroom); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.interlocked); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.interlock_reason); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const FabricSegment& value) {
    encode_ident(writer, value.id);
    encode_ident(writer, value.site);
    writer.u32(value.available_paths);
    writer.u32(value.required_paths);
    writer.boolean(value.drainable);
}

Status decode(Reader& reader, FabricSegment& value) {
    FabricSegment parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_ident(reader, parsed.site); !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.available_paths); !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.required_paths); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.drainable); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const BlackoutPeriod& value) {
    encode_ident(writer, value.id);
    encode_list(writer, value.targets, [](Writer& out, const TargetRef& item) { encode(out, item); });
    writer.i64(value.start);
    writer.i64(value.end);
    writer.boolean(value.hard);
    writer.blob(value.reason);
}

Status decode(Reader& reader, BlackoutPeriod& value) {
    BlackoutPeriod parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.targets, "BlackoutPeriod.targets",
                                  [](Reader& in, TargetRef& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.start); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.end); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.hard); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.reason); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const IncidentRecord& value) {
    encode_ident(writer, value.id);
    encode_list(writer, value.targets, [](Writer& out, const TargetRef& item) { encode(out, item); });
    encode_enum(writer, value.severity);
    writer.boolean(value.active);
    writer.boolean(value.hard_block);
    writer.blob(value.summary);
}

Status decode(Reader& reader, IncidentRecord& value) {
    IncidentRecord parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.targets, "IncidentRecord.targets",
                                  [](Reader& in, TargetRef& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.severity, kMaxSeverity, "IncidentRecord.severity"); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.active); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.hard_block); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.summary); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const ProtectedObligation& value) {
    encode_ident(writer, value.id);
    encode_list(writer, value.targets, [](Writer& out, const TargetRef& item) { encode(out, item); });
    encode_enum(writer, value.required_mode);
    writer.u32(value.required_units);
    writer.u32(value.tolerance_units);
    writer.boolean(value.hard_interlock);
    writer.blob(value.description);
}

Status decode(Reader& reader, ProtectedObligation& value) {
    ProtectedObligation parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.targets, "ProtectedObligation.targets",
                                  [](Reader& in, TargetRef& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.required_mode, kMaxRedundancyMode, "ProtectedObligation.mode");
        !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.required_units); !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.tolerance_units); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.hard_interlock); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.description); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const FacilitySnapshot& value) {
    encode_generation(writer, value.revision);
    encode_generation(writer, value.facility_epoch);
    encode_generation(writer, value.policy_generation);
    encode_generation(writer, value.dependency_generation);
    encode_generation(writer, value.capacity_generation);
    encode_generation(writer, value.topology_generation);
    encode_generation(writer, value.maintenance_generation);
    encode_generation(writer, value.control_epoch);
    writer.i64(value.observed_at);
    encode(writer, value.policy);
    encode_list(writer, value.assets, [](Writer& out, const AssetRecord& item) { encode(out, item); });
    encode_list(writer, value.redundancy_groups,
                [](Writer& out, const RedundancyGroup& item) { encode(out, item); });
    encode_list(writer, value.capacity_pools, [](Writer& out, const CapacityPool& item) { encode(out, item); });
    encode_list(writer, value.power_domains, [](Writer& out, const PowerDomain& item) { encode(out, item); });
    encode_list(writer, value.cooling_zones, [](Writer& out, const CoolingZone& item) { encode(out, item); });
    encode_list(writer, value.fabric_segments, [](Writer& out, const FabricSegment& item) { encode(out, item); });
    encode_list(writer, value.blackouts, [](Writer& out, const BlackoutPeriod& item) { encode(out, item); });
    encode_list(writer, value.incidents, [](Writer& out, const IncidentRecord& item) { encode(out, item); });
    encode_list(writer, value.protected_obligations,
                [](Writer& out, const ProtectedObligation& item) { encode(out, item); });
}

Status decode(Reader& reader, FacilitySnapshot& value) {
    FacilitySnapshot parsed;
    if (auto status = decode_generation(reader, parsed.revision); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.facility_epoch); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.policy_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.dependency_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.capacity_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.topology_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.maintenance_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.control_epoch); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.observed_at); !status.ok()) {
        return status;
    }
    if (auto status = decode(reader, parsed.policy); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.assets, "FacilitySnapshot.assets",
                                  [](Reader& in, AssetRecord& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.redundancy_groups, "FacilitySnapshot.redundancy_groups",
                                  [](Reader& in, RedundancyGroup& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.capacity_pools, "FacilitySnapshot.capacity_pools",
                                  [](Reader& in, CapacityPool& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.power_domains, "FacilitySnapshot.power_domains",
                                  [](Reader& in, PowerDomain& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.cooling_zones, "FacilitySnapshot.cooling_zones",
                                  [](Reader& in, CoolingZone& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.fabric_segments, "FacilitySnapshot.fabric_segments",
                                  [](Reader& in, FabricSegment& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.blackouts, "FacilitySnapshot.blackouts",
                                  [](Reader& in, BlackoutPeriod& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.incidents, "FacilitySnapshot.incidents",
                                  [](Reader& in, IncidentRecord& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.protected_obligations, "FacilitySnapshot.protected_obligations",
                                  [](Reader& in, ProtectedObligation& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    parsed.finalize();
    value = std::move(parsed);
    return Status::success();
}

// ---------------------------------------------------------------------------
// Obligations and receipts
// ---------------------------------------------------------------------------

void encode(Writer& writer, const Obligation& value) {
    encode_ident(writer, value.id);
    encode_enum(writer, value.kind);
    encode_enum(writer, value.authority);
    encode(writer, value.target);
    writer.blob(value.requirement);
    writer.boolean(value.mandatory);
    writer.boolean(value.requires_observation);
    writer.u16(static_cast<std::uint16_t>(value.guarded_by));
}

Status decode(Reader& reader, Obligation& value) {
    Obligation parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.kind, kMaxObligationKind, "Obligation.kind"); !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.authority, kMaxObligationAuthority, "Obligation.authority");
        !status.ok()) {
        return status;
    }
    if (auto status = decode(reader, parsed.target); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.requirement); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.mandatory); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.requires_observation); !status.ok()) {
        return status;
    }
    std::uint16_t guarded = 0;
    if (auto status = reader.u16(guarded); !status.ok()) {
        return status;
    }
    parsed.guarded_by = static_cast<ErrorCode>(guarded);
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const ObligationSet& value) {
    encode_list(writer, value.obligations, [](Writer& out, const Obligation& item) { encode(out, item); });
}

Status decode(Reader& reader, ObligationSet& value) {
    ObligationSet parsed;
    if (auto status = decode_list(reader, parsed.obligations, "ObligationSet.obligations",
                                  [](Reader& in, Obligation& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    parsed.finalize();
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const Receipt& value) {
    encode_ident(writer, value.id);
    encode_ident(writer, value.obligation);
    encode_enum(writer, value.kind);
    encode_enum(writer, value.issuer_authority);
    writer.blob(value.issuer);
    encode_generation(writer, value.observation_sequence);
    encode_generation(writer, value.plan_revision);
    writer.digest(value.obligation_digest);
    writer.digest(value.evidence_digest);
    writer.blob(value.evidence);
    writer.i64(value.observed_at);
    writer.i64(value.ingested_at);
}

Status decode(Reader& reader, Receipt& value) {
    Receipt parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_ident(reader, parsed.obligation); !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.kind, kMaxReceiptKind, "Receipt.kind"); !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.issuer_authority, kMaxObligationAuthority, "Receipt.authority");
        !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.issuer); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.observation_sequence); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.plan_revision); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.obligation_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.evidence_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.evidence); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.observed_at); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.ingested_at); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const ObligationStatus& value) {
    encode_ident(writer, value.id);
    encode_enum(writer, value.kind);
    encode_enum(writer, value.authority);
    encode_enum(writer, value.stage);
    encode_enum(writer, value.disposition);
    writer.boolean(value.mandatory);
    encode(writer, value.target);
    writer.blob(value.detail);
    encode_list(writer, value.supporting_receipts,
                [](Writer& out, const ReceiptId& item) { encode_ident(out, item); });
    encode_maybe_ident(writer, value.waiver);
}

Status decode(Reader& reader, ObligationStatus& value) {
    ObligationStatus parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.kind, kMaxObligationKind, "ObligationStatus.kind"); !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.authority, kMaxObligationAuthority, "ObligationStatus.authority");
        !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.stage, kMaxObligationStage, "ObligationStatus.stage");
        !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.disposition, kMaxDisposition, "ObligationStatus.disposition");
        !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.mandatory); !status.ok()) {
        return status;
    }
    if (auto status = decode(reader, parsed.target); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.detail); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.supporting_receipts, "ObligationStatus.supporting_receipts",
                                  [](Reader& in, ReceiptId& item) { return decode_ident(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_maybe_ident(reader, parsed.waiver); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const ObligationStatusReport& value) {
    encode_generation(writer, value.plan_revision);
    writer.digest(value.obligation_set_digest);
    encode_list(writer, value.statuses, [](Writer& out, const ObligationStatus& item) { encode(out, item); });
    writer.boolean(value.all_satisfied);
    writer.boolean(value.ready_for_pre_drain);
    writer.boolean(value.ready_for_isolation);
    writer.boolean(value.ready_for_completion);
}

Status decode(Reader& reader, ObligationStatusReport& value) {
    ObligationStatusReport parsed;
    if (auto status = decode_generation(reader, parsed.plan_revision); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.obligation_set_digest); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.statuses, "ObligationStatusReport.statuses",
                                  [](Reader& in, ObligationStatus& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.all_satisfied); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.ready_for_pre_drain); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.ready_for_isolation); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.ready_for_completion); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

// ---------------------------------------------------------------------------
// Conditions, evaluation, approval, exceptions
// ---------------------------------------------------------------------------

void encode(Writer& writer, const ConditionResult& value) {
    writer.u16(static_cast<std::uint16_t>(value.condition));
    writer.boolean(value.satisfied);
    writer.boolean(value.hard);
    writer.boolean(value.waivable);
    writer.boolean(value.measured);
    encode_maybe_ident(writer, value.waiver);
    writer.blob(value.subject);
    writer.blob(value.detail);
    writer.i64(value.observed);
    writer.i64(value.required);
}

Status decode(Reader& reader, ConditionResult& value) {
    ConditionResult parsed;
    std::uint16_t condition = 0;
    if (auto status = reader.u16(condition); !status.ok()) {
        return status;
    }
    parsed.condition = static_cast<ErrorCode>(condition);
    if (auto status = reader.boolean(parsed.satisfied); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.hard); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.waivable); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.measured); !status.ok()) {
        return status;
    }
    if (auto status = decode_maybe_ident(reader, parsed.waiver); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.subject); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.detail); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.observed); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.required); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const PreconditionReport& value) {
    encode_generation(writer, value.plan_revision);
    writer.digest(value.plan_digest);
    writer.i64(value.evaluated_at);
    encode_generation(writer, value.facility_epoch);
    encode_generation(writer, value.control_epoch);
    encode_generation(writer, value.policy_generation);
    encode_generation(writer, value.dependency_generation);
    encode_generation(writer, value.capacity_generation);
    encode_generation(writer, value.topology_generation);
    encode_generation(writer, value.maintenance_generation);
    writer.digest(value.facility_digest);
    writer.digest(value.policy_digest);
    writer.digest(value.dependency_digest);
    encode_list(writer, value.conditions, [](Writer& out, const ConditionResult& item) { encode(out, item); });
    encode_list(writer, value.applied_exceptions,
                [](Writer& out, const ExceptionId& item) { encode_ident(out, item); });
    writer.boolean(value.satisfied);
    writer.boolean(value.satisfied_without_exceptions);
}

Status decode(Reader& reader, PreconditionReport& value) {
    PreconditionReport parsed;
    if (auto status = decode_generation(reader, parsed.plan_revision); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.plan_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.evaluated_at); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.facility_epoch); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.control_epoch); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.policy_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.dependency_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.capacity_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.topology_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.maintenance_generation); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.facility_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.policy_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.dependency_digest); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.conditions, "PreconditionReport.conditions",
                                  [](Reader& in, ConditionResult& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.applied_exceptions, "PreconditionReport.applied_exceptions",
                                  [](Reader& in, ExceptionId& item) { return decode_ident(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.satisfied); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.satisfied_without_exceptions); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const ApprovalRecord& value) {
    encode_ident(writer, value.id);
    encode_ident(writer, value.plan);
    encode_generation(writer, value.plan_revision);
    writer.blob(value.approved_by);
    writer.blob(value.approval_evidence);
    writer.blob(value.witness);
    writer.i64(value.approved_at);
    writer.i64(value.expires_at);
    writer.digest(value.plan_digest);
    writer.digest(value.facility_digest);
    writer.digest(value.policy_digest);
    writer.digest(value.dependency_digest);
    writer.digest(value.evaluation_digest);
    writer.digest(value.obligation_set_digest);
    encode_generation(writer, value.facility_epoch);
    encode_generation(writer, value.control_epoch);
    encode_generation(writer, value.policy_generation);
    encode_generation(writer, value.dependency_generation);
    encode_generation(writer, value.capacity_generation);
    encode_generation(writer, value.topology_generation);
    encode_generation(writer, value.maintenance_generation);
    encode_list(writer, value.exceptions, [](Writer& out, const ExceptionId& item) { encode_ident(out, item); });
}

Status decode(Reader& reader, ApprovalRecord& value) {
    ApprovalRecord parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_ident(reader, parsed.plan); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.plan_revision); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.approved_by); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.approval_evidence); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.witness); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.approved_at); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.expires_at); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.plan_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.facility_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.policy_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.dependency_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.evaluation_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.obligation_set_digest); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.facility_epoch); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.control_epoch); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.policy_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.dependency_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.capacity_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.topology_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.maintenance_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.exceptions, "ApprovalRecord.exceptions",
                                  [](Reader& in, ExceptionId& item) { return decode_ident(in, item); });
        !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const ExceptionGrant& value) {
    encode_ident(writer, value.id);
    encode_ident(writer, value.plan);
    encode_list(writer, value.waived,
                [](Writer& out, ErrorCode item) { out.u16(static_cast<std::uint16_t>(item)); });
    encode_list(writer, value.targets, [](Writer& out, const TargetRef& item) { encode(out, item); });
    writer.blob(value.granted_by);
    writer.blob(value.justification);
    writer.i64(value.granted_at);
    writer.i64(value.expires_at);
}

Status decode(Reader& reader, ExceptionGrant& value) {
    ExceptionGrant parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_ident(reader, parsed.plan); !status.ok()) {
        return status;
    }
    std::vector<std::uint16_t> waived;
    if (auto status = decode_list(reader, waived, "ExceptionGrant.waived",
                                  [](Reader& in, std::uint16_t& item) { return in.u16(item); });
        !status.ok()) {
        return status;
    }
    parsed.waived.reserve(waived.size());
    for (const auto code : waived) {
        parsed.waived.push_back(static_cast<ErrorCode>(code));
    }
    if (auto status = decode_list(reader, parsed.targets, "ExceptionGrant.targets",
                                  [](Reader& in, TargetRef& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.granted_by); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.justification); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.granted_at); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.expires_at); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

// ---------------------------------------------------------------------------
// Plan
// ---------------------------------------------------------------------------

void encode(Writer& writer, const MaintenancePlan& value) {
    encode_ident(writer, value.id);
    encode_generation(writer, value.revision);
    writer.blob(value.reason);
    writer.blob(value.requested_by);
    encode_enum(writer, value.activity);
    encode_enum(writer, value.priority);
    encode_enum(writer, value.risk);
    encode(writer, value.scope);
    encode(writer, value.window);
    encode_generation(writer, value.facility_epoch);
    encode_generation(writer, value.control_epoch);
    encode_generation(writer, value.policy_generation);
    encode_generation(writer, value.dependency_generation);
    encode_generation(writer, value.capacity_generation);
    encode_generation(writer, value.topology_generation);
    encode_generation(writer, value.maintenance_generation);
    writer.digest(value.policy_digest);
    writer.digest(value.dependency_digest);
}

Status decode(Reader& reader, MaintenancePlan& value) {
    MaintenancePlan parsed;
    if (auto status = decode_ident(reader, parsed.id); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.revision); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.reason); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.requested_by); !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.activity, kMaxActivity, "MaintenancePlan.activity");
        !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.priority, kMaxPriority, "MaintenancePlan.priority");
        !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.risk, kMaxRisk, "MaintenancePlan.risk"); !status.ok()) {
        return status;
    }
    if (auto status = decode(reader, parsed.scope); !status.ok()) {
        return status;
    }
    if (auto status = decode(reader, parsed.window); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.facility_epoch); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.control_epoch); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.policy_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.dependency_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.capacity_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.topology_generation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.maintenance_generation); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.policy_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.dependency_digest); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const PhaseTransition& value) {
    encode_enum(writer, value.from);
    encode_enum(writer, value.to);
    writer.i64(value.at);
    writer.blob(value.actor);
    writer.blob(value.reason);
    writer.u16(static_cast<std::uint16_t>(value.cause));
    encode_generation(writer, value.sequence);
}

Status decode(Reader& reader, PhaseTransition& value) {
    PhaseTransition parsed;
    if (auto status = decode_enum(reader, parsed.from, kMaxPlanPhase, "PhaseTransition.from"); !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.to, kMaxPlanPhase, "PhaseTransition.to"); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.at); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.actor); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.reason); !status.ok()) {
        return status;
    }
    std::uint16_t cause = 0;
    if (auto status = reader.u16(cause); !status.ok()) {
        return status;
    }
    parsed.cause = static_cast<ErrorCode>(cause);
    if (auto status = decode_generation(reader, parsed.sequence); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const ProgressEvent& value) {
    encode_enum(writer, value.kind);
    writer.blob(value.note);
    writer.blob(value.actor);
    writer.i64(value.at);
}

Status decode(Reader& reader, ProgressEvent& value) {
    ProgressEvent parsed;
    if (auto status = decode_enum(reader, parsed.kind, kMaxProgressKind, "ProgressEvent.kind"); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.note); !status.ok()) {
        return status;
    }
    if (auto status = reader.blob(parsed.actor); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.at); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const PlanRecord& value) {
    encode(writer, value.plan);
    encode_enum(writer, value.phase);
    encode_list(writer, value.history, [](Writer& out, const MaintenancePlan& item) { encode(out, item); });
    encode_optional(writer, value.evaluation,
                    [](Writer& out, const PreconditionReport& item) { encode(out, item); });
    encode_optional(writer, value.approval, [](Writer& out, const ApprovalRecord& item) { encode(out, item); });
    encode(writer, value.obligations);
    encode_list(writer, value.receipts, [](Writer& out, const Receipt& item) { encode(out, item); });
    encode_list(writer, value.progress, [](Writer& out, const ProgressEvent& item) { encode(out, item); });
    encode_list(writer, value.exceptions, [](Writer& out, const ExceptionGrant& item) { encode(out, item); });
    encode_list(writer, value.transitions, [](Writer& out, const PhaseTransition& item) { encode(out, item); });
    encode_list(writer, value.blockers, [](Writer& out, const ConditionResult& item) { encode(out, item); });
    encode_generation(writer, value.last_observation_sequence);
    encode_generation(writer, value.recovery_mark);
    writer.boolean(value.recovery_required);
    writer.i64(value.created_at);
    writer.i64(value.updated_at);
    encode_generation(writer, value.sequence);
}

Status decode(Reader& reader, PlanRecord& value) {
    PlanRecord parsed;
    if (auto status = decode(reader, parsed.plan); !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.phase, kMaxPlanPhase, "PlanRecord.phase"); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.history, "PlanRecord.history",
                                  [](Reader& in, MaintenancePlan& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_optional(reader, parsed.evaluation,
                                      [](Reader& in, PreconditionReport& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_optional(reader, parsed.approval,
                                      [](Reader& in, ApprovalRecord& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode(reader, parsed.obligations); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.receipts, "PlanRecord.receipts",
                                  [](Reader& in, Receipt& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.progress, "PlanRecord.progress",
                                  [](Reader& in, ProgressEvent& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.exceptions, "PlanRecord.exceptions",
                                  [](Reader& in, ExceptionGrant& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.transitions, "PlanRecord.transitions",
                                  [](Reader& in, PhaseTransition& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.blockers, "PlanRecord.blockers",
                                  [](Reader& in, ConditionResult& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.last_observation_sequence); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.recovery_mark); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.recovery_required); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.created_at); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.updated_at); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.sequence); !status.ok()) {
        return status;
    }
    parsed.finalize();
    value = std::move(parsed);
    return Status::success();
}

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

void encode(Writer& writer, const RestorationReport& value) {
    encode_ident(writer, value.plan);
    encode_generation(writer, value.plan_revision);
    writer.i64(value.verified_at);
    encode_list(writer, value.outstanding, [](Writer& out, const ObligationId& item) { encode_ident(out, item); });
    encode_list(writer, value.violated_protections,
                [](Writer& out, const ProtectionId& item) { encode_ident(out, item); });
    encode_list(writer, value.redundancy_below_requirement,
                [](Writer& out, const RedundancyGroupId& item) { encode_ident(out, item); });
    encode_list(writer, value.conditions, [](Writer& out, const ConditionResult& item) { encode(out, item); });
    writer.boolean(value.restored);
}

Status decode(Reader& reader, RestorationReport& value) {
    RestorationReport parsed;
    if (auto status = decode_ident(reader, parsed.plan); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.plan_revision); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.verified_at); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.outstanding, "RestorationReport.outstanding",
                                  [](Reader& in, ObligationId& item) { return decode_ident(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.violated_protections, "RestorationReport.violated_protections",
                                  [](Reader& in, ProtectionId& item) { return decode_ident(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.redundancy_below_requirement,
                                  "RestorationReport.redundancy_below_requirement",
                                  [](Reader& in, RedundancyGroupId& item) { return decode_ident(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.conditions, "RestorationReport.conditions",
                                  [](Reader& in, ConditionResult& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.restored); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const CompletionReport& value) {
    encode_ident(writer, value.plan);
    encode_generation(writer, value.plan_revision);
    writer.i64(value.completed_at);
    encode_generation(writer, value.sequence);
    writer.digest(value.plan_digest);
    writer.digest(value.approval_digest);
    writer.digest(value.obligation_set_digest);
    writer.digest(value.receipt_set_digest);
    writer.digest(value.facility_digest);
    writer.digest(value.policy_digest);
    writer.digest(value.dependency_digest);
    encode_list(writer, value.satisfied, [](Writer& out, const ObligationId& item) { encode_ident(out, item); });
    writer.boolean(value.protected_obligations_satisfied);
    writer.boolean(value.restoration_verified);
}

Status decode(Reader& reader, CompletionReport& value) {
    CompletionReport parsed;
    if (auto status = decode_ident(reader, parsed.plan); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.plan_revision); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.completed_at); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.sequence); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.plan_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.approval_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.obligation_set_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.receipt_set_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.facility_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.policy_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.dependency_digest); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.satisfied, "CompletionReport.satisfied",
                                  [](Reader& in, ObligationId& item) { return decode_ident(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.protected_obligations_satisfied); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.restoration_verified); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const RecoveryReport& value) {
    encode_ident(writer, value.plan);
    encode_enum(writer, value.phase_before);
    encode_enum(writer, value.phase_after);
    writer.i64(value.recovered_at);
    encode_generation(writer, value.prior_incarnation);
    encode_generation(writer, value.current_incarnation);
    encode_list(writer, value.notes, [](Writer& out, const std::string& item) { out.blob(item); });
    encode_list(writer, value.outstanding, [](Writer& out, const ObligationId& item) { encode_ident(out, item); });
    writer.boolean(value.recovery_required_after);
}

Status decode(Reader& reader, RecoveryReport& value) {
    RecoveryReport parsed;
    if (auto status = decode_ident(reader, parsed.plan); !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.phase_before, kMaxPlanPhase, "RecoveryReport.phase_before");
        !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.phase_after, kMaxPlanPhase, "RecoveryReport.phase_after");
        !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.recovered_at); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.prior_incarnation); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.current_incarnation); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.notes, "RecoveryReport.notes",
                                  [](Reader& in, std::string& item) { return in.blob(item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.outstanding, "RecoveryReport.outstanding",
                                  [](Reader& in, ObligationId& item) { return decode_ident(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.recovery_required_after); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const Explanation& value) {
    encode_ident(writer, value.plan);
    encode_generation(writer, value.revision);
    encode_enum(writer, value.phase);
    encode_list(writer, value.blockers, [](Writer& out, const ConditionResult& item) { encode(out, item); });
    encode_list(writer, value.reasons, [](Writer& out, const std::string& item) { out.blob(item); });
    encode_list(writer, value.obligations, [](Writer& out, const ObligationStatus& item) { encode(out, item); });
    encode_list(writer, value.next_steps, [](Writer& out, const std::string& item) { out.blob(item); });
    writer.boolean(value.can_evaluate);
    writer.boolean(value.can_approve);
    writer.boolean(value.can_begin);
    writer.boolean(value.can_complete);
    writer.boolean(value.can_cancel);
}

Status decode(Reader& reader, Explanation& value) {
    Explanation parsed;
    if (auto status = decode_ident(reader, parsed.plan); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.revision); !status.ok()) {
        return status;
    }
    if (auto status = decode_enum(reader, parsed.phase, kMaxPlanPhase, "Explanation.phase"); !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.blockers, "Explanation.blockers",
                                  [](Reader& in, ConditionResult& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.reasons, "Explanation.reasons",
                                  [](Reader& in, std::string& item) { return in.blob(item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.obligations, "Explanation.obligations",
                                  [](Reader& in, ObligationStatus& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    if (auto status = decode_list(reader, parsed.next_steps, "Explanation.next_steps",
                                  [](Reader& in, std::string& item) { return in.blob(item); });
        !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.can_evaluate); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.can_approve); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.can_begin); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.can_complete); !status.ok()) {
        return status;
    }
    if (auto status = reader.boolean(parsed.can_cancel); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

// ---------------------------------------------------------------------------
// Ledger
// ---------------------------------------------------------------------------

void encode(Writer& writer, const AttemptRecord& value) {
    writer.digest(value.intent_digest);
    writer.digest(value.outcome_digest);
    encode_generation(writer, value.sequence);
    encode_maybe_ident(writer, value.plan);
    writer.i64(value.applied_at);
}

Status decode(Reader& reader, AttemptRecord& value) {
    AttemptRecord parsed;
    if (auto status = reader.digest(parsed.intent_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.outcome_digest); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.sequence); !status.ok()) {
        return status;
    }
    if (auto status = decode_maybe_ident(reader, parsed.plan); !status.ok()) {
        return status;
    }
    if (auto status = reader.i64(parsed.applied_at); !status.ok()) {
        return status;
    }
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const LedgerState& value) {
    encode_generation(writer, value.sequence);
    writer.u64(value.next_plan_number);
    encode_generation(writer, value.incarnation);
    encode_optional(writer, value.facility,
                    [](Writer& out, const FacilitySnapshot& item) { encode(out, item); });
    writer.u32(static_cast<std::uint32_t>(value.plans.size()));
    for (const auto& entry : value.plans) {
        encode_ident(writer, entry.first);
        encode(writer, entry.second);
    }
    writer.u32(static_cast<std::uint32_t>(value.attempts.size()));
    for (const auto& entry : value.attempts) {
        encode_ident(writer, entry.first);
        encode(writer, entry.second);
    }
}

Status decode(Reader& reader, LedgerState& value) {
    LedgerState parsed;
    if (auto status = decode_generation(reader, parsed.sequence); !status.ok()) {
        return status;
    }
    if (auto status = reader.u64(parsed.next_plan_number); !status.ok()) {
        return status;
    }
    if (auto status = decode_generation(reader, parsed.incarnation); !status.ok()) {
        return status;
    }
    if (auto status = decode_optional(reader, parsed.facility,
                                      [](Reader& in, FacilitySnapshot& item) { return decode(in, item); });
        !status.ok()) {
        return status;
    }
    std::uint32_t plan_count = 0;
    if (auto status = reader.u32(plan_count); !status.ok()) {
        return status;
    }
    if (plan_count > kMaxCollectionItems) {
        return fail(ErrorCode::LimitExceeded, "ledger holds more plans than the accepted limit", "ledger",
                    std::to_string(plan_count));
    }
    for (std::uint32_t index = 0; index < plan_count; ++index) {
        PlanId id;
        PlanRecord record;
        if (auto status = decode_ident(reader, id); !status.ok()) {
            return status;
        }
        if (auto status = decode(reader, record); !status.ok()) {
            return status;
        }
        if (record.plan.id != id) {
            return fail(ErrorCode::IdentityMismatch, "ledger plan key does not match the stored plan identity",
                        "ledger", id.name());
        }
        parsed.plans.emplace(std::move(id), std::move(record));
    }
    std::uint32_t attempt_count = 0;
    if (auto status = reader.u32(attempt_count); !status.ok()) {
        return status;
    }
    if (attempt_count > kMaxCollectionItems) {
        return fail(ErrorCode::LimitExceeded, "ledger holds more attempts than the accepted limit", "ledger",
                    std::to_string(attempt_count));
    }
    for (std::uint32_t index = 0; index < attempt_count; ++index) {
        AttemptId id;
        AttemptRecord record;
        if (auto status = decode_ident(reader, id); !status.ok()) {
            return status;
        }
        if (auto status = decode(reader, record); !status.ok()) {
            return status;
        }
        parsed.attempts.emplace(std::move(id), std::move(record));
    }
    parsed.finalize();
    value = std::move(parsed);
    return Status::success();
}

void encode(Writer& writer, const CommitPayload& value) {
    encode_generation(writer, value.sequence);
    writer.digest(value.transaction_digest);
    writer.u32(value.mutation_bytes);
}

Status decode(Reader& reader, CommitPayload& value) {
    CommitPayload parsed;
    if (auto status = decode_generation(reader, parsed.sequence); !status.ok()) {
        return status;
    }
    if (auto status = reader.digest(parsed.transaction_digest); !status.ok()) {
        return status;
    }
    if (auto status = reader.u32(parsed.mutation_bytes); !status.ok()) {
        return status;
    }
    value = parsed;
    return Status::success();
}

// ---------------------------------------------------------------------------
// Digests
// ---------------------------------------------------------------------------

namespace {

template <typename T, typename EncodeFn>
[[nodiscard]] Digest digest_with(std::string_view purpose_tag, const T& value, EncodeFn encode_value) {
    Writer writer;
    encode_value(writer, value);
    // Qualified: the overload set in this namespace would otherwise hide the
    // generic digest_of(tag, bytes) declared in mc/digest.hpp.
    return mc::digest_of(purpose_tag, writer.data());
}

}  // namespace

Digest digest_of(const MaintenanceScope& value) {
    return digest_with(purpose::kPlanIntent, value,
                       [](Writer& out, const MaintenanceScope& item) { encode(out, item); });
}

Digest digest_of(const PolicyDocument& value) {
    return digest_with(purpose::kPolicy, value, [](Writer& out, const PolicyDocument& item) { encode(out, item); });
}

Digest digest_of(const FacilitySnapshot& value) {
    return digest_with(purpose::kFacilitySnapshot, value,
                       [](Writer& out, const FacilitySnapshot& item) { encode(out, item); });
}

Digest dependency_digest_of(const FacilitySnapshot& value) {
    Writer writer;
    encode_generation(writer, value.dependency_generation);
    encode_generation(writer, value.capacity_generation);
    encode_generation(writer, value.topology_generation);
    encode_list(writer, value.assets, [](Writer& out, const AssetRecord& item) { encode(out, item); });
    encode_list(writer, value.redundancy_groups,
                [](Writer& out, const RedundancyGroup& item) { encode(out, item); });
    encode_list(writer, value.capacity_pools, [](Writer& out, const CapacityPool& item) { encode(out, item); });
    encode_list(writer, value.fabric_segments, [](Writer& out, const FabricSegment& item) { encode(out, item); });
    encode_list(writer, value.protected_obligations,
                [](Writer& out, const ProtectedObligation& item) { encode(out, item); });
    encode_list(writer, value.incidents, [](Writer& out, const IncidentRecord& item) { encode(out, item); });
    encode_list(writer, value.blackouts, [](Writer& out, const BlackoutPeriod& item) { encode(out, item); });
    return mc::digest_of(purpose::kDependencySnapshot, writer.data());
}

Digest digest_of(const Obligation& value) {
    return digest_with(purpose::kObligation, value, [](Writer& out, const Obligation& item) { encode(out, item); });
}

Digest digest_of(const ObligationSet& value) {
    std::vector<Digest> members;
    members.reserve(value.obligations.size());
    for (const auto& obligation : value.obligations) {
        members.push_back(obligation.digest.is_zero() ? digest_of(obligation) : obligation.digest);
    }
    return digest_of_digests(purpose::kObligationSet, std::move(members));
}

Digest digest_of(const Receipt& value) {
    return digest_with(purpose::kReceipt, value, [](Writer& out, const Receipt& item) { encode(out, item); });
}

Digest digest_of(const ObligationStatusReport& value) {
    return digest_with(purpose::kObligationSet, value,
                       [](Writer& out, const ObligationStatusReport& item) { encode(out, item); });
}

Digest digest_of(const PreconditionReport& value) {
    return digest_with(purpose::kPreconditionReport, value,
                       [](Writer& out, const PreconditionReport& item) { encode(out, item); });
}

Digest digest_of(const ApprovalRecord& value) {
    return digest_with(purpose::kApproval, value, [](Writer& out, const ApprovalRecord& item) { encode(out, item); });
}

Digest digest_of(const ExceptionGrant& value) {
    return digest_with(purpose::kExceptionGrant, value,
                       [](Writer& out, const ExceptionGrant& item) { encode(out, item); });
}

Digest digest_of(const MaintenancePlan& value) {
    return digest_with(purpose::kPlanDigest, value,
                       [](Writer& out, const MaintenancePlan& item) { encode(out, item); });
}

Digest digest_of(const PhaseTransition& value) {
    return digest_with(purpose::kPhaseTransition, value,
                       [](Writer& out, const PhaseTransition& item) { encode(out, item); });
}

Digest digest_of(const ProgressEvent& value) {
    return digest_with(purpose::kProgressEvent, value,
                       [](Writer& out, const ProgressEvent& item) { encode(out, item); });
}

Digest digest_of(const PlanRecord& value) {
    return digest_with(purpose::kPlanRecord, value, [](Writer& out, const PlanRecord& item) { encode(out, item); });
}

Digest digest_of(const RestorationReport& value) {
    return digest_with(purpose::kOutcome, value,
                       [](Writer& out, const RestorationReport& item) { encode(out, item); });
}

Digest digest_of(const CompletionReport& value) {
    return digest_with(purpose::kOutcome, value,
                       [](Writer& out, const CompletionReport& item) { encode(out, item); });
}

Digest digest_of(const RecoveryReport& value) {
    return digest_with(purpose::kOutcome, value,
                       [](Writer& out, const RecoveryReport& item) { encode(out, item); });
}

Digest digest_of(const Explanation& value) {
    return digest_with(purpose::kOutcome, value, [](Writer& out, const Explanation& item) { encode(out, item); });
}

Digest digest_of(const LedgerState& value) {
    return digest_with(purpose::kLedgerState, value,
                       [](Writer& out, const LedgerState& item) { encode(out, item); });
}

Digest digest_of_digests(std::string_view purpose_tag, std::vector<Digest> digests) {
    std::sort(digests.begin(), digests.end());
    Writer writer;
    writer.u32(static_cast<std::uint32_t>(digests.size()));
    for (const auto& digest : digests) {
        writer.digest(digest);
    }
    return mc::digest_of(purpose_tag, writer.data());
}

}  // namespace mc::detail

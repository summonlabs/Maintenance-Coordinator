// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// INTERNAL helper for the stable textual spellings of enums.

#ifndef MC_INTERNAL_ENUM_TABLE_HPP
#define MC_INTERNAL_ENUM_TABLE_HPP

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace mc::detail {

template <std::size_t N>
[[nodiscard]] bool parse_enum_name(
    std::string_view text,
    const std::array<std::pair<std::string_view, unsigned>, N>& table,
    unsigned& out) noexcept {
    for (const auto& entry : table) {
        if (entry.first == text) {
            out = entry.second;
            return true;
        }
    }
    return false;
}

template <std::size_t N>
[[nodiscard]] std::string_view enum_name_of(
    unsigned value,
    const std::array<std::pair<std::string_view, unsigned>, N>& table) noexcept {
    for (const auto& entry : table) {
        if (entry.second == value) {
            return entry.first;
        }
    }
    return "unknown";
}

}  // namespace mc::detail

#endif  // MC_INTERNAL_ENUM_TABLE_HPP

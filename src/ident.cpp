// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.

#include "mc/ident.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace mc {
namespace {

[[nodiscard]] constexpr bool is_identifier_byte(unsigned char value) noexcept {
    const bool alpha = (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) ||
                       (value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z'));
    const bool digit = value >= static_cast<unsigned char>('0') && value <= static_cast<unsigned char>('9');
    switch (value) {
        case static_cast<unsigned char>('.'):
        case static_cast<unsigned char>('_'):
        case static_cast<unsigned char>(':'):
        case static_cast<unsigned char>('@'):
        case static_cast<unsigned char>('/'):
        case static_cast<unsigned char>('+'):
        case static_cast<unsigned char>('-'):
            return true;
        default:
            return alpha || digit;
    }
}

// `/` and `:` are accepted inside names because DCCP identities are often
// path shaped ("site/site-a/rack/r07"), but a name may not be "." or ".." and
// may not contain a path traversal component, so an identity can never be
// mistaken for a filesystem path.
[[nodiscard]] bool has_traversal_component(std::string_view name) noexcept {
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t end = name.find('/', start);
        const std::string_view part = name.substr(start, (end == std::string_view::npos ? name.size() : end) - start);
        if (part == "." || part == "..") {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1U;
    }
    return false;
}

[[nodiscard]] constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

bool is_valid_identifier(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxIdentifierLength) {
        return false;
    }
    for (const char raw : name) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte > 0x7FU) {
            return false;  // ASCII only: no normalisation, no case folding
        }
        if (!is_identifier_byte(byte)) {
            return false;
        }
    }
    if (name.front() == '/' || name.back() == '/') {
        return false;
    }
    if (name.find("//") != std::string_view::npos) {
        return false;
    }
    return !has_traversal_component(name);
}

Status validate_identifier(std::string_view name, std::string_view what) {
    if (name.empty()) {
        return fail(ErrorCode::InvalidArgument, "identifier must not be empty", std::string(what), "empty");
    }
    if (name.size() > kMaxIdentifierLength) {
        return fail(ErrorCode::InvalidArgument, "identifier is too long", std::string(what),
                    "length " + std::to_string(name.size()) + " exceeds " +
                        std::to_string(kMaxIdentifierLength));
    }
    if (!is_valid_identifier(name)) {
        return fail(ErrorCode::InvalidArgument, "identifier contains characters that are not allowed",
                    std::string(what), std::string(name));
    }
    return Status::success();
}

bool Digest::parse(std::string_view hex, Digest& out) noexcept {
    if (hex.size() != kDigestBytes * 2U) {
        return false;
    }
    std::array<std::uint8_t, kDigestBytes> bytes{};
    for (std::size_t i = 0; i < kDigestBytes; ++i) {
        const int high = hex_value(hex[i * 2U]);
        const int low = hex_value(hex[(i * 2U) + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    out = Digest::from_bytes(bytes);
    return true;
}

std::string Digest::hex() const {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.resize(kDigestBytes * 2U);
    for (std::size_t i = 0; i < kDigestBytes; ++i) {
        const auto byte = bytes_[i];
        text[i * 2U] = kDigits[(byte >> 4U) & 0x0FU];
        text[(i * 2U) + 1U] = kDigits[byte & 0x0FU];
    }
    return text;
}

bool Digest::is_zero() const noexcept {
    for (const auto byte : bytes_) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace mc

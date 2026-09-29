// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.

#include "mc/digest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mc {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants{{
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
}};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned shift) noexcept {
    return (value >> shift) | (value << (32U - shift));
}

[[nodiscard]] std::uint32_t load_be32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) | (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

void store_be32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    out[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

[[nodiscard]] const std::array<std::uint32_t, 256>& crc_table() noexcept {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0; index < 256U; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) != 0U ? (0xEDB88320U ^ (value >> 1U)) : (value >> 1U);
            }
            values[index] = value;
        }
        return values;
    }();
    return table;
}

}  // namespace

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

void Sha256::reset() noexcept {
    state_[0] = 0x6A09E667U;
    state_[1] = 0xBB67AE85U;
    state_[2] = 0x3C6EF372U;
    state_[3] = 0xA54FF53AU;
    state_[4] = 0x510E527FU;
    state_[5] = 0x9B05688CU;
    state_[6] = 0x1F83D9ABU;
    state_[7] = 0x5BE0CD19U;
    bit_length_ = 0;
    buffer_length_ = 0;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha256::process_block(const std::uint8_t* block) noexcept {
    std::uint32_t schedule[64];
    for (std::size_t index = 0; index < 16U; ++index) {
        schedule[index] = load_be32(block + (index * 4U));
    }
    for (std::size_t index = 16U; index < 64U; ++index) {
        const std::uint32_t s0 = rotr(schedule[index - 15U], 7U) ^ rotr(schedule[index - 15U], 18U) ^
                                 (schedule[index - 15U] >> 3U);
        const std::uint32_t s1 = rotr(schedule[index - 2U], 17U) ^ rotr(schedule[index - 2U], 19U) ^
                                 (schedule[index - 2U] >> 10U);
        schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t index = 0; index < 64U; ++index) {
        const std::uint32_t s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kSha256RoundConstants[index] + schedule[index];
        const std::uint32_t s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size == 0U) {
        return;
    }
    bit_length_ += static_cast<std::uint64_t>(size) * 8U;
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t space = 64U - buffer_length_;
        const std::size_t take = std::min(space, size - offset);
        std::memcpy(buffer_ + buffer_length_, data + offset, take);
        buffer_length_ += take;
        offset += take;
        if (buffer_length_ == 64U) {
            process_block(buffer_);
            buffer_length_ = 0;
        }
    }
}

void Sha256::update(std::string_view text) noexcept {
    update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

Digest Sha256::finish() const noexcept {
    Sha256 copy = *this;
    const std::uint64_t bits = copy.bit_length_;
    const std::uint8_t marker = 0x80U;
    const std::uint8_t zero = 0x00U;
    copy.update(&marker, 1U);
    while (copy.buffer_length_ != 56U) {
        copy.update(&zero, 1U);
    }
    std::uint8_t length_bytes[8];
    for (std::size_t index = 0; index < 8U; ++index) {
        length_bytes[index] = static_cast<std::uint8_t>((bits >> (56U - (8U * index))) & 0xFFU);
    }
    copy.update(length_bytes, 8U);

    std::array<std::uint8_t, kDigestBytes> out{};
    for (std::size_t index = 0; index < 8U; ++index) {
        store_be32(out.data() + (index * 4U), copy.state_[index]);
    }
    return Digest::from_bytes(out);
}

// ---------------------------------------------------------------------------
// Hasher
// ---------------------------------------------------------------------------

void Hasher::update_u8(std::uint8_t value) noexcept {
    sha_.update(&value, 1U);
}

void Hasher::update_u16(std::uint16_t value) noexcept {
    const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value & 0xFFU),
                                   static_cast<std::uint8_t>((value >> 8U) & 0xFFU)};
    sha_.update(bytes, 2U);
}

void Hasher::update_u32(std::uint32_t value) noexcept {
    const std::uint8_t bytes[4] = {static_cast<std::uint8_t>(value & 0xFFU),
                                   static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
                                   static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
                                   static_cast<std::uint8_t>((value >> 24U) & 0xFFU)};
    sha_.update(bytes, 4U);
}

void Hasher::update_u64(std::uint64_t value) noexcept {
    std::uint8_t bytes[8];
    for (std::size_t index = 0; index < 8U; ++index) {
        bytes[index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU);
    }
    sha_.update(bytes, 8U);
}

void Hasher::update_i64(std::int64_t value) noexcept {
    update_u64(static_cast<std::uint64_t>(value));
}

void Hasher::update_bytes(std::string_view text) noexcept {
    // Length prefixed so that ("ab", "c") and ("a", "bc") never collide.
    update_u32(static_cast<std::uint32_t>(text.size()));
    sha_.update(text);
}

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t seed) noexcept {
    const auto& table = crc_table();
    std::uint32_t value = seed ^ 0xFFFFFFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        value = table[(value ^ data[index]) & 0xFFU] ^ (value >> 8U);
    }
    return value ^ 0xFFFFFFFFU;
}

std::uint32_t crc32(std::string_view text, std::uint32_t seed) noexcept {
    return crc32(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), seed);
}

// ---------------------------------------------------------------------------
// Canonical writer
// ---------------------------------------------------------------------------

void Writer::u16(std::uint16_t value) {
    buffer_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    buffer_.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void Writer::u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void Writer::u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void Writer::i64(std::int64_t value) {
    u64(static_cast<std::uint64_t>(value));
}

void Writer::bytes(const std::uint8_t* data, std::size_t size) {
    buffer_.insert(buffer_.end(), data, data + size);
}

void Writer::blob(std::string_view value) {
    // Callers must have bounded the value already; the reader enforces the same
    // bound, so an over-long blob can never be read back as a valid record.
    u32(static_cast<std::uint32_t>(value.size()));
    bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}

// ---------------------------------------------------------------------------
// Canonical reader
// ---------------------------------------------------------------------------

Status Reader::need(std::size_t count, std::string_view what) const {
    if (remaining() < count) {
        return fail(ErrorCode::RecordTruncated, "canonical input ended before the end of " + std::string(what),
                    std::string(what),
                    "need " + std::to_string(count) + " bytes, have " + std::to_string(remaining()));
    }
    return Status::success();
}

Status Reader::u8(std::uint8_t& out) {
    if (auto status = need(1U, "u8"); !status.ok()) {
        return status;
    }
    out = data_[offset_];
    ++offset_;
    return Status::success();
}

Status Reader::u16(std::uint16_t& out) {
    if (auto status = need(2U, "u16"); !status.ok()) {
        return status;
    }
    out = static_cast<std::uint16_t>(data_[offset_]) |
          static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_ + 1U]) << 8U);
    offset_ += 2U;
    return Status::success();
}

Status Reader::u32(std::uint32_t& out) {
    if (auto status = need(4U, "u32"); !status.ok()) {
        return status;
    }
    out = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        out |= static_cast<std::uint32_t>(data_[offset_ + index]) << (8U * index);
    }
    offset_ += 4U;
    return Status::success();
}

Status Reader::u64(std::uint64_t& out) {
    if (auto status = need(8U, "u64"); !status.ok()) {
        return status;
    }
    out = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        out |= static_cast<std::uint64_t>(data_[offset_ + index]) << (8U * index);
    }
    offset_ += 8U;
    return Status::success();
}

Status Reader::i64(std::int64_t& out) {
    std::uint64_t raw = 0;
    if (auto status = u64(raw); !status.ok()) {
        return status;
    }
    out = static_cast<std::int64_t>(raw);
    return Status::success();
}

Status Reader::boolean(bool& out) {
    std::uint8_t raw = 0;
    if (auto status = u8(raw); !status.ok()) {
        return status;
    }
    if (raw > 1U) {
        return fail(ErrorCode::ReservedFieldViolation, "boolean field is neither 0 nor 1", "boolean",
                    std::to_string(raw));
    }
    out = raw == 1U;
    return Status::success();
}

Status Reader::bytes(std::uint8_t* out, std::size_t count) {
    if (auto status = need(count, "bytes"); !status.ok()) {
        return status;
    }
    std::memcpy(out, data_ + offset_, count);
    offset_ += count;
    return Status::success();
}

Status Reader::blob(std::string& out) {
    std::uint32_t size = 0;
    if (auto status = u32(size); !status.ok()) {
        return status;
    }
    if (size > kCanonicalMaxBlob) {
        return fail(ErrorCode::LimitExceeded, "length prefixed blob exceeds the canonical limit", "blob",
                    std::to_string(size));
    }
    if (auto status = need(size, "blob"); !status.ok()) {
        return status;
    }
    out.assign(reinterpret_cast<const char*>(data_ + offset_), size);
    offset_ += size;
    return Status::success();
}

Status Reader::digest(Digest& out) {
    std::array<std::uint8_t, kDigestBytes> bytes{};
    if (auto status = this->bytes(bytes.data(), bytes.size()); !status.ok()) {
        return status;
    }
    out = Digest::from_bytes(bytes);
    return Status::success();
}

// ---------------------------------------------------------------------------
// Digest helpers
// ---------------------------------------------------------------------------

Digest digest_of(std::string_view tag, const std::vector<std::uint8_t>& body) {
    Hasher hasher;
    hasher.update(tag);
    hasher.update_u32(static_cast<std::uint32_t>(body.size()));
    hasher.update(body.data(), body.size());
    return hasher.finish();
}

Digest digest_of(std::string_view tag, std::string_view body) {
    Hasher hasher;
    hasher.update(tag);
    hasher.update_u32(static_cast<std::uint32_t>(body.size()));
    hasher.update(body);
    return hasher.finish();
}

std::string to_hex(const std::uint8_t* data, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.resize(size * 2U);
    for (std::size_t index = 0; index < size; ++index) {
        text[index * 2U] = kDigits[(data[index] >> 4U) & 0x0FU];
        text[(index * 2U) + 1U] = kDigits[data[index] & 0x0FU];
    }
    return text;
}

bool from_hex(std::string_view hex, std::vector<std::uint8_t>& out) {
    if (hex.size() % 2U != 0U) {
        return false;
    }
    out.clear();
    out.reserve(hex.size() / 2U);
    const auto value_of = [](char c) -> int {
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
    };
    for (std::size_t index = 0; index < hex.size(); index += 2U) {
        const int high = value_of(hex[index]);
        const int low = value_of(hex[index + 1U]);
        if (high < 0 || low < 0) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return true;
}

}  // namespace mc

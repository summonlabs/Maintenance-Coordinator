// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Foundation tests: SHA-256, CRC-32, domain separated digests, the canonical
// writer/reader, the error model, the strict JSON boundary and injected time.
//
// Every case here is deterministic: no wall clock, no environment, no network,
// no machine specific path and no test timeout.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The public headers come first: mc_test.hpp adds stream inserters for Digest
// and PlanPhase, so both types must already be declared when it is read.
#include "mc/digest.hpp"
#include "mc/ident.hpp"
#include "mc/json.hpp"
#include "mc/plan.hpp"
#include "mc/time.hpp"

#include "../mc_test.hpp"

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::string sha256_hex(std::string_view text) {
    mc::Sha256 hasher;
    hasher.update(text);
    return hasher.finish().hex();
}

// Deterministic byte pattern: the same length always yields the same bytes.
[[nodiscard]] std::vector<std::uint8_t> pattern_bytes(std::size_t count) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(((index * 37U) + 11U) % 251U));
    }
    return bytes;
}

[[nodiscard]] mc::Digest digest_from_hex(std::string_view hex) {
    mc::Digest digest;
    MC_REQUIRE(mc::Digest::parse(hex, digest));
    return digest;
}

[[nodiscard]] mc::Status read_u16_with(std::size_t available) {
    const std::vector<std::uint8_t> bytes(available, 0x5AU);
    mc::Reader reader(bytes);
    std::uint16_t value = 0;
    return reader.u16(value);
}

[[nodiscard]] mc::Status read_u32_with(std::size_t available) {
    const std::vector<std::uint8_t> bytes(available, 0x5AU);
    mc::Reader reader(bytes);
    std::uint32_t value = 0;
    return reader.u32(value);
}

[[nodiscard]] mc::Status read_u64_with(std::size_t available) {
    const std::vector<std::uint8_t> bytes(available, 0x5AU);
    mc::Reader reader(bytes);
    std::uint64_t value = 0;
    return reader.u64(value);
}

[[nodiscard]] mc::Status read_i64_with(std::size_t available) {
    const std::vector<std::uint8_t> bytes(available, 0x5AU);
    mc::Reader reader(bytes);
    std::int64_t value = 0;
    return reader.i64(value);
}

[[nodiscard]] mc::Status read_digest_with(std::size_t available) {
    const std::vector<std::uint8_t> bytes(available, 0x5AU);
    mc::Reader reader(bytes);
    mc::Digest value;
    return reader.digest(value);
}

// A length prefixed blob whose declared length and whose present byte count are
// chosen independently, so the two reader guards can be told apart.
[[nodiscard]] mc::Status read_blob_declaring(std::uint32_t declared, std::size_t present) {
    mc::Writer header;
    header.u32(declared);
    std::vector<std::uint8_t> bytes = header.data();
    bytes.resize(bytes.size() + present, 0x00U);
    mc::Reader reader(bytes);
    std::string out;
    return reader.blob(out);
}

// A miniature record parser: one fixed shape, then every input byte must have
// been consumed.  This is the "record parser context" in which the library
// documents ErrorCode::TrailingBytes.
[[nodiscard]] mc::Status parse_u32_record(const std::vector<std::uint8_t>& bytes, std::uint32_t& out) {
    mc::Reader reader(bytes);
    if (auto status = reader.u32(out); !status.ok()) {
        return status;
    }
    if (!reader.fully_consumed()) {
        return mc::fail(mc::ErrorCode::TrailingBytes, "record has bytes after its final field", "record",
                        std::to_string(reader.remaining()));
    }
    return mc::Status::success();
}

void check_json_rejected(std::string_view text, mc::ErrorCode expected, const char* label) {
    auto parsed = mc::json::parse(text);
    MC_CHECK_MSG(!parsed.ok(), std::string(label) + ": expected a rejection");
    if (parsed.ok()) {
        return;
    }
    MC_CHECK_MSG(parsed.status().code() == expected,
                 std::string(label) + ": expected " + std::string(mc::to_string(expected)) + ", got " +
                     std::string(mc::to_string(parsed.status().code())) + " -- " +
                     std::string(parsed.status().render()));
}

// Recursive structural equality; mc::json::Value deliberately has no operator==.
[[nodiscard]] bool json_equal(const mc::json::Value& left, const mc::json::Value& right) {
    if (left.kind() != right.kind()) {
        return false;
    }
    switch (left.kind()) {
        case mc::json::Value::Kind::Null:
            return true;
        case mc::json::Value::Kind::Bool:
            return *left.as_bool() == *right.as_bool();
        case mc::json::Value::Kind::Int:
            return *left.as_int() == *right.as_int();
        case mc::json::Value::Kind::String:
            return *left.as_string() == *right.as_string();
        case mc::json::Value::Kind::Array: {
            const auto& lhs = *left.as_array();
            const auto& rhs = *right.as_array();
            if (lhs.size() != rhs.size()) {
                return false;
            }
            for (std::size_t index = 0; index < lhs.size(); ++index) {
                if (!json_equal(lhs[index], rhs[index])) {
                    return false;
                }
            }
            return true;
        }
        case mc::json::Value::Kind::Object: {
            const auto& lhs = *left.as_object();
            const auto& rhs = *right.as_object();
            if (lhs.size() != rhs.size()) {
                return false;
            }
            for (const auto& member : lhs) {
                const mc::json::Value* other = right.find(member.first);
                if (other == nullptr || !json_equal(member.second, *other)) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

// Whole UTF-8 fragments, so a generated string stays valid UTF-8.
[[nodiscard]] std::string random_text(mc::test::Random& random) {
    static const std::array<std::string_view, 12> kFragments{
        "a",  "Z",   "0",          " ",        "\"", "\\\\",
        "\n", "\t",  "\r",         "/",        "\xC3\xA9", "\xF0\x9F\x98\x80"};
    std::string out;
    const std::size_t count = random.next_below(9U);
    for (std::size_t index = 0; index < count; ++index) {
        out += kFragments[random.next_below(kFragments.size())];
    }
    return out;
}

[[nodiscard]] mc::json::Value random_json(mc::test::Random& random, std::size_t depth) {
    const std::uint64_t pick = depth == 0U ? random.next_below(4U) : random.next_below(6U);
    switch (pick) {
        case 0U:
            return mc::json::Value(nullptr);
        case 1U:
            return mc::json::Value(random.next_bool());
        case 2U:
            return mc::json::Value(static_cast<std::int64_t>(random.next_u64() >> 1U) - 4611686018427387904LL);
        case 3U:
            return mc::json::Value(random_text(random));
        case 4U: {
            mc::json::Value array = mc::json::Value::array();
            const std::size_t count = random.next_below(5U);
            for (std::size_t index = 0; index < count; ++index) {
                array.push(random_json(random, depth - 1U));
            }
            return array;
        }
        default: {
            mc::json::Value object = mc::json::Value::object();
            const std::size_t count = random.next_below(5U);
            for (std::size_t index = 0; index < count; ++index) {
                object.set("k" + std::to_string(index), random_json(random, depth - 1U));
            }
            return object;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

MC_TEST(sha256_known_answer_vectors) {
    // FIPS 180-4 examples.
    MC_CHECK_EQ(sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    MC_CHECK_EQ(sha256_hex("abc"),
                std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    MC_CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    // Hasher is the same hash with the little-endian helper surface.
    mc::Hasher hasher;
    hasher.update(std::string_view("abc"));
    MC_CHECK_EQ(hasher.finish().hex(), sha256_hex("abc"));
    mc::Hasher hasher_by_bytes;
    const std::string abc("abc");
    hasher_by_bytes.update(reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size());
    MC_CHECK(hasher_by_bytes.finish() == hasher.finish());

    // One million 'a' characters, streamed in 1000 byte chunks.
    mc::Sha256 million;
    const std::string chunk(1000U, 'a');
    for (std::size_t index = 0; index < 1000U; ++index) {
        million.update(chunk);
    }
    MC_CHECK_EQ(million.byte_count(), static_cast<std::uint64_t>(1000000U));
    MC_CHECK_EQ(million.finish().hex(),
                std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    MC_CHECK_EQ(sha256_hex(std::string(1000000U, 'a')),
                std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

    // A default constructed hasher hashes the empty string and a reset one does too.
    mc::Sha256 reset;
    reset.update("discarded payload");
    MC_CHECK_EQ(reset.byte_count(), static_cast<std::uint64_t>(17U));
    reset.reset();
    MC_CHECK_EQ(reset.byte_count(), static_cast<std::uint64_t>(0U));
    MC_CHECK_EQ(reset.finish().hex(), sha256_hex(""));
}

MC_TEST(sha256_streaming_matches_one_shot_across_block_boundaries) {
    const std::array<std::size_t, 13> sizes{{0U, 1U, 31U, 55U, 56U, 63U, 64U, 65U, 111U, 119U, 120U, 127U, 128U}};
    for (const std::size_t size : sizes) {
        const std::vector<std::uint8_t> bytes = pattern_bytes(size);

        mc::Sha256 one_shot;
        one_shot.update(bytes.data(), bytes.size());
        const mc::Digest expected = one_shot.finish();
        MC_CHECK_EQ(one_shot.byte_count(), static_cast<std::uint64_t>(size));

        // Two chunks.  finish() finalises a copy, so calling it in the middle
        // must leave the hasher exactly where it was.
        const std::size_t half = size / 2U;
        mc::Sha256 two_chunks;
        two_chunks.update(bytes.data(), half);
        const mc::Digest partial = two_chunks.finish();
        MC_CHECK(two_chunks.finish() == partial);
        MC_CHECK_EQ(two_chunks.byte_count(), static_cast<std::uint64_t>(half));
        two_chunks.update(bytes.data() + half, size - half);
        MC_CHECK(two_chunks.finish() == expected);
        MC_CHECK_EQ(two_chunks.byte_count(), static_cast<std::uint64_t>(size));

        // Three chunks, including a chunk boundary exactly on a block edge.
        const std::size_t first = size / 3U;
        const std::size_t second = (size * 2U) / 3U;
        mc::Sha256 three_chunks;
        three_chunks.update(bytes.data(), first);
        three_chunks.update(bytes.data() + first, second - first);
        three_chunks.update(bytes.data() + second, size - second);
        MC_CHECK(three_chunks.finish() == expected);

        // A hasher reused after finish() still hashes the whole input.
        mc::Sha256 reused;
        reused.update(bytes.data(), size);
        MC_CHECK(reused.finish() == expected);
        reused.update(bytes.data(), size);
        MC_CHECK(size == 0U || reused.finish() != expected);
        MC_CHECK_EQ(reused.byte_count(), static_cast<std::uint64_t>(size * 2U));

        // One byte at a time for the small inputs.
        if (size <= 65U) {
            mc::Sha256 bytewise;
            for (std::size_t index = 0; index < size; ++index) {
                bytewise.update(bytes.data() + index, 1U);
            }
            MC_CHECK(bytewise.finish() == expected);
        }
    }
}

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------

MC_TEST(crc32_known_answers_and_incremental_seeding) {
    MC_CHECK_EQ(mc::crc32(std::string_view("123456789")), 0xCBF43926U);
    MC_CHECK_EQ(mc::crc32(std::string_view("")), 0U);
    const std::string text("The quick brown fox jumps over the lazy dog");
    MC_CHECK_EQ(mc::crc32(text), 0x414FA339U);

    // Seeding with the running value is the same as hashing the concatenation.
    const std::string_view view(text);
    const std::string_view head = view.substr(0U, 20U);
    const std::string_view tail = view.substr(20U);
    MC_CHECK_EQ(mc::crc32(tail, mc::crc32(head)), mc::crc32(view));

    // The pointer and the string_view overloads agree.
    MC_CHECK_EQ(mc::crc32(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), mc::crc32(view));
    MC_CHECK_EQ(mc::crc32(static_cast<const std::uint8_t*>(nullptr), 0U), 0U);

    // Byte at a time chaining, seeded with the empty prefix.
    std::uint32_t running = 0U;
    for (const char letter : text) {
        running = mc::crc32(reinterpret_cast<const std::uint8_t*>(&letter), 1U, running);
    }
    MC_CHECK_EQ(running, mc::crc32(view));
}

// ---------------------------------------------------------------------------
// Domain separated digests
// ---------------------------------------------------------------------------

MC_TEST(digest_domain_separation_and_length_prefixing) {
    const std::vector<std::uint8_t> body{0x01U, 0x02U, 0x03U};
    const mc::Digest policy = mc::digest_of(mc::purpose::kPolicy, body);
    const mc::Digest dependency = mc::digest_of(mc::purpose::kDependencySnapshot, body);
    MC_CHECK(policy != dependency);
    MC_CHECK(mc::digest_of(mc::purpose::kPlanIntent, body) != mc::digest_of(mc::purpose::kPlanDigest, body));
    MC_CHECK(mc::digest_of(mc::purpose::kPolicy, body) != mc::digest_of(mc::purpose::kPolicy, std::string_view("")));

    // The byte and the text overloads agree for the same body.
    const std::string_view text_body(reinterpret_cast<const char*>(body.data()), body.size());
    MC_CHECK(mc::digest_of(mc::purpose::kPolicy, text_body) == policy);

    // update_bytes prefixes the length, so ("ab","c") and ("a","bc") differ.
    mc::Hasher left;
    left.update_bytes("ab");
    left.update_bytes("c");
    mc::Hasher right;
    right.update_bytes("a");
    right.update_bytes("bc");
    MC_CHECK(left.finish() != right.finish());

    // ... and it is exactly a u32 length followed by the raw bytes.
    mc::Hasher manual;
    manual.update_u32(2U);
    manual.update(std::string_view("ab"));
    manual.update_u32(1U);
    manual.update(std::string_view("c"));
    MC_CHECK(manual.finish() == left.finish());

    // The purpose tag and the length prefix are both part of digest_of().
    mc::Sha256 naive;
    naive.update(mc::purpose::kPolicy);
    naive.update(std::string_view("abc"));
    MC_CHECK(naive.finish() != mc::digest_of(mc::purpose::kPolicy, std::string_view("abc")));

    // Integer helpers are little endian and width exact.
    mc::Hasher widths;
    widths.update_u16(0x0201U);
    widths.update_u32(0x06050403U);
    widths.update_u64(0x0E0D0C0B0A090807ULL);
    mc::Sha256 bytes;
    const std::array<std::uint8_t, 14> expected_bytes{{0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
                                                       0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU}};
    bytes.update(expected_bytes.data(), expected_bytes.size());
    MC_CHECK(widths.finish() == bytes.finish());
    const std::array<std::uint8_t, 14> reversed_bytes{{0x0EU, 0x0DU, 0x0CU, 0x0BU, 0x0AU, 0x09U, 0x08U, 0x07U,
                                                       0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U}};
    mc::Sha256 reversed;
    reversed.update(reversed_bytes.data(), reversed_bytes.size());
    MC_CHECK(widths.finish() != reversed.finish());
}

MC_TEST(digest_parse_hex_round_trip_and_zero) {
    const std::string hex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    mc::Digest parsed;
    MC_CHECK(mc::Digest::parse(hex, parsed));
    MC_CHECK_EQ(parsed.hex(), hex);
    MC_CHECK(!parsed.is_zero());
    MC_CHECK_EQ(parsed.bytes().size(), 32U);
    MC_CHECK_EQ(static_cast<unsigned>(parsed.bytes()[0]), 0x01U);
    MC_CHECK_EQ(static_cast<unsigned>(parsed.bytes()[31]), 0xEFU);

    const std::string upper("0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF");
    mc::Digest from_upper;
    MC_CHECK(mc::Digest::parse(upper, from_upper));
    MC_CHECK(from_upper == parsed);
    MC_CHECK_EQ(from_upper.hex(), hex);

    // Wrong lengths are rejected and never write the output.
    mc::Digest untouched = parsed;
    MC_CHECK(!mc::Digest::parse(std::string_view(hex).substr(0U, 63U), untouched));
    MC_CHECK(!mc::Digest::parse(hex + "0", untouched));
    MC_CHECK(!mc::Digest::parse(std::string_view(""), untouched));
    MC_CHECK(!mc::Digest::parse(std::string_view(hex).substr(0U, 32U), untouched));
    MC_CHECK(untouched == parsed);

    // Non hexadecimal characters are rejected.
    std::string non_hex = hex;
    non_hex[5] = 'z';
    MC_CHECK(!mc::Digest::parse(non_hex, untouched));
    std::string spaced = hex;
    spaced[10] = ' ';
    MC_CHECK(!mc::Digest::parse(spaced, untouched));
    std::string prefixed = "0x" + hex;
    MC_CHECK(!mc::Digest::parse(prefixed, untouched));

    // Zero digest.
    const mc::Digest zero;
    MC_CHECK(zero.is_zero());
    MC_CHECK_EQ(zero.hex(), std::string(64U, '0'));
    MC_CHECK(zero != parsed);
    mc::Digest reparsed_zero;
    MC_CHECK(mc::Digest::parse(zero.hex(), reparsed_zero));
    MC_CHECK(reparsed_zero.is_zero());
    MC_CHECK(mc::Digest{}.bytes() == reparsed_zero.bytes());

    // Shared hex helpers.
    std::vector<std::uint8_t> bytes;
    MC_CHECK(mc::from_hex(hex, bytes));
    MC_CHECK_EQ(bytes.size(), 32U);
    MC_CHECK_EQ(mc::to_hex(bytes.data(), bytes.size()), hex);
    MC_CHECK(!mc::from_hex("abc", bytes));
    MC_CHECK(!mc::from_hex("zz", bytes));
    MC_CHECK(mc::from_hex("", bytes));
    MC_CHECK(bytes.empty());
    MC_CHECK_EQ(mc::to_hex(nullptr, 0U), std::string(""));

    // A known answer ties parse() and hex() to the SHA-256 path.
    const mc::Digest abc = digest_from_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    mc::Sha256 hasher;
    hasher.update(std::string_view("abc"));
    MC_CHECK(abc == hasher.finish());
    MC_CHECK_EQ(abc.hex(), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

// ---------------------------------------------------------------------------
// Identities
// ---------------------------------------------------------------------------

MC_TEST(identifier_validation) {
    MC_CHECK(mc::is_valid_identifier("site/site-a/rack/r07"));
    MC_CHECK(mc::is_valid_identifier("asset_01"));
    MC_CHECK(mc::is_valid_identifier("a.b:c@d+e-f"));
    MC_CHECK(mc::is_valid_identifier("0"));
    MC_CHECK(mc::is_valid_identifier(std::string(mc::kMaxIdentifierLength, 'a')));

    MC_CHECK(!mc::is_valid_identifier(""));
    MC_CHECK(!mc::is_valid_identifier(std::string(mc::kMaxIdentifierLength + 1U, 'a')));
    MC_CHECK(!mc::is_valid_identifier("/site-a"));
    MC_CHECK(!mc::is_valid_identifier("site-a/"));
    MC_CHECK(!mc::is_valid_identifier("site//a"));
    MC_CHECK(!mc::is_valid_identifier("."));
    MC_CHECK(!mc::is_valid_identifier(".."));
    MC_CHECK(!mc::is_valid_identifier("site/../rack"));
    MC_CHECK(!mc::is_valid_identifier("site/./rack"));
    MC_CHECK(!mc::is_valid_identifier("asset 01"));
    MC_CHECK(!mc::is_valid_identifier("asset\t01"));
    MC_CHECK(!mc::is_valid_identifier("asset\n01"));
    MC_CHECK(!mc::is_valid_identifier(" leading"));
    MC_CHECK(!mc::is_valid_identifier("trailing "));
    MC_CHECK(!mc::is_valid_identifier("caf\xC3\xA9"));
    MC_CHECK(!mc::is_valid_identifier("\xFF"));
    MC_CHECK(!mc::is_valid_identifier("asset#1"));
    MC_CHECK(!mc::is_valid_identifier("asset%31"));

    MC_CHECK(mc::validate_identifier("asset-01", "asset").ok());
    MC_CHECK_CODE(mc::validate_identifier("", "asset"), mc::ErrorCode::InvalidArgument);
    MC_CHECK_CODE(mc::validate_identifier(std::string(mc::kMaxIdentifierLength + 1U, 'a'), "asset"),
                  mc::ErrorCode::InvalidArgument);
    MC_CHECK_CODE(mc::validate_identifier("asset 01", "asset"), mc::ErrorCode::InvalidArgument);
    const mc::Status too_long = mc::validate_identifier(std::string(129U, 'a'), "asset");
    MC_CHECK_EQ(std::string(too_long.subject()), std::string("asset"));
    MC_CHECK_EQ(std::string(too_long.message()), std::string("identifier is too long"));

    // Ident<T>::parse keeps the error model.
    auto parsed = mc::AssetId::parse("asset_01");
    MC_REQUIRE(parsed.ok());
    MC_CHECK_EQ(parsed.value().name(), std::string("asset_01"));
    MC_CHECK_EQ(parsed.value().view(), std::string_view("asset_01"));
    MC_CHECK(!parsed.value().empty());
    MC_CHECK(parsed.value() == *parsed);

    auto rejected = mc::AssetId::parse("asset 01");
    MC_CHECK(!rejected.ok());
    MC_CHECK_CODE(rejected.status(), mc::ErrorCode::InvalidArgument);
    MC_CHECK_EQ(std::string(rejected.status().subject()), std::string("asset"));
    MC_CHECK(mc::AssetId{}.empty());
    MC_CHECK(mc::AssetId{} != parsed.value());
    MC_CHECK(mc::AssetId::parse("asset_01").value() == parsed.value());
    MC_CHECK(mc::AssetId{} < parsed.value());

    // Generations: unset is its own state, never generation zero.
    const mc::LifecycleGeneration unset;
    MC_CHECK(!unset.is_set());
    MC_CHECK(unset != mc::LifecycleGeneration::from_value(0U));
    MC_CHECK(unset < mc::LifecycleGeneration::from_value(0U));
    std::uint64_t raw = 12345U;
    MC_CHECK(!unset.try_value(raw));
    MC_CHECK_EQ(raw, 12345U);
    const mc::LifecycleGeneration seven = mc::LifecycleGeneration::from_value(7U);
    MC_CHECK(seven.is_set());
    MC_CHECK(seven.try_value(raw));
    MC_CHECK_EQ(raw, 7U);
    MC_CHECK_EQ(seven.value(), 7U);
    MC_CHECK_EQ(seven.next().value(), 8U);
    MC_CHECK(unset < seven);
    MC_CHECK(!(seven < unset));
    MC_CHECK(seven == mc::LifecycleGeneration::from_value(7U));
}

// ---------------------------------------------------------------------------
// Canonical writer / reader
// ---------------------------------------------------------------------------

MC_TEST(canonical_writer_reader_scalar_round_trip) {
    const std::vector<std::uint8_t> raw_bytes{0x00U, 0xFFU, 0x7FU};
    const mc::Digest digest = mc::digest_of(mc::purpose::kPolicy, std::string_view("body"));
    auto ident = mc::AssetId::parse("asset-01");
    MC_REQUIRE(ident.ok());

    mc::Writer writer;
    writer.u8(0x12U);
    writer.u16(0x3456U);
    writer.u32(0x89ABCDEFU);
    writer.u64(0x0123456789ABCDEFULL);
    writer.i64(-1234567890123LL);
    writer.boolean(true);
    writer.boolean(false);
    writer.bytes(raw_bytes.data(), raw_bytes.size());
    writer.blob(std::string_view("hello"));
    writer.text(std::string_view(""));
    writer.digest(digest);
    writer.generation(mc::LifecycleGeneration::from_value(42U));
    writer.generation(mc::LifecycleGeneration{});
    writer.ident(ident.value());

    // Fixed width encodings with no padding.
    const std::size_t expected_size = 1U + 2U + 4U + 8U + 8U + 1U + 1U + 3U + (4U + 5U) + 4U + 32U +
                                      (1U + 8U) + (1U + 8U) + (4U + 8U);
    MC_CHECK_EQ(writer.size(), expected_size);
    MC_CHECK_EQ(writer.data().size(), expected_size);

    mc::Reader reader(writer.data());
    std::uint8_t u8 = 0U;
    std::uint16_t u16 = 0U;
    std::uint32_t u32 = 0U;
    std::uint64_t u64 = 0U;
    std::int64_t i64 = 0;
    bool first = false;
    bool second = true;
    MC_REQUIRE_OK(reader.u8(u8));
    MC_REQUIRE_OK(reader.u16(u16));
    MC_REQUIRE_OK(reader.u32(u32));
    MC_REQUIRE_OK(reader.u64(u64));
    MC_REQUIRE_OK(reader.i64(i64));
    MC_REQUIRE_OK(reader.boolean(first));
    MC_REQUIRE_OK(reader.boolean(second));
    std::vector<std::uint8_t> decoded_raw(3U);
    MC_REQUIRE_OK(reader.bytes(decoded_raw.data(), decoded_raw.size()));
    std::string blob;
    MC_REQUIRE_OK(reader.blob(blob));
    std::string empty("sentinel");
    MC_REQUIRE_OK(reader.text(empty));
    mc::Digest decoded_digest;
    MC_REQUIRE_OK(reader.digest(decoded_digest));
    mc::LifecycleGeneration set_generation;
    MC_REQUIRE_OK(reader.generation(set_generation));
    mc::LifecycleGeneration unset_generation;
    MC_REQUIRE_OK(reader.generation(unset_generation));
    mc::AssetId decoded_ident;
    MC_REQUIRE_OK(reader.ident(decoded_ident));

    MC_CHECK(reader.fully_consumed());
    MC_CHECK_EQ(reader.remaining(), 0U);
    MC_CHECK_EQ(reader.offset(), writer.size());

    MC_CHECK_EQ(u8, 0x12U);
    MC_CHECK_EQ(u16, 0x3456U);
    MC_CHECK_EQ(u32, 0x89ABCDEFU);
    MC_CHECK_EQ(u64, 0x0123456789ABCDEFULL);
    MC_CHECK_EQ(i64, -1234567890123LL);
    MC_CHECK(first);
    MC_CHECK(!second);
    MC_CHECK(decoded_raw == raw_bytes);
    MC_CHECK_EQ(blob, std::string("hello"));
    MC_CHECK(empty.empty());
    MC_CHECK(decoded_digest == digest);
    MC_CHECK(set_generation == mc::LifecycleGeneration::from_value(42U));
    MC_CHECK(!unset_generation.is_set());
    MC_CHECK(decoded_ident == ident.value());

    // The reader also accepts a raw pointer and length.
    mc::Reader viewed(writer.data().data(), writer.size());
    std::uint8_t peek = 0U;
    MC_REQUIRE_OK(viewed.u8(peek));
    MC_CHECK_EQ(peek, 0x12U);
    MC_CHECK(!viewed.fully_consumed());

    // Writer::clear() resets it and take() moves the bytes out.
    mc::Writer scratch;
    scratch.u32(5U);
    scratch.clear();
    MC_CHECK_EQ(scratch.size(), 0U);
    scratch.u8(9U);
    const std::vector<std::uint8_t> taken = std::move(scratch).take();
    MC_CHECK_EQ(taken.size(), 1U);
    MC_CHECK_EQ(taken[0], 0x09U);
}

MC_TEST(canonical_reader_reports_record_truncated) {
    MC_CHECK_CODE(read_u16_with(0U), mc::ErrorCode::RecordTruncated);
    MC_CHECK_CODE(read_u16_with(1U), mc::ErrorCode::RecordTruncated);
    MC_CHECK(read_u16_with(2U).ok());
    MC_CHECK_CODE(read_u32_with(0U), mc::ErrorCode::RecordTruncated);
    MC_CHECK_CODE(read_u32_with(3U), mc::ErrorCode::RecordTruncated);
    MC_CHECK(read_u32_with(4U).ok());
    MC_CHECK_CODE(read_u64_with(7U), mc::ErrorCode::RecordTruncated);
    MC_CHECK(read_u64_with(8U).ok());
    MC_CHECK_CODE(read_i64_with(7U), mc::ErrorCode::RecordTruncated);
    MC_CHECK(read_i64_with(8U).ok());
    MC_CHECK_CODE(read_digest_with(31U), mc::ErrorCode::RecordTruncated);
    MC_CHECK(read_digest_with(32U).ok());

    // u8 over an empty buffer.
    const std::vector<std::uint8_t> empty;
    mc::Reader empty_reader(empty);
    std::uint8_t byte = 0U;
    MC_CHECK_CODE(empty_reader.u8(byte), mc::ErrorCode::RecordTruncated);
    MC_CHECK_EQ(empty_reader.remaining(), 0U);
    MC_CHECK(empty_reader.fully_consumed());

    // A failed read never consumes anything.
    const std::vector<std::uint8_t> two_bytes{0x01U, 0x02U};
    mc::Reader partial(two_bytes);
    std::uint32_t wide = 0U;
    MC_CHECK_CODE(partial.u32(wide), mc::ErrorCode::RecordTruncated);
    MC_CHECK_EQ(partial.offset(), 0U);
    MC_CHECK_EQ(partial.remaining(), 2U);
    MC_CHECK(!partial.fully_consumed());
    const mc::Status truncated = partial.u32(wide);
    MC_CHECK_EQ(std::string(truncated.subject()), std::string("u32"));
    MC_CHECK(std::string(truncated.detail()).find("need 4 bytes, have 2") != std::string::npos);

    // A blob whose header claims more bytes than are present.
    MC_CHECK_CODE(read_blob_declaring(10U, 5U), mc::ErrorCode::RecordTruncated);
    MC_CHECK(read_blob_declaring(0U, 0U).ok());
    MC_CHECK(read_blob_declaring(5U, 5U).ok());
    MC_CHECK(read_blob_declaring(5U, 6U).ok());  // trailing bytes are the caller's problem
    MC_CHECK_CODE(read_blob_declaring(0xFFFFFFFFU, 0U), mc::ErrorCode::LimitExceeded);

    // Exactly at the canonical blob limit the length is acceptable and the
    // failure is truncation; one byte beyond it is a limit violation.
    const auto limit = static_cast<std::uint32_t>(mc::kCanonicalMaxBlob);
    MC_CHECK_CODE(read_blob_declaring(limit, 0U), mc::ErrorCode::RecordTruncated);
    MC_CHECK_CODE(read_blob_declaring(limit + 1U, 0U), mc::ErrorCode::LimitExceeded);
}

MC_TEST(canonical_reader_rejects_reserved_boolean_and_oversized_blob) {
    const std::array<std::uint8_t, 4> reserved{{2U, 3U, 0x7FU, 0xFFU}};
    for (const std::uint8_t raw : reserved) {
        const std::vector<std::uint8_t> buffer{raw};
        mc::Reader reader(buffer);
        bool value = true;
        MC_CHECK_CODE(reader.boolean(value), mc::ErrorCode::ReservedFieldViolation);
        MC_CHECK_EQ(reader.offset(), 1U);
        MC_CHECK(value);  // the output is untouched on failure
    }
    MC_CHECK_CODE(read_blob_declaring(0xFFFFFFFFU, 0U), mc::ErrorCode::LimitExceeded);
    MC_CHECK_CODE(read_blob_declaring(0x80000000U, 0U), mc::ErrorCode::LimitExceeded);

    const std::vector<std::uint8_t> valid{0x00U, 0x01U};
    mc::Reader reader(valid);
    bool value = true;
    MC_REQUIRE_OK(reader.boolean(value));
    MC_CHECK(!value);
    MC_REQUIRE_OK(reader.boolean(value));
    MC_CHECK(value);
    MC_CHECK(reader.fully_consumed());
}

MC_TEST(reader_fully_consumed_and_trailing_bytes) {
    const std::vector<std::uint8_t> exact{0x78U, 0x56U, 0x34U, 0x12U};
    std::uint32_t out = 0U;
    MC_REQUIRE_OK(parse_u32_record(exact, out));
    MC_CHECK_EQ(out, 0x12345678U);

    std::vector<std::uint8_t> trailing = exact;
    trailing.push_back(0x00U);
    MC_CHECK_CODE(parse_u32_record(trailing, out), mc::ErrorCode::TrailingBytes);
    MC_CHECK_EQ(out, 0x12345678U);

    mc::Reader reader(trailing);
    std::uint32_t raw = 0U;
    MC_REQUIRE_OK(reader.u32(raw));
    MC_CHECK(!reader.fully_consumed());
    MC_CHECK_EQ(reader.remaining(), 1U);
    MC_CHECK_EQ(reader.offset(), 4U);
    std::uint8_t last = 0U;
    MC_REQUIRE_OK(reader.u8(last));
    MC_CHECK_EQ(last, 0x00U);
    MC_CHECK(reader.fully_consumed());

    std::vector<std::uint8_t> short_record{0x01U};
    MC_CHECK_CODE(parse_u32_record(short_record, out), mc::ErrorCode::RecordTruncated);
}

// ---------------------------------------------------------------------------
// Error model
// ---------------------------------------------------------------------------

MC_TEST(error_code_text_round_trip) {
    const std::vector<std::pair<mc::ErrorCode, const char*>> codes{
        {mc::ErrorCode::InvalidUsage, "InvalidUsage"},
        {mc::ErrorCode::UnknownCommand, "UnknownCommand"},
        {mc::ErrorCode::MissingArgument, "MissingArgument"},
        {mc::ErrorCode::InvalidArgument, "InvalidArgument"},
        {mc::ErrorCode::MalformedInput, "MalformedInput"},
        {mc::ErrorCode::UnsupportedFormatVersion, "UnsupportedFormatVersion"},
        {mc::ErrorCode::UnknownPlan, "UnknownPlan"},
        {mc::ErrorCode::UnknownTarget, "UnknownTarget"},
        {mc::ErrorCode::DuplicateIdentity, "DuplicateIdentity"},
        {mc::ErrorCode::IdentityMismatch, "IdentityMismatch"},
        {mc::ErrorCode::UnknownObligation, "UnknownObligation"},
        {mc::ErrorCode::UnknownAsset, "UnknownAsset"},
        {mc::ErrorCode::StaleRevision, "StaleRevision"},
        {mc::ErrorCode::StaleAuthority, "StaleAuthority"},
        {mc::ErrorCode::StalePolicyGeneration, "StalePolicyGeneration"},
        {mc::ErrorCode::StaleObservation, "StaleObservation"},
        {mc::ErrorCode::MissingGeneration, "MissingGeneration"},
        {mc::ErrorCode::StalePlanDigest, "StalePlanDigest"},
        {mc::ErrorCode::IllegalPhase, "IllegalPhase"},
        {mc::ErrorCode::AlreadyTerminal, "AlreadyTerminal"},
        {mc::ErrorCode::NotApproved, "NotApproved"},
        {mc::ErrorCode::NotReady, "NotReady"},
        {mc::ErrorCode::ObligationsOutstanding, "ObligationsOutstanding"},
        {mc::ErrorCode::ProtectedObligationViolated, "ProtectedObligationViolated"},
        {mc::ErrorCode::RecoveryRequired, "RecoveryRequired"},
        {mc::ErrorCode::ProgressOutOfOrder, "ProgressOutOfOrder"},
        {mc::ErrorCode::ProtectedWindowActive, "ProtectedWindowActive"},
        {mc::ErrorCode::BlackoutPeriodActive, "BlackoutPeriodActive"},
        {mc::ErrorCode::RedundancyInsufficient, "RedundancyInsufficient"},
        {mc::ErrorCode::ActiveIncident, "ActiveIncident"},
        {mc::ErrorCode::LifecycleStateInvalid, "LifecycleStateInvalid"},
        {mc::ErrorCode::HardInterlock, "HardInterlock"},
        {mc::ErrorCode::ExceptionExpired, "ExceptionExpired"},
        {mc::ErrorCode::ScopeViolation, "ScopeViolation"},
        {mc::ErrorCode::IoError, "IoError"},
        {mc::ErrorCode::RecordCorrupt, "RecordCorrupt"},
        {mc::ErrorCode::RecordTruncated, "RecordTruncated"},
        {mc::ErrorCode::ChecksumMismatch, "ChecksumMismatch"},
        {mc::ErrorCode::TrailingBytes, "TrailingBytes"},
        {mc::ErrorCode::ReservedFieldViolation, "ReservedFieldViolation"},
        {mc::ErrorCode::LayoutInvalid, "LayoutInvalid"},
        {mc::ErrorCode::LimitExceeded, "LimitExceeded"},
        {mc::ErrorCode::DuplicateOperation, "DuplicateOperation"},
        {mc::ErrorCode::ReplayIntentMismatch, "ReplayIntentMismatch"},
        {mc::ErrorCode::InternalError, "InternalError"},
    };
    MC_CHECK(codes.size() >= 30U);

    for (const auto& entry : codes) {
        const mc::ErrorCode code = entry.first;
        const std::string name(entry.second);
        MC_CHECK_EQ(std::string(mc::to_string(code)), name);
        mc::ErrorCode parsed = mc::ErrorCode::Ok;
        MC_CHECK_MSG(mc::parse_error_code(name, parsed), "could not parse " + name);
        MC_CHECK(parsed == code);
        MC_CHECK(!mc::is_waivable_condition(code) || mc::is_precondition_code(code));
    }

    // Spellings and numeric values are unique.
    for (std::size_t left = 0; left < codes.size(); ++left) {
        for (std::size_t right = left + 1U; right < codes.size(); ++right) {
            MC_CHECK(codes[left].first != codes[right].first);
            MC_CHECK(std::string_view(codes[left].second) != std::string_view(codes[right].second));
        }
    }

    MC_CHECK_EQ(std::string(mc::to_string(mc::ErrorCode::Ok)), std::string("ok"));
    mc::ErrorCode parsed = mc::ErrorCode::InternalError;
    MC_CHECK(!mc::parse_error_code("ok", parsed));
    MC_CHECK(!mc::parse_error_code("", parsed));
    MC_CHECK(!mc::parse_error_code("OK", parsed));
    MC_CHECK(!mc::parse_error_code("invalidusage", parsed));
    MC_CHECK(!mc::parse_error_code("NotARealCode", parsed));
    MC_CHECK(parsed == mc::ErrorCode::InternalError);
    MC_CHECK_EQ(std::string(mc::to_string(static_cast<mc::ErrorCode>(9999U))), std::string("UnknownErrorCode"));
    MC_CHECK_EQ(std::string(mc::to_string(static_cast<mc::ErrorCode>(17U))), std::string("UnknownErrorCode"));
}

MC_TEST(error_code_precedence_and_class_boundaries) {
    const auto at = [](std::uint16_t raw) { return static_cast<mc::ErrorCode>(raw); };

    MC_CHECK_EQ(mc::primary_of(mc::ErrorCode::InvalidArgument, mc::ErrorCode::StaleRevision),
                mc::ErrorCode::InvalidArgument);
    MC_CHECK_EQ(mc::primary_of(mc::ErrorCode::StaleRevision, mc::ErrorCode::InvalidArgument),
                mc::ErrorCode::InvalidArgument);
    MC_CHECK_EQ(mc::primary_of(mc::ErrorCode::Ok, mc::ErrorCode::InternalError), mc::ErrorCode::Ok);
    MC_CHECK_EQ(mc::primary_of(mc::ErrorCode::InternalError, mc::ErrorCode::Ok), mc::ErrorCode::Ok);
    MC_CHECK_EQ(mc::primary_of(mc::ErrorCode::LimitExceeded, mc::ErrorCode::LimitExceeded),
                mc::ErrorCode::LimitExceeded);
    MC_CHECK_EQ(mc::primary_of(mc::ErrorCode::LayoutInvalid, mc::ErrorCode::HardInterlock),
                mc::ErrorCode::HardInterlock);
    MC_CHECK_EQ(mc::primary_of(mc::ErrorCode::UnknownAsset, mc::ErrorCode::LifecycleStateInvalid),
                mc::ErrorCode::UnknownAsset);

    // Class ranges: [start, end).
    MC_CHECK(mc::is_precondition_code(at(500U)));
    MC_CHECK(mc::is_precondition_code(mc::ErrorCode::HardInterlock));
    MC_CHECK(mc::is_precondition_code(at(599U)));
    MC_CHECK(!mc::is_precondition_code(at(499U)));
    MC_CHECK(!mc::is_precondition_code(at(600U)));
    MC_CHECK(!mc::is_precondition_code(mc::ErrorCode::Ok));

    MC_CHECK(mc::is_integrity_code(at(600U)));
    MC_CHECK(mc::is_integrity_code(mc::ErrorCode::TrailingBytes));
    MC_CHECK(mc::is_integrity_code(at(699U)));
    MC_CHECK(!mc::is_integrity_code(at(599U)));
    MC_CHECK(!mc::is_integrity_code(at(700U)));

    MC_CHECK(mc::is_authority_code(at(300U)));
    MC_CHECK(mc::is_authority_code(mc::ErrorCode::MissingGeneration));
    MC_CHECK(mc::is_authority_code(at(399U)));
    MC_CHECK(!mc::is_authority_code(at(299U)));
    MC_CHECK(!mc::is_authority_code(at(400U)));

    MC_CHECK(mc::is_lifecycle_code(at(400U)));
    MC_CHECK(mc::is_lifecycle_code(mc::ErrorCode::IllegalPhase));
    MC_CHECK(mc::is_lifecycle_code(at(499U)));
    MC_CHECK(!mc::is_lifecycle_code(at(399U)));
    MC_CHECK(!mc::is_lifecycle_code(at(500U)));

    // The four classes are disjoint and never true for Ok.
    MC_CHECK(!mc::is_authority_code(mc::ErrorCode::Ok));
    MC_CHECK(!mc::is_lifecycle_code(mc::ErrorCode::Ok));
    MC_CHECK(!mc::is_precondition_code(mc::ErrorCode::Ok));
    MC_CHECK(!mc::is_integrity_code(mc::ErrorCode::Ok));

    // Waivable conditions are soft policy conditions only.
    MC_CHECK(mc::is_waivable_condition(mc::ErrorCode::ProtectedWindowActive));
    MC_CHECK(mc::is_waivable_condition(mc::ErrorCode::BlackoutPeriodActive));
    MC_CHECK(mc::is_waivable_condition(mc::ErrorCode::RedundancyInsufficient));
    MC_CHECK(mc::is_waivable_condition(mc::ErrorCode::SpareCapacityInsufficient));
    MC_CHECK(mc::is_waivable_condition(mc::ErrorCode::PowerHeadroomInsufficient));
    MC_CHECK(mc::is_waivable_condition(mc::ErrorCode::CoolingHeadroomInsufficient));
    MC_CHECK(mc::is_waivable_condition(mc::ErrorCode::ActiveIncident));
    MC_CHECK(mc::is_waivable_condition(mc::ErrorCode::ServiceClassViolation));
    MC_CHECK(mc::is_waivable_condition(mc::ErrorCode::PersonnelEvidenceMissing));
    MC_CHECK(mc::is_waivable_condition(mc::ErrorCode::ApprovalEvidenceMissing));
    MC_CHECK(!mc::is_waivable_condition(mc::ErrorCode::HardInterlock));
    MC_CHECK(!mc::is_waivable_condition(mc::ErrorCode::ProtectedObligationViolated));
    MC_CHECK(!mc::is_waivable_condition(mc::ErrorCode::LifecycleStateInvalid));
    MC_CHECK(!mc::is_waivable_condition(mc::ErrorCode::ConcurrentMaintenanceConflict));
    MC_CHECK(!mc::is_waivable_condition(mc::ErrorCode::InvalidArgument));
    MC_CHECK(!mc::is_waivable_condition(mc::ErrorCode::Ok));
}

MC_TEST(status_merge_keeps_the_lowest_code) {
    mc::Status status = mc::fail(mc::ErrorCode::StaleRevision, "plan is stale", "plan-1", "rev 4");
    MC_CHECK(!status.ok());
    MC_CHECK(status.code() == mc::ErrorCode::StaleRevision);
    MC_CHECK(status.suppressed().empty());
    MC_CHECK_EQ(std::string(status.message()), std::string("plan is stale"));
    MC_CHECK_EQ(std::string(status.subject()), std::string("plan-1"));
    MC_CHECK_EQ(std::string(status.detail()), std::string("rev 4"));

    // Merging success changes nothing.
    status.merge(mc::Status::success());
    MC_CHECK_EQ(status.code(), mc::ErrorCode::StaleRevision);
    MC_CHECK(status.suppressed().empty());

    // A higher numeric code never displaces the primary; it is recorded.
    status.merge(mc::fail(mc::ErrorCode::StalePlanDigest, "digest moved", "plan-1"));
    MC_CHECK_EQ(status.code(), mc::ErrorCode::StaleRevision);
    MC_CHECK_EQ(status.suppressed().size(), 1U);
    MC_REQUIRE(!status.suppressed().empty());
    MC_CHECK_EQ(status.suppressed()[0].code(), mc::ErrorCode::StalePlanDigest);

    // A lower numeric code is promoted to primary; the outranked faults are
    // kept, in suppression order.
    status.merge(mc::fail(mc::ErrorCode::InvalidArgument, "plan names no target", "plan-1"));
    MC_CHECK_EQ(status.code(), mc::ErrorCode::InvalidArgument);
    MC_CHECK_EQ(status.suppressed().size(), 2U);
    MC_REQUIRE(status.suppressed().size() == 2U);
    MC_CHECK_EQ(status.suppressed()[0].code(), mc::ErrorCode::StalePlanDigest);
    MC_CHECK_EQ(status.suppressed()[1].code(), mc::ErrorCode::StaleRevision);
    MC_CHECK_EQ(std::string(status.suppressed()[1].detail()), std::string("rev 4"));

    // A higher code is only recorded as suppressed.
    status.merge(mc::fail(mc::ErrorCode::ChecksumMismatch, "checksum", "record-9"));
    MC_CHECK_EQ(status.code(), mc::ErrorCode::InvalidArgument);
    MC_CHECK_EQ(status.suppressed().size(), 3U);
    MC_REQUIRE(!status.suppressed().empty());
    MC_CHECK_EQ(status.suppressed().back().code(), mc::ErrorCode::ChecksumMismatch);

    // Identical faults collapse instead of piling up.
    mc::Status identical = mc::fail(mc::ErrorCode::InvalidArgument, "bad", "x", "y");
    identical.merge(mc::fail(mc::ErrorCode::InvalidArgument, "bad", "x", "y"));
    MC_CHECK(identical.suppressed().empty());
    identical.merge(mc::fail(mc::ErrorCode::InvalidArgument, "bad", "x", "different"));
    MC_CHECK_EQ(identical.suppressed().size(), 1U);

    // Success adopts the first failure it merges.
    mc::Status empty;
    MC_CHECK(empty.ok());
    MC_CHECK(empty.suppressed().empty());
    empty.merge(mc::fail(mc::ErrorCode::LimitExceeded, "too big", "blob"));
    MC_CHECK(!empty.ok());
    MC_CHECK_EQ(empty.code(), mc::ErrorCode::LimitExceeded);

    // fail() never yields success, and success() is the only ok status.
    MC_CHECK_EQ(mc::fail(mc::ErrorCode::Ok, "oops").code(), mc::ErrorCode::InternalError);
    MC_CHECK(mc::Status::success().ok());
    MC_CHECK(mc::Status::success().suppressed().empty());
    MC_CHECK(!mc::Status(mc::ErrorCode::InvalidArgument, "message").ok());
}

MC_TEST(status_render_names_the_code_and_subject) {
    const mc::Status status =
        mc::fail(mc::ErrorCode::ProtectedObligationViolated, "protected obligation would be violated", "po-a",
                 "requires 1 unit");
    const std::string rendered = status.render();
    MC_CHECK(rendered.find("ProtectedObligationViolated") != std::string::npos);
    MC_CHECK(rendered.find("protected obligation would be violated") != std::string::npos);
    MC_CHECK(rendered.find("[po-a]") != std::string::npos);
    MC_CHECK(rendered.find("(requires 1 unit)") != std::string::npos);
    MC_CHECK_EQ(mc::test::display(status), rendered);

    mc::Status merged = status;
    merged.merge(mc::fail(mc::ErrorCode::RedundancyInsufficient, "not enough units", "rg-a"));
    const std::string merged_text = merged.render();
    MC_CHECK(merged_text.find("suppressed:") != std::string::npos);
    MC_CHECK(merged_text.find("RedundancyInsufficient") != std::string::npos);
    MC_CHECK(merged_text.find("ProtectedObligationViolated") != std::string::npos);

    MC_CHECK_EQ(mc::fail(mc::ErrorCode::IoError, "disk").render(), std::string("IoError: disk"));
    MC_CHECK_EQ(mc::Status::success().render(), std::string("ok"));
    MC_CHECK_EQ(mc::fail(mc::ErrorCode::IoError, "disk", "store-1").render(),
                std::string("IoError: disk [store-1]"));

    // Result<T> never carries an ok status as a failure.
    const mc::Result<int> value(7);
    MC_CHECK(value.ok());
    MC_CHECK_EQ(value.value(), 7);
    const mc::Result<int> failed(mc::Status::success());
    MC_CHECK(!failed.ok());
    MC_CHECK_EQ(failed.status().code(), mc::ErrorCode::InternalError);
    const mc::Result<std::string> carried(std::string("payload"));
    MC_CHECK(carried.ok());
    MC_CHECK_EQ(carried.value(), std::string("payload"));
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

MC_TEST(json_parses_values_strings_and_escapes) {
    const std::string document =
        "{\"int\":1,\"negative\":-2,\"big\":9223372036854775807,\"zero\":0,\"yes\":true,\"no\":false,"
        "\"nothing\":null,\"array\":[1,2,3],\"nested\":{\"inner\":\"x\"},"
        "\"escapes\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\",\"accent\":\"\\u00e9\",\"emoji\":\"\\uD83D\\uDE00\","
        "\"empty-array\":[],\"empty-object\":{}}";
    auto parsed = mc::json::parse(document);
    MC_REQUIRE(parsed.ok());
    const mc::json::Value& value = parsed.value();
    MC_REQUIRE(value.is_object());
    MC_CHECK_EQ(value.size(), 14U);
    MC_CHECK(value.has("int"));
    MC_CHECK(!value.has("missing"));
    MC_CHECK(value.find("missing") == nullptr);

    MC_REQUIRE(value.find("int") != nullptr);
    MC_CHECK(value.find("int")->is_int());
    MC_CHECK_EQ(*value.find("int")->as_int(), std::int64_t{1});
    MC_CHECK_EQ(*value.find("negative")->as_int(), std::int64_t{-2});
    MC_CHECK_EQ(*value.find("big")->as_int(), std::int64_t{9223372036854775807LL});
    MC_CHECK_EQ(*value.find("zero")->as_int(), std::int64_t{0});
    MC_CHECK(*value.find("yes")->as_bool());
    MC_CHECK(!*value.find("no")->as_bool());
    MC_CHECK(value.find("nothing")->is_null());
    MC_CHECK(!value.find("nothing")->is_int());
    MC_CHECK(value.find("nothing")->as_int() == nullptr);
    MC_CHECK(value.find("int")->as_string() == nullptr);

    MC_CHECK_EQ(*value.find("escapes")->as_string(), std::string("\"\\/\b\f\n\r\t"));
    MC_CHECK_EQ(*value.find("accent")->as_string(), std::string("\xC3\xA9"));
    MC_CHECK_EQ(*value.find("emoji")->as_string(), std::string("\xF0\x9F\x98\x80"));
    MC_CHECK_EQ(value.find("nested")->find("inner")->as_string()->size(), 1U);

    const mc::json::Value* array = value.find("array");
    MC_REQUIRE(array != nullptr);
    MC_REQUIRE(array->is_array());
    MC_CHECK_EQ(array->size(), 3U);
    MC_CHECK_EQ(*array->as_array()->at(2U).as_int(), std::int64_t{3});
    MC_CHECK_EQ(value.find("empty-array")->size(), 0U);
    MC_CHECK_EQ(value.find("empty-object")->size(), 0U);

    // Whitespace is insignificant.
    auto spaced = mc::json::parse("  \t\r\n { \"a\" : [ 1 , 2 ] } \n");
    MC_REQUIRE(spaced.ok());
    MC_CHECK_EQ(spaced.value().size(), 1U);
    MC_CHECK_EQ(spaced.value().find("a")->size(), 2U);

    // dump() re-parses to an equal document, compact and indented.
    auto reparsed = mc::json::parse(value.dump());
    MC_REQUIRE(reparsed.ok());
    MC_CHECK(json_equal(value, reparsed.value()));
    MC_CHECK_EQ(reparsed.value().dump(), value.dump());
    auto pretty = mc::json::parse(value.dump(2U));
    MC_REQUIRE(pretty.ok());
    MC_CHECK(json_equal(value, pretty.value()));
    MC_CHECK(pretty.value().dump(2U).find('\n') != std::string::npos);

    // The value model itself: insertion order is preserved and re-setting a key
    // replaces it in place.
    mc::json::Value built = mc::json::Value::object();
    built.set("b", mc::json::Value(2));
    built.set("a", mc::json::Value(1));
    built.set("b", mc::json::Value(3));
    MC_CHECK_EQ(built.size(), 2U);
    MC_CHECK_EQ(built.dump(), std::string("{\"b\": 3,\"a\": 1}"));
    mc::json::Value pushed = mc::json::Value::array();
    pushed.push(mc::json::Value(nullptr));
    pushed.push(mc::json::Value("x"));
    MC_CHECK_EQ(pushed.dump(), std::string("[null,\"x\"]"));
    MC_CHECK_EQ(mc::json::Value().dump(), std::string("null"));
    MC_CHECK_EQ(mc::json::Value(true).dump(), std::string("true"));
    MC_CHECK_EQ(mc::json::Value(false).dump(), std::string("false"));

    // escape()/is_valid_utf8() are the inverse/side conditions of parsing.
    MC_CHECK_EQ(mc::json::escape("\n\"\\"), std::string("\\n\\\"\\\\"));
    MC_CHECK_EQ(mc::json::escape(std::string("\x01")), std::string("\\u0001"));
    MC_CHECK(mc::json::is_valid_utf8("plain ascii"));
    MC_CHECK(mc::json::is_valid_utf8("\xC3\xA9"));
    MC_CHECK(mc::json::is_valid_utf8("\xF0\x9F\x98\x80"));
    MC_CHECK(mc::json::is_valid_utf8(""));
    MC_CHECK(!mc::json::is_valid_utf8("\xC3"));
    MC_CHECK(!mc::json::is_valid_utf8("\x80"));
    MC_CHECK(!mc::json::is_valid_utf8("\xC0\x80"));
    MC_CHECK(!mc::json::is_valid_utf8("\xED\xA0\x80"));
    MC_CHECK(!mc::json::is_valid_utf8("\xF5\x80\x80\x80"));
}

MC_TEST(json_rejects_malformed_input) {
    const char* const malformed[] = {
        "",                                  // empty document
        "{",                                 // unterminated object
        "}",                                 // stray closing brace
        "[1,]",                              // trailing comma in an array
        "{\"a\":1,}",                        // trailing comma in an object
        "{\"a\"} ",                          // member without a value
        "{\"a\":1 \"b\":2}",                 // missing comma
        "{\"a\":1, \"a\":2}",                // duplicate member
        "01",                                // leading zero
        "-",                                 // sign without digits
        "1.5",                               // fractional number
        "1e3",                               // exponent
        "tru",                               // truncated literal
        "nul",                               // truncated literal
        "\"unterminated",                     // unterminated string
        "\"\\q\"",                            // unknown escape
        "\"\\u12\"",                          // truncated escape
        "\"\\uD800\"",                        // lone high surrogate
        "\"\\uDC00\"",                        // lone low surrogate
        "\"a\nb\"",                           // raw control character
        "[]]",                               // trailing content
        "{} {}",                             // trailing content
    };
    for (const char* text : malformed) {
        auto parsed = mc::json::parse(text, "malformed");
        MC_CHECK_MSG(!parsed.ok(), std::string("accepted malformed input: ") + text);
    }

    // A document nested deeper than the accepted bound is a limit failure, not
    // a syntax failure.
    std::string deep;
    for (int index = 0; index < 70; ++index) {
        deep.push_back('[');
    }
    auto too_deep = mc::json::parse(deep, "deep");
    MC_CHECK(!too_deep.ok());
    MC_CHECK(too_deep.status().code() == mc::ErrorCode::LimitExceeded);
}

MC_TEST(timestamp_format_and_parse_round_trip) {
    mc::Timestamp epoch = 0;
    MC_CHECK_EQ(mc::kNoTimestamp, (std::numeric_limits<std::int64_t>::min)());
    MC_CHECK(!mc::is_set(mc::kNoTimestamp));
    MC_CHECK(mc::is_set(0));
    MC_CHECK_EQ(mc::format_timestamp(mc::kNoTimestamp), std::string("unset"));
    MC_CHECK_EQ(mc::format_timestamp(0), std::string("1970-01-01T00:00:00Z"));
    MC_CHECK(mc::parse_timestamp("1970-01-01T00:00:00Z", epoch));
    MC_CHECK_EQ(epoch, 0LL);
    MC_CHECK(mc::is_set(epoch));

    const mc::Timestamp base = 1700000000LL * mc::kNanosPerSecond;  // 2023-11-14T22:13:20Z
    MC_CHECK_EQ(mc::format_timestamp(base), std::string("2023-11-14T22:13:20Z"));
    MC_CHECK_EQ(mc::format_timestamp(base + 123456789LL), std::string("2023-11-14T22:13:20.123456789Z"));
    MC_CHECK_EQ(mc::format_timestamp(-86400LL * mc::kNanosPerSecond), std::string("1969-12-31T00:00:00Z"));

    const std::array<mc::Timestamp, 7> instants{{
        1LL,
        999999999LL,
        1000000000LL,
        base,
        base + 123456789LL,
        -86400LL * mc::kNanosPerSecond,
        -365LL * mc::kNanosPerDay,
    }};
    for (const mc::Timestamp instant : instants) {
        const std::string text = mc::format_timestamp(instant);
        mc::Timestamp parsed = 0;
        MC_CHECK_MSG(mc::parse_timestamp(text, parsed), "could not parse " + text);
        MC_CHECK_EQ(parsed, instant);
        MC_CHECK_EQ(mc::format_timestamp(parsed), text);
    }

    // A nine digit fraction is the widest accepted spelling.
    mc::Timestamp parsed = 0;
    MC_CHECK(mc::parse_timestamp("2026-06-15T12:30:45.123456789Z", parsed));
    MC_CHECK_EQ(parsed, 1781526645LL * mc::kNanosPerSecond + 123456789LL);
    MC_CHECK_EQ(mc::format_timestamp(parsed), std::string("2026-06-15T12:30:45.123456789Z"));

    // A whole second never renders a zero fraction.
    MC_CHECK(mc::parse_timestamp("1970-01-01T00:00:00.000000000Z", parsed));
    MC_CHECK_EQ(parsed, 0LL);
    MC_CHECK(mc::parse_timestamp("2000-02-29T00:00:00Z", parsed));  // leap day
    MC_CHECK_EQ(mc::format_timestamp(parsed), std::string("2000-02-29T00:00:00Z"));

    // Instants before the epoch are rendered with floor division, so one
    // nanosecond before 1970 is the last nanosecond of 1969 and the rendering
    // always parses back to the instant it came from.
    MC_CHECK(mc::parse_timestamp("1969-12-31T23:59:59.999999999Z", parsed));
    MC_CHECK_EQ(parsed, -1LL);
    MC_CHECK_EQ(mc::format_timestamp(-1LL), std::string("1969-12-31T23:59:59.999999999Z"));
    MC_CHECK_EQ(mc::format_timestamp(-(mc::kNanosPerSecond + 1LL)), std::string("1969-12-31T23:59:58.999999999Z"));
    MC_CHECK_EQ(mc::format_timestamp(-86401LL * mc::kNanosPerSecond), std::string("1969-12-30T23:59:59Z"));
    for (const mc::Timestamp negative : {-1LL, -999999999LL, -1000000001LL, -86400LL * mc::kNanosPerSecond,
                                         -(mc::kNanosPerDay + 1LL), -1000LL * mc::kNanosPerDay}) {
        const std::string text = mc::format_timestamp(negative);
        mc::Timestamp round_tripped = 0;
        MC_CHECK_MSG(mc::parse_timestamp(text, round_tripped), "could not parse " + text);
        MC_CHECK_EQ(round_tripped, negative);
    }
}

MC_TEST(timestamp_parse_rejections) {
    mc::Timestamp out = 0;
    MC_CHECK(!mc::parse_timestamp("2026-02-30T00:00:00Z", out));   // February has no 30th
    MC_CHECK(!mc::parse_timestamp("2026-02-29T00:00:00Z", out));   // 2026 is not a leap year
    MC_CHECK(!mc::parse_timestamp("1900-02-29T00:00:00Z", out));   // 1900 is not a leap year
    MC_CHECK(mc::parse_timestamp("2024-02-29T00:00:00Z", out));    // 2024 is a leap year
    MC_CHECK(mc::parse_timestamp("2000-02-29T00:00:00Z", out));    // 2000 is a leap year
    MC_CHECK(!mc::parse_timestamp("2026-04-31T00:00:00Z", out));   // April has 30 days
    MC_CHECK(!mc::parse_timestamp("2026-01-01T00:00:60Z", out));   // leap seconds are not accepted
    MC_CHECK(!mc::parse_timestamp("2026-13-01T00:00:00Z", out));   // month 13
    MC_CHECK(!mc::parse_timestamp("2026-00-01T00:00:00Z", out));   // month 0
    MC_CHECK(!mc::parse_timestamp("2026-01-00T00:00:00Z", out));   // day 0
    MC_CHECK(!mc::parse_timestamp("2026-01-01T24:00:00Z", out));   // hour 24
    MC_CHECK(!mc::parse_timestamp("2026-01-01T00:60:00Z", out));   // minute 60
    MC_CHECK(!mc::parse_timestamp("2026-01-01T00:00:00", out));    // no zone designator
    MC_CHECK(!mc::parse_timestamp("2026-01-01 00:00:00Z", out));   // space instead of 'T'
    MC_CHECK(!mc::parse_timestamp("2026-01-01T00:00:00+01:00", out));  // offsets are not accepted
    MC_CHECK(!mc::parse_timestamp("2026-1-01T00:00:00Z", out));    // wrong width
    MC_CHECK(!mc::parse_timestamp("2026-01-01T00:00:00.Z", out));  // empty fraction
    MC_CHECK(!mc::parse_timestamp("2026-01-01T00:00:00.1234567891Z", out));  // fraction too wide
    MC_CHECK(!mc::parse_timestamp("", out));                       // empty
    MC_CHECK(!mc::parse_timestamp("not a timestamp", out));        // text
}

MC_TEST(manual_clock_is_explicit) {
    mc::ManualClock clock(1000);
    MC_CHECK_EQ(clock.now_nanos(), 1000LL);
    clock.advance(500);
    MC_CHECK_EQ(clock.now_nanos(), 1500LL);
    clock.set(0);  // the epoch is a real instant the clock may report
    MC_CHECK_EQ(clock.now_nanos(), 0LL);
    clock.set(mc::kNoTimestamp);
    MC_CHECK_EQ(clock.now_nanos(), mc::kNoTimestamp);

    mc::SystemClock system;
    MC_CHECK(system.now_nanos() > 0);
    const mc::Clock& as_clock = clock;
    MC_CHECK_EQ(as_clock.now_nanos(), mc::kNoTimestamp);
    MC_CHECK(!mc::is_set(mc::kNoTimestamp));
    MC_CHECK_EQ(mc::format_timestamp(mc::kNoTimestamp), std::string("unset"));
}

MC_TEST(json_rejections_carry_precise_codes) {
    check_json_rejected("{} {}", mc::ErrorCode::TrailingBytes, "trailing content after an object");
    check_json_rejected("1 2", mc::ErrorCode::TrailingBytes, "trailing content after a number");
    check_json_rejected("[]x", mc::ErrorCode::TrailingBytes, "trailing content after an array");
    check_json_rejected("null null", mc::ErrorCode::TrailingBytes, "trailing content after null");

    check_json_rejected("{\"a\":1,\"a\":2}", mc::ErrorCode::MalformedInput, "duplicate object key");
    check_json_rejected("{\"a\":{\"b\":1,\"b\":2}}", mc::ErrorCode::MalformedInput, "nested duplicate key");
    check_json_rejected("01", mc::ErrorCode::MalformedInput, "leading zero");
    check_json_rejected("[01]", mc::ErrorCode::MalformedInput, "leading zero in an array");
    check_json_rejected("1.5", mc::ErrorCode::MalformedInput, "fractional number");
    check_json_rejected("-0.5", mc::ErrorCode::MalformedInput, "negative fractional number");
    check_json_rejected("1e3", mc::ErrorCode::MalformedInput, "exponent number");
    check_json_rejected("\"a\nb\"", mc::ErrorCode::MalformedInput, "unescaped newline");
    check_json_rejected("\"a\tb\"", mc::ErrorCode::MalformedInput, "unescaped tab");
    check_json_rejected("\"\\q\"", mc::ErrorCode::MalformedInput, "unknown escape");
    check_json_rejected("\"\\u12\"", mc::ErrorCode::MalformedInput, "truncated \\u escape");
    check_json_rejected("\"\\uZZZZ\"", mc::ErrorCode::MalformedInput, "non hexadecimal \\u escape");
    check_json_rejected("\"\\uD83D\"", mc::ErrorCode::MalformedInput, "lone high surrogate");
    check_json_rejected("\"\\uD83D\\u0041\"", mc::ErrorCode::MalformedInput, "high surrogate then a code unit");
    check_json_rejected("\"\\uD83D\\uD83D\"", mc::ErrorCode::MalformedInput, "two high surrogates");
    check_json_rejected("\"\\uDE00\"", mc::ErrorCode::MalformedInput, "lone low surrogate");
    check_json_rejected("\"abc", mc::ErrorCode::MalformedInput, "unterminated string");
    check_json_rejected("\"abc\\\"", mc::ErrorCode::MalformedInput, "string ending in an escape");
    check_json_rejected("\"\\", mc::ErrorCode::MalformedInput, "escape at end of input");
    check_json_rejected("{\"a\" 1}", mc::ErrorCode::MalformedInput, "missing colon");
    check_json_rejected("{\"a\":1 \"b\":2}", mc::ErrorCode::MalformedInput, "missing comma");
}

MC_TEST(json_limits_and_require_field_readers) {
    // Nesting is bounded: the accepted depth parses, one level more does not.
    std::string accepted;
    for (int index = 0; index < 60; ++index) {
        accepted.push_back('[');
    }
    for (int index = 0; index < 60; ++index) {
        accepted.push_back(']');
    }
    auto shallow = mc::json::parse(accepted, "depth");
    MC_CHECK(shallow.ok());

    std::string rejected;
    for (int index = 0; index < 70; ++index) {
        rejected.push_back('[');
    }
    auto too_deep = mc::json::parse(rejected, "depth");
    MC_CHECK(!too_deep.ok());
    MC_CHECK(too_deep.status().code() == mc::ErrorCode::LimitExceeded);

    // A document larger than the accepted bound is refused before parsing.
    std::string oversized(mc::json::kMaxJsonDocumentBytes + 1U, ' ');
    oversized.front() = '[';
    oversized.back() = ']';
    auto too_large = mc::json::parse(oversized, "size");
    MC_CHECK(!too_large.ok());
    MC_CHECK(too_large.status().code() == mc::ErrorCode::LimitExceeded);

    // An integer that does not fit is a bound failure, never a wrapped value.
    auto overflow = mc::json::parse("9223372036854775808", "int64");
    MC_CHECK(!overflow.ok());
    MC_CHECK(overflow.status().code() == mc::ErrorCode::LimitExceeded);
    auto smallest = mc::json::parse("-9223372036854775808", "int64");
    MC_CHECK(smallest.ok());

    // Field readers name the field that was wrong.
    auto object = mc::json::parse("{\"s\":\"text\",\"i\":7,\"b\":true}", "fields");
    MC_REQUIRE_OK(object);
    std::string text;
    std::int64_t integer = 0;
    bool boolean = false;
    MC_REQUIRE_OK(mc::json::require_object(object.value(), "fields"));
    MC_REQUIRE_OK(mc::json::require_string(object.value(), "s", text));
    MC_CHECK_EQ(text, std::string("text"));
    MC_REQUIRE_OK(mc::json::require_int(object.value(), "i", integer));
    MC_CHECK_EQ(integer, 7LL);
    MC_REQUIRE_OK(mc::json::require_bool(object.value(), "b", boolean));
    MC_CHECK(boolean);

    std::string optional = "unchanged";
    MC_REQUIRE_OK(mc::json::require_string(object.value(), "absent", optional, false));
    MC_CHECK_EQ(optional, std::string("unchanged"));

    MC_CHECK(mc::json::require_string(object.value(), "absent", text).code() == mc::ErrorCode::MissingArgument);
    MC_CHECK(mc::json::require_int(object.value(), "s", integer).code() == mc::ErrorCode::MalformedInput);
    MC_CHECK(mc::json::require_bool(object.value(), "i", boolean).code() == mc::ErrorCode::MalformedInput);
    MC_CHECK(mc::json::require_object(mc::json::Value(1), "fields").code() == mc::ErrorCode::MalformedInput);
}

MC_TEST(json_dump_and_parse_are_inverse_over_generated_values) {
    // A deterministic generator builds nested documents; dumping one and
    // parsing it back must reproduce an identical document.
    mc::test::Random random(0x5151C0DEULL);
    const auto generate = [&random](auto&& self, int depth) -> mc::json::Value {
        const std::uint64_t kind = random.next_below(depth > 2 ? 4U : 6U);
        switch (kind) {
            case 0:
                return mc::json::Value(nullptr);
            case 1:
                return mc::json::Value(random.next_bool());
            case 2:
                return mc::json::Value(static_cast<std::int64_t>(random.next_u64() >> 1U));
            case 3: {
                const char* const words[] = {"plain", "with \"quotes\"", "tab\there", "slash\\and",
                                             "unicode \xC3\xA9"};
                return mc::json::Value(std::string(words[random.next_below(5U)]));
            }
            case 4: {
                mc::json::Value array = mc::json::Value::array();
                const std::uint64_t count = random.next_below(4U);
                for (std::uint64_t index = 0; index < count; ++index) {
                    array.push(self(self, depth + 1));
                }
                return array;
            }
            default: {
                mc::json::Value object = mc::json::Value::object();
                const std::uint64_t count = random.next_below(4U);
                for (std::uint64_t index = 0; index < count; ++index) {
                    object.set("k" + std::to_string(index), self(self, depth + 1));
                }
                return object;
            }
        }
    };

    for (int iteration = 0; iteration < 64; ++iteration) {
        const mc::json::Value original = generate(generate, 0);
        const std::string text = original.dump();
        auto parsed = mc::json::parse(text, "generated");
        MC_CHECK_MSG(parsed.ok(), "could not re-parse a generated document: " + text);
        if (parsed.ok()) {
            MC_CHECK_EQ(parsed.value().dump(), text);
        }
    }
}

MC_TEST_MAIN()

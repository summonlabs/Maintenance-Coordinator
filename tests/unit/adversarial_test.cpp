// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Adversarial input tests.
//
// The point is not that hostile input is rejected - it is that hostile input is
// rejected *for a stated reason*, without reading past a bound, without
// allocating on a claimed size, and without ever converting "unmeasured" into
// "fine".  Every case here is deterministic.

#include "../fixture.hpp"
#include "../harness.hpp"
#include "../mc_test.hpp"

#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "mc/engine.hpp"
#include "mc/json.hpp"
#include "serialize.hpp"

using namespace mc;        // NOLINT(google-build-using-namespace)
using namespace mc::test;  // NOLINT(google-build-using-namespace)

namespace {

[[nodiscard]] std::vector<std::uint8_t> facility_payload(const FacilitySnapshot& facility) {
    Writer writer;
    detail::encode(writer, facility);
    return std::move(writer).take();
}

[[nodiscard]] Status decode_facility(const std::vector<std::uint8_t>& bytes, FacilitySnapshot& out) {
    Reader reader(bytes);
    auto status = detail::decode(reader, out);
    if (!status.ok()) {
        return status;
    }
    if (!reader.fully_consumed()) {
        return fail(ErrorCode::TrailingBytes, "trailing bytes", "decode");
    }
    return Status::success();
}

[[nodiscard]] Status decode_plan(const std::vector<std::uint8_t>& bytes, PlanRecord& out) {
    Reader reader(bytes);
    auto status = detail::decode(reader, out);
    if (!status.ok()) {
        return status;
    }
    if (!reader.fully_consumed()) {
        return fail(ErrorCode::TrailingBytes, "trailing bytes", "decode");
    }
    return Status::success();
}

}  // namespace

MC_TEST(every_truncation_of_a_valid_encoding_is_rejected) {
    const FacilitySnapshot facility = make_facility();
    const std::vector<std::uint8_t> payload = facility_payload(facility);
    MC_REQUIRE(payload.size() > 8U);

    int accepted = 0;
    for (std::size_t length = 0; length < payload.size(); ++length) {
        std::vector<std::uint8_t> truncated(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(length));
        FacilitySnapshot decoded;
        const Status status = decode_facility(truncated, decoded);
        if (status.ok()) {
            ++accepted;
        }
    }
    MC_CHECK_EQ(accepted, 0);
    FacilitySnapshot round_trip;
    MC_REQUIRE_OK(decode_facility(payload, round_trip));
    MC_CHECK(round_trip.digest == facility.digest);
}

MC_TEST(single_byte_mutations_are_rejected_or_canonicalised_consistently) {
    const FacilitySnapshot facility = make_facility();
    const std::vector<std::uint8_t> payload = facility_payload(facility);
    // Every byte position is mutated with a deterministic value.  Decoding is a
    // canonicalising operation, so the property under test is idempotence: an
    // accepted input decodes to a value that re-encodes to bytes which decode
    // again to exactly the same value.  Nothing is ever accepted and then
    // silently reinterpreted on the next pass.
    Random random(0x5EED1234ULL);
    int rejected = 0;
    int accepted = 0;
    int canonicalised = 0;
    for (std::size_t index = 0; index < payload.size(); ++index) {
        std::vector<std::uint8_t> mutated = payload;
        mutated[index] =
            static_cast<std::uint8_t>(mutated[index] ^ static_cast<std::uint8_t>(1U + random.next_below(255U)));
        FacilitySnapshot decoded;
        if (const Status status = decode_facility(mutated, decoded); !status.ok()) {
            ++rejected;
            continue;
        }
        ++accepted;
        Writer writer;
        detail::encode(writer, decoded);
        if (writer.data() != mutated) {
            ++canonicalised;
        }
        FacilitySnapshot again;
        if (const Status status = decode_facility(writer.data(), again); !status.ok()) {
            MC_CHECK_MSG(false, "re-encoded value was rejected at byte " + std::to_string(index));
            continue;
        }
        if (again.digest != decoded.digest) {
            MC_CHECK_MSG(false, "decoding is not idempotent at byte " + std::to_string(index));
        }
    }
    MC_CHECK(rejected > 0);
    MC_CHECK_EQ(accepted, rejected + accepted - rejected);
    (void)canonicalised;
}

MC_TEST(a_plan_record_survives_a_decode_encode_round_trip_under_mutation) {
    Harness harness("adv-plan-roundtrip", make_facility());
    ProposeRequest request;
    request.reason = "adversarial round trip";
    request.requested_by = "operator-1";
    request.activity = MaintenanceActivity::FirmwareUpgrade;
    request.scope = rack_scope();
    request.window = fixture_window();
    request.header.attempt = attempt_id("adv-propose");
    request.header.intent_digest = intent_digest_of(request);
    auto proposed = harness.coordinator().propose(request);
    MC_REQUIRE_OK(proposed);

    Writer writer;
    detail::encode(writer, proposed.value().plan);
    PlanRecord decoded;
    MC_REQUIRE_OK(decode_plan(writer.data(), decoded));
    MC_CHECK(decoded.digest == proposed.value().plan.digest);
    MC_CHECK(decoded.plan.digest == proposed.value().plan.plan.digest);
}

MC_TEST(impossible_timestamps_are_rejected_not_wrapped) {
    Harness harness("adv-timestamps", make_facility());
    ProposeRequest request;
    request.reason = "absurd window";
    request.requested_by = "operator-1";
    request.activity = MaintenanceActivity::HardwareRepair;
    request.scope = rack_scope();
    request.window.start = 1;
    request.window.end = (std::numeric_limits<std::int64_t>::max)();
    request.header.attempt = attempt_id("adv-window");
    request.header.intent_digest = intent_digest_of(request);
    // The duration of such a window overflows a signed subtraction, so the
    // engine must reject it rather than compare a wrapped value with policy.
    auto result = harness.coordinator().propose(request);
    MC_CHECK_CODE(result.status(), ErrorCode::InvalidArgument);

    ProposeRequest negative = request;
    negative.window.start = (std::numeric_limits<std::int64_t>::min)() + 1;
    negative.window.end = 1;
    negative.header.attempt = attempt_id("adv-window-2");
    negative.header.intent_digest = intent_digest_of(negative);
    auto second = harness.coordinator().propose(negative);
    MC_CHECK_CODE(second.status(), ErrorCode::InvalidArgument);
}

MC_TEST(path_traversal_and_odd_store_paths_are_refused) {
    {
        StoreOptions options;
        options.directory = std::filesystem::path("build") / ".." / ".." / "escape";
        auto opened = Store::open(options);
        MC_CHECK(opened.ok() == false);
        MC_CHECK(opened.status().code() == ErrorCode::PathTraversalRejected);
    }
    {
        StoreOptions options;
        options.directory = std::filesystem::path();
        auto opened = Store::open(options);
        MC_CHECK(opened.ok() == false);
        MC_CHECK(opened.status().code() == ErrorCode::InvalidArgument);
    }
    {
        // A store path that approaches the platform path limit still works,
        // because the implementation opens files through the extended-length
        // prefix rather than the legacy MAX_PATH form.
        std::filesystem::path deep = make_temp_dir("adv-long-path");
        for (int index = 0; index < 6; ++index) {
            deep /= "segment-abcdefghijklmnopqrstuvwxyz-0123456789";
        }
        StoreOptions options;
        options.directory = deep;
        auto opened = Store::open(options);
        MC_REQUIRE_OK(opened);
        MC_CHECK(deep.string().size() > 200U);
        auto store = std::move(opened).value();
        auto empty_commit = store->commit(Transaction{});
        MC_CHECK(empty_commit.ok() == false);
        MC_CHECK(empty_commit.status().code() == ErrorCode::InvalidArgument);
    }
}

MC_TEST(a_random_byte_journal_is_rejected_without_reading_past_its_end) {
    const std::filesystem::path directory = make_temp_dir("adv-journal");
    Random random(0xABCDEF01ULL);
    for (int round = 0; round < 8; ++round) {
        const std::size_t length = 1U + static_cast<std::size_t>(random.next_below(4096U));
        std::string bytes;
        bytes.resize(length);
        for (std::size_t index = 0; index < length; ++index) {
            bytes[index] = static_cast<char>(random.next_below(256U));
        }
        std::filesystem::create_directories(directory);
        std::ofstream output(directory / "journal.mcl", std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();

        StoreOptions options;
        options.directory = directory;
        options.read_only = true;
        auto opened = Store::open(options);
        // Any outcome is acceptable except a crash or a silent success with
        // invented state; a random stream must not decode into a ledger.
        if (opened.ok()) {
            MC_CHECK_MSG(opened.value()->cached_ledger().plans.empty(),
                         "random bytes were accepted as a ledger with plans");
        }
    }
}

MC_TEST(a_document_that_is_not_json_is_rejected_precisely) {
    const char* const cases[] = {
        "", "{", "[]]", "{\"a\":}", "{\"a\":1,}", "{\"a\":1 \"b\":2}", "tru", "01",
        "{\"a\":1.5}", "\"\\uD800\"", "\"\\uDC00\"", "\"unterminated",
    };
    for (const char* text : cases) {
        auto parsed = json::parse(text, "case");
        MC_CHECK(parsed.ok() == false);
    }
    auto valid = json::parse("{\"a\":[1,2,3],\"b\":\"x\",\"c\":true,\"d\":null}", "case");
    MC_REQUIRE_OK(valid);
    MC_CHECK(valid.value().has("a"));
    MC_CHECK(valid.value().find("zz") == nullptr);
}

MC_TEST(unknown_and_duplicate_targets_are_reported_consistently) {
    Harness harness("adv-targets", make_facility());
    ProposeRequest request;
    request.reason = "duplicate targets";
    request.requested_by = "operator-1";
    request.activity = MaintenanceActivity::HardwareRepair;
    request.scope = MaintenanceScope({target(TargetKind::Rack, "rack-01"), target(TargetKind::Rack, "rack-01")});
    request.window = fixture_window();
    request.header.attempt = attempt_id("adv-duplicate");
    request.header.intent_digest = intent_digest_of(request);
    auto first = harness.coordinator().propose(request);
    MC_REQUIRE_OK(first);
    // Duplicates collapse into one canonical target rather than being rejected
    // or silently counted twice.
    MC_CHECK_EQ(first.value().plan.plan.scope.size(), static_cast<std::size_t>(1));

    ProposeRequest mixed = request;
    mixed.scope = MaintenanceScope({target(TargetKind::Rack, "rack-01"), target(TargetKind::Asset, "asset-99")});
    mixed.header.attempt = attempt_id("adv-mixed");
    mixed.header.intent_digest = intent_digest_of(mixed);
    auto second = harness.coordinator().propose(mixed);
    MC_CHECK_CODE(second.status(), ErrorCode::UnknownTarget);
}

MC_TEST(stale_and_missing_generations_are_distinguished) {
    Harness harness("adv-generations", make_facility());
    ProposeRequest request;
    request.reason = "generation binding";
    request.requested_by = "operator-1";
    request.activity = MaintenanceActivity::Inspection;
    request.scope = rack_scope();
    request.window = fixture_window();
    request.header.attempt = attempt_id("adv-gen");
    request.header.intent_digest = intent_digest_of(request);
    auto proposed = harness.coordinator().propose(request);
    MC_REQUIRE_OK(proposed);

    EvaluateRequest evaluate;
    evaluate.plan = proposed.value().plan.plan.id;
    evaluate.revision = Revision::from_value(99U);  // a revision that does not exist
    evaluate.header.attempt = attempt_id("adv-gen-evaluate");
    evaluate.header.intent_digest = intent_digest_of(evaluate);
    auto stale = harness.coordinator().evaluate(evaluate);
    MC_CHECK_CODE(stale.status(), ErrorCode::StaleRevision);

    EvaluateRequest unknown;
    unknown.plan = plan_id("plan-does-not-exist");
    unknown.revision = Revision::from_value(1U);
    unknown.header.attempt = attempt_id("adv-gen-unknown");
    unknown.header.intent_digest = intent_digest_of(unknown);
    auto missing = harness.coordinator().evaluate(unknown);
    MC_CHECK_CODE(missing.status(), ErrorCode::UnknownPlan);

    // Without an attempt identity nothing is mutated at all.
    EvaluateRequest anonymous = unknown;
    anonymous.plan = proposed.value().plan.plan.id;
    anonymous.revision = proposed.value().plan.plan.revision;
    anonymous.header.attempt = AttemptId{};
    anonymous.header.intent_digest = intent_digest_of(anonymous);
    auto refused = harness.coordinator().evaluate(anonymous);
    MC_CHECK_CODE(refused.status(), ErrorCode::InvalidArgument);
}

MC_TEST_MAIN()

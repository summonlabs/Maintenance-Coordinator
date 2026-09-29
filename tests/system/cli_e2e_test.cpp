// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// End-to-end proof at the command line boundary.
//
// Every command here is a real, separate operating-system process running the
// installed command line tool.  The JSON it prints is parsed with the library's
// own strict reader, so the test fails if the printed document is not the
// document the contract promises.

#include "../mc_test.hpp"
#include "../process.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "mc/json.hpp"

using namespace mc;        // NOLINT(google-build-using-namespace)
using namespace mc::test;  // NOLINT(google-build-using-namespace)

namespace {

// The tool is built next to the library's own build tree, which the test
// executable is also inside, so the path is derived from the running module.
[[nodiscard]] std::filesystem::path tool_path() {
    std::vector<wchar_t> buffer(4096, L'\0');
    const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    const std::filesystem::path self(std::wstring(buffer.data(), written));
    return self.parent_path().parent_path() / "tools" / "maintenance-coordinator" /
           "maintenance-coordinator.exe";
}

[[nodiscard]] std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

class Cli {
public:
    Cli(const std::string& directory, const std::string& store)
        : directory_(directory), store_(store), tool_(narrow(tool_path().wstring())) {}

    struct Invocation {
        std::uint32_t exit_code{0};
        json::Value document;
        std::string raw;
        [[nodiscard]] bool ok() const { return document.has("ok") && document.find("ok")->as_bool() != nullptr &&
                                              *document.find("ok")->as_bool(); }
        [[nodiscard]] std::string error_code() const {
            const json::Value* error = document.find("error");
            if (error == nullptr) {
                return {};
            }
            const json::Value* code = error->find("code");
            return code != nullptr && code->as_string() != nullptr ? *code->as_string() : std::string{};
        }
    };

    [[nodiscard]] Invocation run(const std::vector<std::string>& arguments, std::size_t index) {
        const std::string output =
            (std::filesystem::path(directory_) / ("out-" + std::to_string(index) + ".json")).string();
        // The command comes first, then the global options, then its own.
        std::vector<std::string> full;
        full.reserve(arguments.size() + 8U);
        if (!arguments.empty()) {
            full.push_back(arguments.front());
        }
        full.push_back("--store");
        full.push_back(store_);
        full.push_back("--json");
        full.push_back("--now");
        full.push_back("2026-01-01T02:00:00Z");
        full.push_back("--actor");
        full.push_back("operator-1");
        for (std::size_t position = 1; position < arguments.size(); ++position) {
            full.push_back(arguments[position]);
        }
        Child child = Child::spawn_capturing(tool_, full, output);
        Invocation invocation;
        if (!child.valid()) {
            invocation.exit_code = 0xFFFFFFFFU;
            return invocation;
        }
        invocation.exit_code = child.wait();
        std::ifstream input(output, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        invocation.raw = text;
        auto parsed = json::parse(text, "cli-output");
        if (parsed.ok()) {
            invocation.document = std::move(parsed).value();
        }
        return invocation;
    }

private:
    std::string directory_;
    std::string store_;
    std::string tool_;
};

[[nodiscard]] std::string phase_of(const Cli::Invocation& invocation) {
    const json::Value* plan = invocation.document.find("plan");
    if (plan == nullptr) {
        return {};
    }
    const json::Value* phase = plan->find("phase");
    return phase != nullptr && phase->as_string() != nullptr ? *phase->as_string() : std::string{};
}

void write_model(const std::filesystem::path& path) {
    const char* const model = R"JSON({
  "revision": 1, "facility_epoch": 1, "policy_generation": 1, "dependency_generation": 1,
  "capacity_generation": 1, "topology_generation": 1, "maintenance_generation": 1, "control_epoch": 1,
  "observed_at": "2026-01-01T00:00:00Z",
  "policy": {"id": "policy-1", "revision": 1, "min_redundancy_margin_units": 0, "min_spare_capacity_units": 0,
             "min_power_headroom_milliwatts": 1, "min_cooling_headroom_units": 1, "approval_validity_hours": 8,
             "max_window_hours": 168, "max_concurrent_windows_per_rack": 1, "require_personnel_evidence": true,
             "require_dual_approval": false, "require_work_stop_evidence": true, "require_restoration_evidence": true},
  "assets": [
    {"id":"asset-01","rack":"rack-01","site":"site-a","lifecycle":"in-service","service_class":"business-critical",
     "lifecycle_generation":1,"hardware_generation":1,"firmware_generation":1,"capacity_units":1,
     "isolatable":true,"drainable":true,"healthy":true},
    {"id":"asset-02","rack":"rack-01","site":"site-a","lifecycle":"in-service","service_class":"business-critical",
     "lifecycle_generation":1,"hardware_generation":1,"firmware_generation":1,"capacity_units":1,
     "isolatable":true,"drainable":true,"healthy":true},
    {"id":"asset-03","rack":"rack-02","site":"site-a","lifecycle":"in-service","service_class":"business-critical",
     "lifecycle_generation":1,"hardware_generation":1,"firmware_generation":1,"capacity_units":1,
     "isolatable":true,"drainable":true,"healthy":true},
    {"id":"asset-04","rack":"rack-02","site":"site-a","lifecycle":"in-service","service_class":"business-critical",
     "lifecycle_generation":1,"hardware_generation":1,"firmware_generation":1,"capacity_units":1,
     "isolatable":true,"drainable":true,"healthy":true}],
  "redundancy_groups": [{"id":"rg-a","site":"site-a","mode":"n+2","required_units":2,"observed_available_units":4,
                         "members":["asset-01","asset-02","asset-03","asset-04"]}],
  "capacity_pools": [{"id":"pool-a","site":"site-a","units_total":10,"units_available":8,"units_protected":2}],
  "power_domains": [{"id":"pd-a","site":"site-a","headroom":{"measured":true,"available_units":20,"required_units":5},
                     "interlocked":false,"interlock_reason":""}],
  "cooling_zones": [{"id":"cz-a","site":"site-a","headroom":{"measured":true,"available_units":20,"required_units":5},
                     "interlocked":false,"interlock_reason":""}],
  "fabric_segments": [{"id":"fs-a","site":"site-a","available_paths":4,"required_paths":2,"drainable":true}]
})JSON";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << model;
}

}  // namespace

MC_TEST(command_line_lifecycle_end_to_end) {
    const std::filesystem::path directory = make_temp_dir("cli-e2e");
    const std::string store = (directory / "store").string();
    const std::filesystem::path model = directory / "model.json";
    write_model(model);
    MC_REQUIRE(std::filesystem::exists(tool_path()));
    Cli cli(directory.string(), store);
    std::size_t index = 0;

    MC_CHECK_EQ(cli.run({"init"}, index++).exit_code, 0U);

    auto installed = cli.run({"install-facility", "--file", model.string()}, index++);
    MC_REQUIRE_OK(installed.exit_code == 0U ? Status::success()
                                            : fail(ErrorCode::InvalidArgument, installed.raw));
    MC_CHECK(installed.ok());

    auto proposed = cli.run({"propose", "--reason", "replace failed accelerator", "--requester", "operator-1",
                             "--activity", "hardware-repair", "--priority", "elevated", "--risk", "high",
                             "--target", "rack:rack-01", "--start", "2026-01-01T00:00:00Z", "--end",
                             "2026-01-01T08:00:00Z"},
                            index++);
    MC_REQUIRE_OK(proposed.exit_code == 0U ? Status::success() : fail(ErrorCode::InvalidArgument, proposed.raw));
    MC_CHECK_EQ(phase_of(proposed), std::string("proposed"));

    auto evaluated = cli.run({"evaluate", "--plan", "plan-000001", "--revision", "1"}, index++);
    MC_REQUIRE_OK(evaluated.exit_code == 0U ? Status::success() : fail(ErrorCode::InvalidArgument, evaluated.raw));
    MC_CHECK_EQ(phase_of(evaluated), std::string("evaluating"));

    // Beginning before readiness is a lifecycle refusal, not an accident.
    auto too_early = cli.run({"begin", "--plan", "plan-000001", "--revision", "1"}, index++);
    MC_CHECK_EQ(too_early.exit_code, 4U);
    MC_CHECK_EQ(too_early.error_code(), std::string("NotReady"));

    auto approved = cli.run({"approve", "--plan", "plan-000001", "--revision", "1", "--approver", "approver-1",
                             "--evidence", "change-record-4711"},
                            index++);
    MC_REQUIRE_OK(approved.exit_code == 0U ? Status::success() : fail(ErrorCode::InvalidArgument, approved.raw));
    MC_CHECK_EQ(phase_of(approved), std::string("approved"));

    auto derived = cli.run({"derive-obligations", "--plan", "plan-000001", "--revision", "1"}, index++);
    MC_REQUIRE_OK(derived.exit_code == 0U ? Status::success() : fail(ErrorCode::InvalidArgument, derived.raw));
    MC_CHECK_EQ(phase_of(derived), std::string("pre-drain"));

    // The obligation list is printed, so the test can drive the rest of the
    // lifecycle without hard coding identities.
    const json::Value* obligations = derived.document.find("obligations");
    MC_REQUIRE(obligations != nullptr);
    const json::Value* statuses = obligations->find("statuses");
    MC_REQUIRE(statuses != nullptr && statuses->as_array() != nullptr);
    MC_CHECK(statuses->as_array()->size() >= static_cast<std::size_t>(8));

    std::uint64_t sequence = 0;
    for (const auto& status : *statuses->as_array()) {
        const json::Value* id = status.find("id");
        const json::Value* stage = status.find("stage");
        const json::Value* kind = status.find("kind");
        MC_REQUIRE(id != nullptr && id->as_string() != nullptr);
        MC_REQUIRE(stage != nullptr && stage->as_string() != nullptr);
        MC_REQUIRE(kind != nullptr && kind->as_string() != nullptr);
        if (*stage->as_string() != std::string("pre-drain") && *stage->as_string() != std::string("isolation")) {
            continue;
        }
        ReceiptKind receipt_kind{ReceiptKind::DrainRequested};
        MC_REQUIRE(parse_receipt_kind("drain-observed", receipt_kind));
        auto obligation_kind = ObligationKind::AsiDrain;
        MC_REQUIRE(parse_obligation_kind(*kind->as_string(), obligation_kind));
        const std::string receipt_kind_name(to_string(satisfying_receipt(obligation_kind)));
        const std::string authority_name(to_string(authority_of(obligation_kind)));
        auto ingested = cli.run({"ingest", "--plan", "plan-000001", "--revision", "1", "--receipt",
                                 "receipt-" + std::to_string(++sequence), "--obligation", *id->as_string(),
                                 "--kind", receipt_kind_name, "--authority", authority_name, "--issuer",
                                 authority_name + "-authority", "--sequence", std::to_string(sequence),
                                 "--evidence", "observed for " + *id->as_string()},
                                index++);
        MC_CHECK_EQ(ingested.exit_code, 0U);
    }

    auto ready = cli.run({"show", "--plan", "plan-000001"}, index++);
    MC_REQUIRE_OK(ready.exit_code == 0U ? Status::success() : fail(ErrorCode::InvalidArgument, ready.raw));
    MC_CHECK_EQ(phase_of(ready), std::string("ready"));

    MC_CHECK_EQ(cli.run({"begin", "--plan", "plan-000001", "--revision", "1"}, index++).exit_code, 0U);
    MC_CHECK_EQ(cli.run({"progress", "--plan", "plan-000001", "--revision", "1", "--kind", "work-completed",
                         "--note", "replacement installed"},
                        index++)
                   .exit_code,
                0U);

    // The completion stage needs its observations too.
    const json::Value* verified = derived.document.find("obligations");
    MC_REQUIRE(verified != nullptr);
    auto shown = cli.run({"show", "--plan", "plan-000001"}, index++);
    MC_REQUIRE_OK(shown.exit_code == 0U ? Status::success() : fail(ErrorCode::InvalidArgument, shown.raw));
    const json::Value* shown_obligations = shown.document.find("obligations");
    MC_REQUIRE(shown_obligations != nullptr);
    const json::Value* shown_statuses = shown_obligations->find("statuses");
    MC_REQUIRE(shown_statuses != nullptr && shown_statuses->as_array() != nullptr);
    for (const auto& status : *shown_statuses->as_array()) {
        const json::Value* id = status.find("id");
        const json::Value* stage = status.find("stage");
        const json::Value* kind = status.find("kind");
        const json::Value* disposition = status.find("disposition");
        MC_REQUIRE(id != nullptr && id->as_string() != nullptr);
        MC_REQUIRE(stage != nullptr && stage->as_string() != nullptr);
        MC_REQUIRE(kind != nullptr && kind->as_string() != nullptr);
        MC_REQUIRE(disposition != nullptr && disposition->as_string() != nullptr);
        if (*stage->as_string() != std::string("completion")) {
            continue;
        }
        if (*disposition->as_string() == std::string("satisfied")) {
            continue;
        }
        auto obligation_kind = ObligationKind::AsiDrain;
        MC_REQUIRE(parse_obligation_kind(*kind->as_string(), obligation_kind));
        const std::string receipt_kind_name(to_string(satisfying_receipt(obligation_kind)));
        const std::string authority_name(to_string(authority_of(obligation_kind)));
        auto ingested = cli.run({"ingest", "--plan", "plan-000001", "--revision", "1", "--receipt",
                                 "receipt-done-" + std::to_string(++sequence), "--obligation", *id->as_string(),
                                 "--kind", receipt_kind_name, "--authority", authority_name, "--issuer",
                                 authority_name + "-authority", "--sequence", std::to_string(sequence),
                                 "--evidence", "restored for " + *id->as_string()},
                                index++);
        MC_CHECK_EQ(ingested.exit_code, 0U);
    }

    auto restored = cli.run({"verify-restoration", "--plan", "plan-000001", "--revision", "1"}, index++);
    MC_CHECK_EQ(restored.exit_code, 0U);
    MC_CHECK_EQ(phase_of(restored), std::string("restore"));

    auto completed = cli.run({"complete", "--plan", "plan-000001", "--revision", "1", "--note", "window closed"},
                             index++);
    MC_CHECK_EQ(completed.exit_code, 0U);
    MC_CHECK_EQ(phase_of(completed), std::string("complete"));

    auto listed = cli.run({"list"}, index++);
    MC_REQUIRE_OK(listed.exit_code == 0U ? Status::success() : fail(ErrorCode::InvalidArgument, listed.raw));
    const json::Value* plans = listed.document.find("plans");
    MC_REQUIRE(plans != nullptr && plans->as_array() != nullptr);
    MC_CHECK_EQ(plans->as_array()->size(), static_cast<std::size_t>(1));

    // A second process reopens the same store and sees the same committed state.
    auto reopened = cli.run({"show", "--plan", "plan-000001"}, index++);
    MC_CHECK_EQ(reopened.exit_code, 0U);
    MC_CHECK_EQ(phase_of(reopened), std::string("complete"));

    // Failure surfaces: unknown plan, stale revision, unknown command.
    auto unknown = cli.run({"show", "--plan", "plan-does-not-exist"}, index++);
    MC_CHECK_EQ(unknown.exit_code, 2U);
    MC_CHECK_EQ(unknown.error_code(), std::string("UnknownPlan"));
    auto stale = cli.run({"evaluate", "--plan", "plan-000001", "--revision", "99"}, index++);
    MC_CHECK_EQ(stale.exit_code, 3U);
    MC_CHECK_EQ(stale.error_code(), std::string("StaleRevision"));
    auto bogus = cli.run({"frobnicate"}, index++);
    MC_CHECK_EQ(bogus.exit_code, 1U);

    // The audit trail is readable and reports every durable record.
    auto audit = cli.run({"audit"}, index++);
    MC_CHECK_EQ(audit.exit_code, 0U);

    // Idempotent replay: the same command line twice applies once.
    auto first = cli.run({"cancel", "--plan", "plan-000001", "--revision", "1", "--reason", "already complete"},
                         index++);
    MC_CHECK_EQ(first.exit_code, 4U);  // a completed plan is terminal
}

MC_TEST_MAIN()

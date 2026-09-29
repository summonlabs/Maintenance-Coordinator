// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// maintenance-coordinator: the command line surface.
//
// The tool is a thin, strict adapter over the library.  It parses arguments and
// JSON, calls exactly one coordinator command, and renders the result.  It owns
// no policy of its own: every enum spelling, every identity, every generation
// and every timestamp is validated with the library helper that owns it, so the
// CLI can never disagree with the engine about what a value means.
//
// Determinism rules honoured here:
//   * the whole input document is read and validated before the store is opened;
//   * one process runs exactly one command and issues exactly one call;
//   * no pointer, address, elapsed time or other run-varying fact is printed;
//   * the default attempt identity is derived from the intent digest, so
//     repeating a command is a replay rather than a second mutation.
//
// Exit codes are the error-code class of the failure:
//   0 ok, 1 usage (1xx), 2 identity (2xx), 3 authority/fencing (3xx),
//   4 lifecycle (4xx), 5 policy (5xx), 6 I/O and bounds (6xx/7xx),
//   7 replay (8xx), 8 internal (9xx).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include "mc/engine.hpp"
#include "mc/json.hpp"
#include "mc/store.hpp"
#include "mc/version.hpp"

namespace {

using namespace mc;  // NOLINT(google-build-using-namespace)

// ---------------------------------------------------------------------------
// Exit codes
// ---------------------------------------------------------------------------

[[nodiscard]] int exit_code_of(ErrorCode code) noexcept {
    const auto value = static_cast<unsigned>(code);
    if (value == 0U) {
        return 0;
    }
    if (value < 200U) {
        return 1;
    }
    if (value < 300U) {
        return 2;
    }
    if (value < 400U) {
        return 3;
    }
    if (value < 500U) {
        return 4;
    }
    if (value < 600U) {
        return 5;
    }
    if (value < 800U) {
        return 6;
    }
    if (value < 900U) {
        return 7;
    }
    return 8;
}

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

constexpr std::string_view kDefaultActor = "cli-operator";
constexpr std::uint64_t kMaxUint32Value = 0xFFFFFFFFULL;
constexpr std::uint64_t kMaxHours = 1000000ULL;  // ~114 years; keeps nanos in range

[[nodiscard]] std::string join_names(std::initializer_list<std::string_view> names) {
    std::string text;
    bool first = true;
    for (const std::string_view name : names) {
        if (!first) {
            text += '|';
        }
        first = false;
        text += name;
    }
    return text;
}

[[nodiscard]] std::string comma_list(const std::vector<std::string>& items) {
    std::string text;
    for (const auto& item : items) {
        if (!text.empty()) {
            text += ',';
        }
        text += item;
    }
    return text;
}

template <typename Tag>
[[nodiscard]] std::string comma_list(const std::vector<Ident<Tag>>& items) {
    std::string text;
    for (const auto& item : items) {
        if (!text.empty()) {
            text += ',';
        }
        text += item.name();
    }
    return text;
}

[[nodiscard]] std::string comma_list(const std::vector<TargetRef>& targets) {
    std::string text;
    for (const auto& target : targets) {
        if (!text.empty()) {
            text += ',';
        }
        text += target.str();
    }
    return text;
}

[[nodiscard]] std::string quote_text(std::string_view text) {
    return std::string("\"") + std::string(text) + "\"";
}

// Paths arrive as UTF-8 and are handed to the standard library as UTF-8; the
// narrow path constructor would reinterpret them in the active code page.
[[nodiscard]] std::filesystem::path to_path(const std::string& text) {
    const auto* begin = reinterpret_cast<const char8_t*>(text.data());
    return std::filesystem::path(std::u8string(begin, begin + text.size()));
}

[[nodiscard]] std::string to_utf8(const wchar_t* text) {
    if (text == nullptr) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    if (written <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written - 1));
    return out;
}

[[nodiscard]] std::string magic_text(std::uint32_t magic) {
    std::string text(4, '\0');
    for (std::size_t index = 0; index < 4U; ++index) {
        text[index] = static_cast<char>((magic >> (8U * index)) & 0xFFU);
    }
    return text;
}

// ---------------------------------------------------------------------------
// JSON value helpers.  Every number is pushed as int64 explicitly: the value
// model deliberately has no unsigned constructor, and an implicit conversion
// here would be an ambiguity rather than a mistake the compiler can see.
// ---------------------------------------------------------------------------

[[nodiscard]] json::Value jstring(std::string_view text) { return json::Value(std::string(text)); }
[[nodiscard]] json::Value jcount(std::uint64_t value) { return json::Value(static_cast<std::int64_t>(value)); }
[[nodiscard]] json::Value jint(std::int64_t value) { return json::Value(value); }
[[nodiscard]] json::Value jbool(bool value) { return json::Value(value); }
[[nodiscard]] json::Value jnull() { return json::Value(nullptr); }

[[nodiscard]] json::Value jdigest(const Digest& value) { return jstring(value.hex()); }

[[nodiscard]] json::Value jtext(std::string_view text) { return jstring(text); }

template <typename Tag>
[[nodiscard]] json::Value jgeneration(const Generation<Tag>& value) {
    return value.is_set() ? jcount(value.value()) : jnull();
}

[[nodiscard]] json::Value jtime(Timestamp value) {
    return is_set(value) ? jstring(format_timestamp(value)) : jnull();
}

template <typename Tag>
[[nodiscard]] json::Value jident(const Ident<Tag>& value) { return jstring(value.name()); }

template <typename Tag>
[[nodiscard]] json::Value jident_array(const std::vector<Ident<Tag>>& values) {
    json::Value array = json::Value::array();
    for (const auto& value : values) {
        array.push(jident(value));
    }
    return array;
}

template <typename Enum, typename Fn>
[[nodiscard]] json::Value jstring_array(const std::vector<Enum>& values, Fn convert) {
    json::Value array = json::Value::array();
    for (const auto& value : values) {
        array.push(jstring(convert(value)));
    }
    return array;
}

// ---------------------------------------------------------------------------
// Argument list
// ---------------------------------------------------------------------------

struct ArgList {
    std::vector<std::pair<std::string, std::string>> items;

    [[nodiscard]] bool has(std::string_view name) const {
        for (const auto& item : items) {
            if (item.first == name) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const std::string* find(std::string_view name) const {
        const std::string* found = nullptr;
        for (const auto& item : items) {
            if (item.first == name) {
                found = &item.second;
            }
        }
        return found;
    }

    [[nodiscard]] std::vector<std::string> all(std::string_view name) const {
        std::vector<std::string> values;
        for (const auto& item : items) {
            if (item.first == name) {
                values.push_back(item.second);
            }
        }
        return values;
    }
};

[[nodiscard]] bool is_flag_option(std::string_view name) {
    return name == "read-only" || name == "json" || name == "help" || name == "version" ||
           name == "flexible" || name == "active-only";
}

[[nodiscard]] bool is_global_option(std::string_view name) {
    return name == "store" || name == "read-only" || name == "json" || name == "now" || name == "actor" ||
           name == "attempt" || name == "help" || name == "version";
}

struct CommandSpec {
    std::string_view name;
    std::vector<std::string_view> options;
};

[[nodiscard]] const std::vector<CommandSpec>& command_table() {
    static const std::vector<CommandSpec> table{
        {"init", {}},
        {"install-facility", {"file"}},
        {"propose",
         {"reason", "requester", "activity", "priority", "risk", "target", "start", "end", "plan-id", "flexible"}},
        {"evaluate", {"plan", "revision"}},
        {"approve", {"plan", "revision", "approver", "evidence", "witness"}},
        {"derive-obligations", {"plan", "revision"}},
        {"ingest",
         {"plan", "revision", "receipt", "obligation", "kind", "authority", "issuer", "sequence", "evidence",
          "observed-at"}},
        {"grant-exception", {"plan", "revision", "waive", "target", "grantor", "justification", "validity-hours"}},
        {"begin", {"plan", "revision"}},
        {"progress", {"plan", "revision", "kind", "note"}},
        {"verify-restoration", {"plan", "revision"}},
        {"complete", {"plan", "revision", "note"}},
        {"cancel", {"plan", "revision", "reason"}},
        {"recover", {"plan"}},
        {"explain", {"plan"}},
        {"show", {"plan"}},
        {"list", {"active-only"}},
        {"audit", {}},
        {"compact", {}},
    };
    return table;
}

[[nodiscard]] const CommandSpec* find_command(std::string_view name) {
    for (const auto& spec : command_table()) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

[[nodiscard]] bool command_allows(const CommandSpec& spec, std::string_view name) {
    if (is_global_option(name)) {
        return true;
    }
    for (const auto& option : spec.options) {
        if (option == name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] Status parse_arguments(const std::vector<std::string>& tokens, ArgList& out) {
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const std::string& token = tokens[index];
        if (token.size() < 3U || token[0] != '-' || token[1] != '-') {
            return fail(ErrorCode::InvalidUsage, "every option is spelled --name", "argument", token);
        }
        std::string name = token.substr(2);
        std::string value;
        bool assigned = false;
        const std::size_t equals = name.find('=');
        if (equals != std::string::npos) {
            value = name.substr(equals + 1U);
            name = name.substr(0, equals);
            assigned = true;
        }
        if (name.empty()) {
            return fail(ErrorCode::InvalidUsage, "an option name must not be empty", "argument", token);
        }
        if (is_flag_option(name)) {
            if (assigned) {
                return fail(ErrorCode::InvalidUsage, "this option does not take a value", name, value);
            }
            out.items.emplace_back(std::move(name), std::string());
            continue;
        }
        if (!assigned) {
            if (index + 1U >= tokens.size()) {
                return fail(ErrorCode::MissingArgument, "this option needs a value", name);
            }
            ++index;
            value = tokens[index];
        }
        out.items.emplace_back(std::move(name), std::move(value));
    }
    return Status::success();
}

// ---------------------------------------------------------------------------
// Typed option readers.  Every failure is an argument error (1xx).
// ---------------------------------------------------------------------------

[[nodiscard]] Result<std::string> require_option(const ArgList& args, std::string_view name) {
    const std::string* value = args.find(name);
    if (value == nullptr) {
        return fail(ErrorCode::MissingArgument, "this command needs this option", std::string(name));
    }
    if (value->empty()) {
        return fail(ErrorCode::InvalidArgument, "this option must not be empty", std::string(name));
    }
    return *value;
}

[[nodiscard]] std::string optional_option(const ArgList& args, std::string_view name) {
    const std::string* value = args.find(name);
    return value == nullptr ? std::string() : *value;
}

[[nodiscard]] Status parse_unsigned(std::string_view text, std::string_view what, std::uint64_t limit,
                                    std::uint64_t& out) {
    if (text.empty()) {
        return fail(ErrorCode::InvalidArgument, "value must not be empty", std::string(what));
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return fail(ErrorCode::InvalidArgument, "value must be a non-negative decimal integer",
                        std::string(what), std::string(text));
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (limit - digit) / 10U) {
            return fail(ErrorCode::InvalidArgument, "value is out of range", std::string(what), std::string(text));
        }
        value = (value * 10U) + digit;
    }
    out = value;
    return Status::success();
}

[[nodiscard]] Result<std::uint64_t> unsigned_option(const ArgList& args, std::string_view name,
                                                    std::uint64_t limit = kMaxUint32Value) {
    const auto text = require_option(args, name);
    if (!text.ok()) {
        return text.status();
    }
    std::uint64_t value = 0;
    if (auto status = parse_unsigned(*text, name, limit, value); !status.ok()) {
        return status;
    }
    return value;
}

template <typename Tag>
[[nodiscard]] Result<Ident<Tag>> ident_option(const ArgList& args, std::string_view name) {
    const auto text = require_option(args, name);
    if (!text.ok()) {
        return text.status();
    }
    return Ident<Tag>::parse(*text, name);
}

[[nodiscard]] Result<Timestamp> timestamp_option(const ArgList& args, std::string_view name) {
    const auto text = require_option(args, name);
    if (!text.ok()) {
        return text.status();
    }
    Timestamp value = kNoTimestamp;
    if (!parse_timestamp(*text, value)) {
        return fail(ErrorCode::InvalidArgument, "value is not an RFC 3339 UTC timestamp", std::string(name), *text);
    }
    return value;
}

template <typename Enum, typename ParseFn>
[[nodiscard]] Result<Enum> enum_option(const ArgList& args, std::string_view name, ParseFn parse) {
    const auto text = require_option(args, name);
    if (!text.ok()) {
        return text.status();
    }
    Enum value{};
    if (!parse(*text, value)) {
        return fail(ErrorCode::InvalidArgument, "value is not a name this library knows", std::string(name), *text);
    }
    return value;
}

[[nodiscard]] Result<std::vector<TargetRef>> target_options(const ArgList& args, std::string_view name,
                                                            bool required) {
    const std::vector<std::string> texts = args.all(name);
    if (required && texts.empty()) {
        return fail(ErrorCode::MissingArgument, "this command needs at least one target", std::string(name));
    }
    std::vector<TargetRef> targets;
    targets.reserve(texts.size());
    for (const auto& text : texts) {
        auto parsed = TargetRef::parse(text);
        if (!parsed.ok()) {
            return parsed.status();
        }
        targets.push_back(parsed.value());
    }
    return targets;
}

[[nodiscard]] Result<MaintenanceScope> scope_option(const ArgList& args) {
    auto targets = target_options(args, "target", true);
    if (!targets.ok()) {
        return targets.status();
    }
    MaintenanceScope scope(std::move(targets).value());
    scope.canonicalize();
    return scope;
}

[[nodiscard]] Result<std::vector<ErrorCode>> waived_options(const ArgList& args) {
    const std::vector<std::string> texts = args.all("waive");
    if (texts.empty()) {
        return fail(ErrorCode::MissingArgument, "this command needs at least one --waive name", "waive");
    }
    std::vector<ErrorCode> codes;
    codes.reserve(texts.size());
    for (const auto& text : texts) {
        ErrorCode code = ErrorCode::Ok;
        if (!parse_error_code(text, code)) {
            return fail(ErrorCode::InvalidArgument, "value is not an ErrorCode spelling", "waive", text);
        }
        if (code == ErrorCode::Ok) {
            return fail(ErrorCode::InvalidArgument, "no condition may be waived as \"ok\"", "waive", text);
        }
        codes.push_back(code);
    }
    return codes;
}

// ---------------------------------------------------------------------------
// The invocation: global options plus one command.
// ---------------------------------------------------------------------------

struct Invocation {
    std::string command;
    ArgList args;
    std::string store;
    bool read_only{false};
    bool json{false};
    bool have_now{false};
    Timestamp now{kNoTimestamp};
    std::string actor{kDefaultActor};
    bool have_attempt{false};
    std::string attempt;
};

[[nodiscard]] Status build_invocation(const std::string& command, const ArgList& args, const CommandSpec& spec,
                                      Invocation& out) {
    for (const auto& item : args.items) {
        if (!command_allows(spec, item.first)) {
            return fail(ErrorCode::InvalidUsage, "this option is not valid for this command", item.first, command);
        }
    }
    out.command = command;
    out.args = args;
    out.read_only = args.has("read-only");
    out.json = args.has("json");

    const auto store = require_option(args, "store");
    if (!store.ok()) {
        return store.status();
    }
    out.store = *store;

    if (args.has("now")) {
        auto instant = timestamp_option(args, "now");
        if (!instant.ok()) {
            return instant.status();
        }
        out.have_now = true;
        out.now = *instant;
    }
    if (args.has("actor")) {
        auto actor = ident_option<OperatorIdTag>(args, "actor");
        if (!actor.ok()) {
            return actor.status();
        }
        out.actor = actor.value().name();
    }
    if (args.has("attempt")) {
        auto attempt = ident_option<AttemptIdTag>(args, "attempt");
        if (!attempt.ok()) {
            return attempt.status();
        }
        out.have_attempt = true;
        out.attempt = attempt.value().name();
    }
    return Status::success();
}

// A command header binds the attempt identity to the exact intent digest the
// engine will derive, so nothing here can be replayed as a different operation.
template <typename Request>
[[nodiscard]] Status apply_header(Request& request, const Invocation& invocation) {
    const Digest intent = intent_digest_of(request);
    const std::string attempt =
        invocation.have_attempt ? invocation.attempt : std::string("auto-") + intent.hex().substr(0, 16U);
    auto identity = AttemptId::parse(attempt, "attempt");
    if (!identity.ok()) {
        return identity.status();
    }
    request.header.attempt = identity.value();
    request.header.intent_digest = intent;
    request.header.actor = invocation.actor;
    return Status::success();
}

// ---------------------------------------------------------------------------
// The facility document reader.  Strict: an unknown member is MalformedInput,
// a missing required member is MissingArgument, and every scalar is validated
// by the library helper that owns its type.
// ---------------------------------------------------------------------------

class Fields {
public:
    Fields(const json::Value& value, std::string what) : value_(&value), what_(std::move(what)) {}

    [[nodiscard]] Status open() { return json::require_object(*value_, what_); }

    [[nodiscard]] Status text(std::string_view key, std::string& out, bool required = true) {
        mark(key);
        return json::require_string(*value_, key, out, required);
    }

    [[nodiscard]] Status integer(std::string_view key, std::int64_t& out, bool required = true) {
        mark(key);
        return json::require_int(*value_, key, out, required);
    }

    [[nodiscard]] Status boolean(std::string_view key, bool& out, bool required = true) {
        mark(key);
        return json::require_bool(*value_, key, out, required);
    }

    [[nodiscard]] Status object(std::string_view key, const json::Value*& out, bool required = true) {
        mark(key);
        const json::Value* member = value_->find(key);
        if (member == nullptr) {
            if (!required) {
                out = nullptr;
                return Status::success();
            }
            return fail(ErrorCode::MissingArgument, "required object member is missing", std::string(key));
        }
        if (!member->is_object()) {
            return fail(ErrorCode::MalformedInput, "member is not a JSON object", std::string(key));
        }
        out = member;
        return Status::success();
    }

    [[nodiscard]] Status array(std::string_view key, const json::Value*& out, bool required = true) {
        mark(key);
        const json::Value* member = value_->find(key);
        if (member == nullptr) {
            if (!required) {
                out = nullptr;
                return Status::success();
            }
            return fail(ErrorCode::MissingArgument, "required array member is missing", std::string(key));
        }
        if (!member->is_array()) {
            return fail(ErrorCode::MalformedInput, "member is not a JSON array", std::string(key));
        }
        out = member;
        return Status::success();
    }

    // Rejects every member this reader never asked for.
    [[nodiscard]] Status finish() const {
        const json::Value::Object* members = value_->as_object();
        if (members == nullptr) {
            return Status::success();
        }
        for (const auto& member : *members) {
            bool known = false;
            for (const auto& key : marked_) {
                if (key == member.first) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                return fail(ErrorCode::MalformedInput, "unknown member in this document", member.first, what_);
            }
        }
        return Status::success();
    }

private:
    void mark(std::string_view key) { marked_.emplace_back(key); }

    const json::Value* value_;
    std::string what_;
    std::vector<std::string> marked_;
};

[[nodiscard]] Status read_bounded(Fields& fields, std::string_view key, std::uint64_t limit, std::uint64_t& out) {
    std::int64_t raw = 0;
    if (auto status = fields.integer(key, raw); !status.ok()) {
        return status;
    }
    if (raw < 0) {
        return fail(ErrorCode::InvalidArgument, "value must not be negative", std::string(key));
    }
    const auto value = static_cast<std::uint64_t>(raw);
    if (value > limit) {
        return fail(ErrorCode::InvalidArgument, "value is out of range", std::string(key), std::to_string(value));
    }
    out = value;
    return Status::success();
}

[[nodiscard]] Status read_uint32(Fields& fields, std::string_view key, std::uint32_t& out) {
    std::uint64_t value = 0;
    if (auto status = read_bounded(fields, key, kMaxUint32Value, value); !status.ok()) {
        return status;
    }
    out = static_cast<std::uint32_t>(value);
    return Status::success();
}

[[nodiscard]] Status read_timestamp(Fields& fields, std::string_view key, Timestamp& out) {
    std::string text;
    if (auto status = fields.text(key, text); !status.ok()) {
        return status;
    }
    if (!parse_timestamp(text, out)) {
        return fail(ErrorCode::InvalidArgument, "value is not an RFC 3339 UTC timestamp", std::string(key), text);
    }
    return Status::success();
}

template <typename Tag>
[[nodiscard]] Status read_ident(Fields& fields, std::string_view key, Ident<Tag>& out) {
    std::string text;
    if (auto status = fields.text(key, text); !status.ok()) {
        return status;
    }
    auto parsed = Ident<Tag>::parse(text, key);
    if (!parsed.ok()) {
        return parsed.status();
    }
    out = parsed.value();
    return Status::success();
}

template <typename Enum, typename ParseFn>
[[nodiscard]] Status read_enum(Fields& fields, std::string_view key, Enum& out, ParseFn parse) {
    std::string text;
    if (auto status = fields.text(key, text); !status.ok()) {
        return status;
    }
    if (!parse(text, out)) {
        return fail(ErrorCode::InvalidArgument, "value is not a name this library knows", std::string(key), text);
    }
    return Status::success();
}

[[nodiscard]] Status read_targets(Fields& fields, std::string_view key, std::vector<TargetRef>& out) {
    const json::Value* array = nullptr;
    if (auto status = fields.array(key, array, false); !status.ok()) {
        return status;
    }
    if (array == nullptr) {
        return Status::success();
    }
    for (const auto& element : *array->as_array()) {
        const std::string* text = element.as_string();
        if (text == nullptr) {
            return fail(ErrorCode::MalformedInput, "a target must be a JSON string", std::string(key));
        }
        auto parsed = TargetRef::parse(*text);
        if (!parsed.ok()) {
            return parsed.status();
        }
        out.push_back(parsed.value());
    }
    return Status::success();
}

[[nodiscard]] Status read_ident_array(Fields& fields, std::string_view key, std::vector<AssetId>& out) {
    const json::Value* array = nullptr;
    if (auto status = fields.array(key, array, false); !status.ok()) {
        return status;
    }
    if (array == nullptr) {
        return Status::success();
    }
    for (const auto& element : *array->as_array()) {
        const std::string* text = element.as_string();
        if (text == nullptr) {
            return fail(ErrorCode::MalformedInput, "an asset identity must be a JSON string", std::string(key));
        }
        auto parsed = AssetId::parse(*text, key);
        if (!parsed.ok()) {
            return parsed.status();
        }
        out.push_back(parsed.value());
    }
    return Status::success();
}

template <typename T, typename Fn>
[[nodiscard]] Status read_items(Fields& fields, std::string_view key, std::vector<T>& out, Fn read_one) {
    const json::Value* array = nullptr;
    if (auto status = fields.array(key, array, false); !status.ok()) {
        return status;
    }
    if (array == nullptr) {
        return Status::success();
    }
    for (const auto& element : *array->as_array()) {
        out.emplace_back();
        if (auto status = read_one(element, out.back()); !status.ok()) {
            out.pop_back();
            return status;
        }
    }
    return Status::success();
}

[[nodiscard]] Status read_headroom(const json::Value& value, Headroom& out) {
    Fields fields(value, "headroom");
    if (auto status = fields.open(); !status.ok()) {
        return status;
    }
    std::int64_t available = 0;
    std::int64_t required = 0;
    if (auto status = fields.boolean("measured", out.measured); !status.ok()) {
        return status;
    }
    if (auto status = fields.integer("available_units", available); !status.ok()) {
        return status;
    }
    if (auto status = fields.integer("required_units", required); !status.ok()) {
        return status;
    }
    out.available_units = available;
    out.required_units = required;
    return fields.finish();
}

[[nodiscard]] Status read_policy(const json::Value& value, const PolicyGeneration& generation,
                                 PolicyDocument& out) {
    Fields fields(value, "policy");
    if (auto status = fields.open(); !status.ok()) {
        return status;
    }
    std::uint64_t raw = 0;
    if (auto status = read_ident<PolicyIdTag>(fields, "id", out.id); !status.ok()) {
        return status;
    }
    if (auto status = read_bounded(fields, "revision", kMaxUint32Value, raw); !status.ok()) {
        return status;
    }
    out.revision = Revision::from_value(raw);
    out.generation = generation;
    if (auto status = read_uint32(fields, "min_redundancy_margin_units", out.min_redundancy_margin_units);
        !status.ok()) {
        return status;
    }
    if (auto status = read_uint32(fields, "min_spare_capacity_units", out.min_spare_capacity_units); !status.ok()) {
        return status;
    }
    if (auto status = fields.integer("min_power_headroom_milliwatts", out.min_power_headroom_milliwatts);
        !status.ok()) {
        return status;
    }
    if (auto status = fields.integer("min_cooling_headroom_units", out.min_cooling_headroom_units); !status.ok()) {
        return status;
    }
    if (auto status = read_bounded(fields, "approval_validity_hours", kMaxHours, raw); !status.ok()) {
        return status;
    }
    out.approval_validity_nanos = static_cast<std::int64_t>(raw) * kNanosPerHour;
    if (auto status = read_bounded(fields, "max_window_hours", kMaxHours, raw); !status.ok()) {
        return status;
    }
    out.max_window_duration_nanos = static_cast<std::int64_t>(raw) * kNanosPerHour;
    if (auto status = read_uint32(fields, "max_concurrent_windows_per_rack", out.max_concurrent_windows_per_rack);
        !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("require_personnel_evidence", out.require_personnel_evidence); !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("require_dual_approval", out.require_dual_approval); !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("require_work_stop_evidence", out.require_work_stop_evidence); !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("require_restoration_evidence", out.require_restoration_evidence);
        !status.ok()) {
        return status;
    }
    return fields.finish();
}

[[nodiscard]] Status read_asset(const json::Value& value, AssetRecord& out) {
    Fields fields(value, "asset");
    if (auto status = fields.open(); !status.ok()) {
        return status;
    }
    std::uint64_t raw = 0;
    if (auto status = read_ident<AssetIdTag>(fields, "id", out.id); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<RackIdTag>(fields, "rack", out.rack); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<SiteIdTag>(fields, "site", out.site); !status.ok()) {
        return status;
    }
    if (auto status = read_enum<LifecycleState>(fields, "lifecycle", out.lifecycle, parse_lifecycle_state); !status.ok()) {
        return status;
    }
    if (auto status = read_enum<ServiceClass>(fields, "service_class", out.service_class, parse_service_class); !status.ok()) {
        return status;
    }
    if (auto status = read_bounded(fields, "lifecycle_generation", kMaxUint32Value, raw); !status.ok()) {
        return status;
    }
    out.lifecycle_generation = LifecycleGeneration::from_value(raw);
    if (auto status = read_bounded(fields, "hardware_generation", kMaxUint32Value, raw); !status.ok()) {
        return status;
    }
    out.hardware_generation = HardwareGeneration::from_value(raw);
    if (auto status = read_bounded(fields, "firmware_generation", kMaxUint32Value, raw); !status.ok()) {
        return status;
    }
    out.firmware_generation = FirmwareGeneration::from_value(raw);
    if (auto status = read_uint32(fields, "capacity_units", out.capacity_units); !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("isolatable", out.isolatable); !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("drainable", out.drainable); !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("healthy", out.healthy); !status.ok()) {
        return status;
    }
    return fields.finish();
}

[[nodiscard]] Status read_redundancy_group(const json::Value& value, RedundancyGroup& out) {
    Fields fields(value, "redundancy-group");
    if (auto status = fields.open(); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<RedundancyGroupIdTag>(fields, "id", out.id); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<SiteIdTag>(fields, "site", out.site); !status.ok()) {
        return status;
    }
    if (auto status = read_enum<RedundancyMode>(fields, "mode", out.mode, parse_redundancy_mode); !status.ok()) {
        return status;
    }
    if (auto status = read_uint32(fields, "required_units", out.required_units); !status.ok()) {
        return status;
    }
    if (auto status = read_uint32(fields, "observed_available_units", out.observed_available_units);
        !status.ok()) {
        return status;
    }
    if (auto status = read_ident_array(fields, "members", out.members); !status.ok()) {
        return status;
    }
    return fields.finish();
}

[[nodiscard]] Status read_capacity_pool(const json::Value& value, CapacityPool& out) {
    Fields fields(value, "capacity-pool");
    if (auto status = fields.open(); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<CapacityPoolIdTag>(fields, "id", out.id); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<SiteIdTag>(fields, "site", out.site); !status.ok()) {
        return status;
    }
    if (auto status = read_uint32(fields, "units_total", out.units_total); !status.ok()) {
        return status;
    }
    if (auto status = read_uint32(fields, "units_available", out.units_available); !status.ok()) {
        return status;
    }
    if (auto status = read_uint32(fields, "units_protected", out.units_protected); !status.ok()) {
        return status;
    }
    return fields.finish();
}

[[nodiscard]] Status read_power_domain(const json::Value& value, PowerDomain& out) {
    Fields fields(value, "power-domain");
    if (auto status = fields.open(); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<PowerDomainIdTag>(fields, "id", out.id); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<SiteIdTag>(fields, "site", out.site); !status.ok()) {
        return status;
    }
    const json::Value* headroom = nullptr;
    if (auto status = fields.object("headroom", headroom); !status.ok()) {
        return status;
    }
    if (auto status = read_headroom(*headroom, out.headroom); !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("interlocked", out.interlocked); !status.ok()) {
        return status;
    }
    if (auto status = fields.text("interlock_reason", out.interlock_reason); !status.ok()) {
        return status;
    }
    return fields.finish();
}

[[nodiscard]] Status read_cooling_zone(const json::Value& value, CoolingZone& out) {
    Fields fields(value, "cooling-zone");
    if (auto status = fields.open(); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<CoolingZoneIdTag>(fields, "id", out.id); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<SiteIdTag>(fields, "site", out.site); !status.ok()) {
        return status;
    }
    const json::Value* headroom = nullptr;
    if (auto status = fields.object("headroom", headroom); !status.ok()) {
        return status;
    }
    if (auto status = read_headroom(*headroom, out.headroom); !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("interlocked", out.interlocked); !status.ok()) {
        return status;
    }
    if (auto status = fields.text("interlock_reason", out.interlock_reason); !status.ok()) {
        return status;
    }
    return fields.finish();
}

[[nodiscard]] Status read_fabric_segment(const json::Value& value, FabricSegment& out) {
    Fields fields(value, "fabric-segment");
    if (auto status = fields.open(); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<FabricSegmentIdTag>(fields, "id", out.id); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<SiteIdTag>(fields, "site", out.site); !status.ok()) {
        return status;
    }
    if (auto status = read_uint32(fields, "available_paths", out.available_paths); !status.ok()) {
        return status;
    }
    if (auto status = read_uint32(fields, "required_paths", out.required_paths); !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("drainable", out.drainable); !status.ok()) {
        return status;
    }
    return fields.finish();
}

[[nodiscard]] Status read_blackout(const json::Value& value, BlackoutPeriod& out) {
    Fields fields(value, "blackout");
    if (auto status = fields.open(); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<BlackoutIdTag>(fields, "id", out.id); !status.ok()) {
        return status;
    }
    if (auto status = read_targets(fields, "targets", out.targets); !status.ok()) {
        return status;
    }
    if (auto status = read_timestamp(fields, "start", out.start); !status.ok()) {
        return status;
    }
    if (auto status = read_timestamp(fields, "end", out.end); !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("hard", out.hard); !status.ok()) {
        return status;
    }
    if (auto status = fields.text("reason", out.reason); !status.ok()) {
        return status;
    }
    return fields.finish();
}

[[nodiscard]] Status read_incident(const json::Value& value, IncidentRecord& out) {
    Fields fields(value, "incident");
    if (auto status = fields.open(); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<IncidentIdTag>(fields, "id", out.id); !status.ok()) {
        return status;
    }
    if (auto status = read_targets(fields, "targets", out.targets); !status.ok()) {
        return status;
    }
    if (auto status = read_enum<Severity>(fields, "severity", out.severity, parse_severity); !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("active", out.active); !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("hard_block", out.hard_block); !status.ok()) {
        return status;
    }
    if (auto status = fields.text("summary", out.summary); !status.ok()) {
        return status;
    }
    return fields.finish();
}

[[nodiscard]] Status read_protection(const json::Value& value, ProtectedObligation& out) {
    Fields fields(value, "protected-obligation");
    if (auto status = fields.open(); !status.ok()) {
        return status;
    }
    if (auto status = read_ident<ProtectionIdTag>(fields, "id", out.id); !status.ok()) {
        return status;
    }
    if (auto status = read_targets(fields, "targets", out.targets); !status.ok()) {
        return status;
    }
    if (auto status = read_enum<RedundancyMode>(fields, "required_mode", out.required_mode, parse_redundancy_mode); !status.ok()) {
        return status;
    }
    if (auto status = read_uint32(fields, "required_units", out.required_units); !status.ok()) {
        return status;
    }
    if (auto status = read_uint32(fields, "tolerance_units", out.tolerance_units); !status.ok()) {
        return status;
    }
    if (auto status = fields.boolean("hard_interlock", out.hard_interlock); !status.ok()) {
        return status;
    }
    if (auto status = fields.text("description", out.description); !status.ok()) {
        return status;
    }
    return fields.finish();
}

[[nodiscard]] Result<FacilitySnapshot> read_facility_document(const json::Value& value) {
    Fields fields(value, "facility");
    if (auto status = fields.open(); !status.ok()) {
        return status;
    }
    FacilitySnapshot facility;
    std::uint64_t raw = 0;
    if (auto status = read_bounded(fields, "revision", kMaxUint32Value, raw); !status.ok()) {
        return status;
    }
    facility.revision = Revision::from_value(raw);
    if (auto status = read_bounded(fields, "facility_epoch", kMaxUint32Value, raw); !status.ok()) {
        return status;
    }
    facility.facility_epoch = FacilityEpoch::from_value(raw);
    if (auto status = read_bounded(fields, "policy_generation", kMaxUint32Value, raw); !status.ok()) {
        return status;
    }
    facility.policy_generation = PolicyGeneration::from_value(raw);
    if (auto status = read_bounded(fields, "dependency_generation", kMaxUint32Value, raw); !status.ok()) {
        return status;
    }
    facility.dependency_generation = DependencyGeneration::from_value(raw);
    if (auto status = read_bounded(fields, "capacity_generation", kMaxUint32Value, raw); !status.ok()) {
        return status;
    }
    facility.capacity_generation = CapacityGeneration::from_value(raw);
    if (auto status = read_bounded(fields, "topology_generation", kMaxUint32Value, raw); !status.ok()) {
        return status;
    }
    facility.topology_generation = TopologyGeneration::from_value(raw);
    if (auto status = read_bounded(fields, "maintenance_generation", kMaxUint32Value, raw); !status.ok()) {
        return status;
    }
    facility.maintenance_generation = MaintenanceGeneration::from_value(raw);
    if (auto status = read_bounded(fields, "control_epoch", kMaxUint32Value, raw); !status.ok()) {
        return status;
    }
    facility.control_epoch = ControlEpoch::from_value(raw);
    if (auto status = read_timestamp(fields, "observed_at", facility.observed_at); !status.ok()) {
        return status;
    }

    const json::Value* policy = nullptr;
    if (auto status = fields.object("policy", policy); !status.ok()) {
        return status;
    }
    if (auto status = read_policy(*policy, facility.policy_generation, facility.policy); !status.ok()) {
        return status;
    }
    if (auto status = read_items(fields, "assets", facility.assets, read_asset); !status.ok()) {
        return status;
    }
    if (auto status = read_items(fields, "redundancy_groups", facility.redundancy_groups, read_redundancy_group);
        !status.ok()) {
        return status;
    }
    if (auto status = read_items(fields, "capacity_pools", facility.capacity_pools, read_capacity_pool);
        !status.ok()) {
        return status;
    }
    if (auto status = read_items(fields, "power_domains", facility.power_domains, read_power_domain);
        !status.ok()) {
        return status;
    }
    if (auto status = read_items(fields, "cooling_zones", facility.cooling_zones, read_cooling_zone);
        !status.ok()) {
        return status;
    }
    if (auto status = read_items(fields, "fabric_segments", facility.fabric_segments, read_fabric_segment);
        !status.ok()) {
        return status;
    }
    if (auto status = read_items(fields, "blackouts", facility.blackouts, read_blackout); !status.ok()) {
        return status;
    }
    if (auto status = read_items(fields, "incidents", facility.incidents, read_incident); !status.ok()) {
        return status;
    }
    if (auto status = read_items(fields, "protected_obligations", facility.protected_obligations,
                                 read_protection);
        !status.ok()) {
        return status;
    }
    if (auto status = fields.finish(); !status.ok()) {
        return status;
    }
    facility.finalize();
    return facility;
}

// ---------------------------------------------------------------------------
// Input documents
// ---------------------------------------------------------------------------

[[nodiscard]] Result<std::string> read_document(const std::string& path) {
    if (path == "-") {
        (void)_setmode(_fileno(stdin), _O_BINARY);
        std::string data;
        char buffer[8192];
        std::size_t count = std::fread(buffer, 1, sizeof(buffer), stdin);
        while (count > 0U) {
            data.append(buffer, count);
            if (data.size() > kMaxTextDocumentBytes) {
                return fail(ErrorCode::LimitExceeded, "the input document exceeds the accepted size", "file",
                            path);
            }
            count = std::fread(buffer, 1, sizeof(buffer), stdin);
        }
        if (std::ferror(stdin) != 0) {
            return fail(ErrorCode::IoError, "the input document could not be read", "file", path);
        }
        return data;
    }
    std::ifstream stream(to_path(path), std::ios::binary);
    if (!stream) {
        return fail(ErrorCode::IoError, "the input document could not be opened", "file", path);
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        return fail(ErrorCode::IoError, "the input document could not be sized", "file", path);
    }
    if (static_cast<std::uint64_t>(size) > kMaxTextDocumentBytes) {
        return fail(ErrorCode::LimitExceeded, "the input document exceeds the accepted size", "file", path);
    }
    stream.seekg(0, std::ios::beg);
    std::string data(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        stream.read(data.data(), size);
    }
    if (!stream) {
        return fail(ErrorCode::IoError, "the input document could not be read", "file", path);
    }
    return data;
}

[[nodiscard]] Result<FacilitySnapshot> read_facility_file(const std::string& path) {
    auto document = read_document(path);
    if (!document.ok()) {
        return document.status();
    }
    auto parsed = json::parse(*document, "facility");
    if (!parsed.ok()) {
        return parsed.status();
    }
    return read_facility_document(parsed.value());
}

// ---------------------------------------------------------------------------
// Store session: the store, the clock and the coordinator for one command.
// ---------------------------------------------------------------------------

class Session {
public:
    [[nodiscard]] static Result<std::unique_ptr<Session>> open(const Invocation& invocation) {
        StoreOptions options;
        options.directory = to_path(invocation.store);
        options.read_only = invocation.read_only;
        options.create_if_missing = true;
        auto opened = Store::open(options);
        if (!opened.ok()) {
            return opened.status();
        }
        auto session = std::unique_ptr<Session>(new Session());
        session->store_ = std::move(opened).value();
        if (invocation.have_now) {
            session->manual_clock_ = std::make_unique<ManualClock>(invocation.now);
        } else {
            session->system_clock_ = std::make_unique<SystemClock>();
        }
        const Clock& clock = session->manual_clock_ != nullptr ? static_cast<const Clock&>(*session->manual_clock_)
                                                              : static_cast<const Clock&>(*session->system_clock_);
        session->coordinator_ = std::make_unique<Coordinator>(*session->store_, clock);
        return session;
    }

    [[nodiscard]] Store& store() { return *store_; }
    [[nodiscard]] Coordinator& coordinator() { return *coordinator_; }

private:
    Session() = default;

    std::unique_ptr<ManualClock> manual_clock_;
    std::unique_ptr<SystemClock> system_clock_;
    std::unique_ptr<Store> store_;
    std::unique_ptr<Coordinator> coordinator_;
};

// ---------------------------------------------------------------------------
// Outcome of one command
// ---------------------------------------------------------------------------

struct CommandOutcome {
    std::string command;
    std::optional<CommandResult> result;
    std::optional<RecoverySummary> recovery;
    bool has_recovery{false};
    std::vector<AuditEntry> audit;
    bool has_audit{false};
    std::vector<std::string> notes;
};

[[nodiscard]] CommandOutcome make_outcome(const Invocation& invocation) {
    CommandOutcome outcome;
    outcome.command = invocation.command;
    return outcome;
}

// ---------------------------------------------------------------------------
// Command handlers.  Every one of these either opens the store and issues
// exactly one call, or fails before the store is touched.
// ---------------------------------------------------------------------------

[[nodiscard]] Status run_init(const Invocation& invocation, CommandOutcome& outcome) {
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    const std::filesystem::path directory = (*session)->store().directory();
    outcome.has_recovery = true;
    outcome.recovery = (*session)->store().recovery();
    outcome.notes.push_back("store: " + directory.string());
    return Status::success();
}

[[nodiscard]] Status run_audit(const Invocation& invocation, CommandOutcome& outcome) {
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto entries = (*session)->store().audit();
    if (!entries.ok()) {
        return entries.status();
    }
    outcome.has_recovery = true;
    outcome.recovery = (*session)->store().recovery();
    outcome.has_audit = true;
    outcome.audit = std::move(entries).value();
    return Status::success();
}

[[nodiscard]] Status run_install_facility(const Invocation& invocation, CommandOutcome& outcome) {
    const auto path = require_option(invocation.args, "file");
    if (!path.ok()) {
        return path.status();
    }
    // The whole document is read and validated before the store is touched.
    auto facility = read_facility_file(*path);
    if (!facility.ok()) {
        return facility.status();
    }
    InstallFacilityRequest request;
    request.facility = std::move(facility).value();
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().install_facility(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_propose(const Invocation& invocation, CommandOutcome& outcome) {
    const auto& args = invocation.args;
    ProposeRequest request;
    const auto reason = require_option(args, "reason");
    if (!reason.ok()) {
        return reason.status();
    }
    request.reason = *reason;
    const auto requester = ident_option<OperatorIdTag>(args, "requester");
    if (!requester.ok()) {
        return requester.status();
    }
    request.requested_by = requester.value().name();
    const auto activity =
        enum_option<MaintenanceActivity>(args, "activity", [](std::string_view text, MaintenanceActivity& out) {
            return parse_maintenance_activity(text, out);
        });
    if (!activity.ok()) {
        return activity.status();
    }
    request.activity = *activity;
    const auto priority = enum_option<PlanPriority>(args, "priority", [](std::string_view text, PlanPriority& out) {
        return parse_plan_priority(text, out);
    });
    if (!priority.ok()) {
        return priority.status();
    }
    request.priority = *priority;
    const auto risk = enum_option<ServiceRiskClass>(args, "risk", [](std::string_view text, ServiceRiskClass& out) {
        return parse_service_risk_class(text, out);
    });
    if (!risk.ok()) {
        return risk.status();
    }
    request.risk = *risk;
    auto scope = scope_option(args);
    if (!scope.ok()) {
        return scope.status();
    }
    request.scope = std::move(scope).value();
    const auto start = timestamp_option(args, "start");
    if (!start.ok()) {
        return start.status();
    }
    request.window.start = *start;
    const auto end = timestamp_option(args, "end");
    if (!end.ok()) {
        return end.status();
    }
    request.window.end = *end;
    request.window.flexible = args.has("flexible");
    if (const std::string* plan_id = args.find("plan-id"); plan_id != nullptr) {
        auto parsed = PlanId::parse(*plan_id, "plan-id");
        if (!parsed.ok()) {
            return parsed.status();
        }
        request.plan_id = parsed.value();
    }
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().propose(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

// Reads the --plan and --revision pair every mutating command after proposal
// shares.  The revision is parsed here so a bad value is an argument error
// rather than a stale-generation failure.
[[nodiscard]] Status read_plan_revision(const ArgList& args, PlanId& plan, Revision& revision) {
    const auto plan_id = ident_option<PlanIdTag>(args, "plan");
    if (!plan_id.ok()) {
        return plan_id.status();
    }
    plan = plan_id.value();
    std::uint64_t raw = 0;
    const auto text = require_option(args, "revision");
    if (!text.ok()) {
        return text.status();
    }
    if (auto status = parse_unsigned(*text, "revision", kMaxUint32Value, raw); !status.ok()) {
        return status;
    }
    revision = Revision::from_value(raw);
    return Status::success();
}

[[nodiscard]] Status run_evaluate(const Invocation& invocation, CommandOutcome& outcome) {
    EvaluateRequest request;
    if (auto status = read_plan_revision(invocation.args, request.plan, request.revision); !status.ok()) {
        return status;
    }
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().evaluate(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_approve(const Invocation& invocation, CommandOutcome& outcome) {
    ApproveRequest request;
    if (auto status = read_plan_revision(invocation.args, request.plan, request.revision); !status.ok()) {
        return status;
    }
    const auto approver = ident_option<OperatorIdTag>(invocation.args, "approver");
    if (!approver.ok()) {
        return approver.status();
    }
    request.approved_by = approver.value().name();
    const auto evidence = require_option(invocation.args, "evidence");
    if (!evidence.ok()) {
        return evidence.status();
    }
    request.approval_evidence = *evidence;
    if (invocation.args.has("witness")) {
        const auto witness = ident_option<OperatorIdTag>(invocation.args, "witness");
        if (!witness.ok()) {
            return witness.status();
        }
        request.witness = witness.value().name();
    }
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().approve(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_derive_obligations(const Invocation& invocation, CommandOutcome& outcome) {
    DeriveObligationsRequest request;
    if (auto status = read_plan_revision(invocation.args, request.plan, request.revision); !status.ok()) {
        return status;
    }
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().derive_obligations(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_ingest(const Invocation& invocation, CommandOutcome& outcome) {
    IngestReceiptRequest request;
    if (auto status = read_plan_revision(invocation.args, request.plan, request.revision); !status.ok()) {
        return status;
    }
    const auto receipt = ident_option<ReceiptIdTag>(invocation.args, "receipt");
    if (!receipt.ok()) {
        return receipt.status();
    }
    request.receipt_id = receipt.value();
    const auto obligation = ident_option<ObligationIdTag>(invocation.args, "obligation");
    if (!obligation.ok()) {
        return obligation.status();
    }
    request.obligation = obligation.value();
    const auto kind = enum_option<ReceiptKind>(invocation.args, "kind", [](std::string_view text, ReceiptKind& out) {
        return parse_receipt_kind(text, out);
    });
    if (!kind.ok()) {
        return kind.status();
    }
    request.kind = *kind;
    const auto authority =
        enum_option<ObligationAuthority>(invocation.args, "authority", [](std::string_view text, ObligationAuthority& out) {
            return parse_obligation_authority(text, out);
        });
    if (!authority.ok()) {
        return authority.status();
    }
    request.issuer_authority = *authority;
    const auto issuer = ident_option<OperatorIdTag>(invocation.args, "issuer");
    if (!issuer.ok()) {
        return issuer.status();
    }
    request.issuer = issuer.value().name();
    const auto sequence = unsigned_option(invocation.args, "sequence", kMaxUint32Value);
    if (!sequence.ok()) {
        return sequence.status();
    }
    request.observation_sequence = ObservationSequence::from_value(*sequence);
    const auto evidence = require_option(invocation.args, "evidence");
    if (!evidence.ok()) {
        return evidence.status();
    }
    request.evidence = *evidence;
    if (invocation.args.has("observed-at")) {
        const auto observed = timestamp_option(invocation.args, "observed-at");
        if (!observed.ok()) {
            return observed.status();
        }
        request.observed_at = *observed;
    } else {
        // Deterministic under --now; otherwise the host clock, which is the only
        // defensible "when did this happen" the tool has without an explicit
        // observation time.
        request.observed_at = invocation.have_now ? invocation.now : SystemClock{}.now_nanos();
    }
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().ingest_receipt(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_grant_exception(const Invocation& invocation, CommandOutcome& outcome) {
    GrantExceptionRequest request;
    if (auto status = read_plan_revision(invocation.args, request.plan, request.revision); !status.ok()) {
        return status;
    }
    auto waived = waived_options(invocation.args);
    if (!waived.ok()) {
        return waived.status();
    }
    request.waived = std::move(waived).value();
    auto targets = target_options(invocation.args, "target", true);
    if (!targets.ok()) {
        return targets.status();
    }
    request.targets = std::move(targets).value();
    std::sort(request.targets.begin(), request.targets.end());
    request.targets.erase(std::unique(request.targets.begin(), request.targets.end()), request.targets.end());
    const auto grantor = ident_option<OperatorIdTag>(invocation.args, "grantor");
    if (!grantor.ok()) {
        return grantor.status();
    }
    request.granted_by = grantor.value().name();
    const auto justification = require_option(invocation.args, "justification");
    if (!justification.ok()) {
        return justification.status();
    }
    request.justification = *justification;
    if (invocation.args.has("validity-hours")) {
        const auto hours = unsigned_option(invocation.args, "validity-hours", kMaxHours);
        if (!hours.ok()) {
            return hours.status();
        }
        request.validity_nanos = static_cast<std::int64_t>(*hours) * kNanosPerHour;
    }
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().grant_exception(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_begin(const Invocation& invocation, CommandOutcome& outcome) {
    BeginRequest request;
    if (auto status = read_plan_revision(invocation.args, request.plan, request.revision); !status.ok()) {
        return status;
    }
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().begin(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_progress(const Invocation& invocation, CommandOutcome& outcome) {
    RecordProgressRequest request;
    if (auto status = read_plan_revision(invocation.args, request.plan, request.revision); !status.ok()) {
        return status;
    }
    const auto kind = enum_option<ProgressKind>(invocation.args, "kind", [](std::string_view text, ProgressKind& out) {
        return parse_progress_kind(text, out);
    });
    if (!kind.ok()) {
        return kind.status();
    }
    request.kind = *kind;
    request.note = optional_option(invocation.args, "note");
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().record_progress(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_verify_restoration(const Invocation& invocation, CommandOutcome& outcome) {
    VerifyRequest request;
    if (auto status = read_plan_revision(invocation.args, request.plan, request.revision); !status.ok()) {
        return status;
    }
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().verify_restoration(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_complete(const Invocation& invocation, CommandOutcome& outcome) {
    CompleteRequest request;
    if (auto status = read_plan_revision(invocation.args, request.plan, request.revision); !status.ok()) {
        return status;
    }
    request.note = optional_option(invocation.args, "note");
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().complete(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_cancel(const Invocation& invocation, CommandOutcome& outcome) {
    CancelRequest request;
    if (auto status = read_plan_revision(invocation.args, request.plan, request.revision); !status.ok()) {
        return status;
    }
    request.reason = optional_option(invocation.args, "reason");
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().cancel(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_recover(const Invocation& invocation, CommandOutcome& outcome) {
    RecoverRequest request;
    if (invocation.args.has("plan")) {
        const auto plan = ident_option<PlanIdTag>(invocation.args, "plan");
        if (!plan.ok()) {
            return plan.status();
        }
        request.plan = plan.value();
    }
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().recover(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_compact(const Invocation& invocation, CommandOutcome& outcome) {
    CompactRequest request;
    if (auto status = apply_header(request, invocation); !status.ok()) {
        return status;
    }
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().compact(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_explain(const Invocation& invocation, CommandOutcome& outcome) {
    ExplainRequest request;
    const auto plan = ident_option<PlanIdTag>(invocation.args, "plan");
    if (!plan.ok()) {
        return plan.status();
    }
    request.plan = plan.value();
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().explain(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_show(const Invocation& invocation, CommandOutcome& outcome) {
    ShowRequest request;
    const auto plan = ident_option<PlanIdTag>(invocation.args, "plan");
    if (!plan.ok()) {
        return plan.status();
    }
    request.plan = plan.value();
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().show(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status run_list(const Invocation& invocation, CommandOutcome& outcome) {
    ListRequest request;
    request.include_terminal = !invocation.args.has("active-only");
    auto session = Session::open(invocation);
    if (!session.ok()) {
        return session.status();
    }
    auto result = (*session)->coordinator().list(request);
    if (!result.ok()) {
        return result.status();
    }
    outcome.result = std::move(result).value();
    return Status::success();
}

[[nodiscard]] Status dispatch(const Invocation& invocation, CommandOutcome& outcome) {
    const std::string& command = invocation.command;
    if (command == "init") {
        return run_init(invocation, outcome);
    }
    if (command == "install-facility") {
        return run_install_facility(invocation, outcome);
    }
    if (command == "propose") {
        return run_propose(invocation, outcome);
    }
    if (command == "evaluate") {
        return run_evaluate(invocation, outcome);
    }
    if (command == "approve") {
        return run_approve(invocation, outcome);
    }
    if (command == "derive-obligations") {
        return run_derive_obligations(invocation, outcome);
    }
    if (command == "ingest") {
        return run_ingest(invocation, outcome);
    }
    if (command == "grant-exception") {
        return run_grant_exception(invocation, outcome);
    }
    if (command == "begin") {
        return run_begin(invocation, outcome);
    }
    if (command == "progress") {
        return run_progress(invocation, outcome);
    }
    if (command == "verify-restoration") {
        return run_verify_restoration(invocation, outcome);
    }
    if (command == "complete") {
        return run_complete(invocation, outcome);
    }
    if (command == "cancel") {
        return run_cancel(invocation, outcome);
    }
    if (command == "recover") {
        return run_recover(invocation, outcome);
    }
    if (command == "explain") {
        return run_explain(invocation, outcome);
    }
    if (command == "show") {
        return run_show(invocation, outcome);
    }
    if (command == "list") {
        return run_list(invocation, outcome);
    }
    if (command == "audit") {
        return run_audit(invocation, outcome);
    }
    if (command == "compact") {
        return run_compact(invocation, outcome);
    }
    return fail(ErrorCode::UnknownCommand, "unknown command", "command", command);
}

// ---------------------------------------------------------------------------
// JSON encoding of library values.  Key order is fixed for every object so two
// identical runs produce byte-identical output.
// ---------------------------------------------------------------------------

[[nodiscard]] json::Value encode_recovery(const RecoverySummary& recovery) {
    json::Value value = json::Value::object();
    value.set("snapshot_loaded", jbool(recovery.snapshot_loaded));
    value.set("snapshot_sequence", jgeneration(recovery.snapshot_sequence));
    value.set("journal_records_replayed", jcount(recovery.journal_records_replayed));
    value.set("journal_bytes_truncated", jcount(recovery.journal_bytes_truncated));
    value.set("transactions_discarded", jcount(recovery.transactions_discarded));
    value.set("plans_recovered", jcount(recovery.plans_recovered));
    value.set("plans_requiring_recovery", jcount(recovery.plans_requiring_recovery));
    value.set("prior_incarnation", jgeneration(recovery.prior_incarnation));
    value.set("current_incarnation", jgeneration(recovery.current_incarnation));
    return value;
}

[[nodiscard]] json::Value encode_mutation(const MutationOutcome& mutation) {
    json::Value value = json::Value::object();
    value.set("intent_digest", jdigest(mutation.intent_digest));
    value.set("outcome_digest", jdigest(mutation.outcome_digest));
    value.set("sequence", jgeneration(mutation.sequence));
    value.set("applied_at", jtime(mutation.applied_at));
    value.set("replayed", jbool(mutation.replayed));
    return value;
}

[[nodiscard]] json::Value encode_window(const WindowSpec& window) {
    json::Value value = json::Value::object();
    value.set("start", jtime(window.start));
    value.set("end", jtime(window.end));
    value.set("flexible", jbool(window.flexible));
    return value;
}

[[nodiscard]] json::Value encode_targets(const std::vector<TargetRef>& targets) {
    json::Value array = json::Value::array();
    for (const auto& target : targets) {
        array.push(jstring(target.str()));
    }
    return array;
}

[[nodiscard]] json::Value encode_scope(const MaintenanceScope& scope) { return encode_targets(scope.targets()); }

[[nodiscard]] json::Value encode_plan_intent(const MaintenancePlan& plan) {
    json::Value value = json::Value::object();
    value.set("id", jident(plan.id));
    value.set("revision", jgeneration(plan.revision));
    value.set("reason", jtext(plan.reason));
    value.set("requested_by", jtext(plan.requested_by));
    value.set("activity", jstring(to_string(plan.activity)));
    value.set("priority", jstring(to_string(plan.priority)));
    value.set("risk", jstring(to_string(plan.risk)));
    value.set("targets", encode_scope(plan.scope));
    value.set("window", encode_window(plan.window));
    value.set("facility_epoch", jgeneration(plan.facility_epoch));
    value.set("control_epoch", jgeneration(plan.control_epoch));
    value.set("policy_generation", jgeneration(plan.policy_generation));
    value.set("dependency_generation", jgeneration(plan.dependency_generation));
    value.set("capacity_generation", jgeneration(plan.capacity_generation));
    value.set("topology_generation", jgeneration(plan.topology_generation));
    value.set("maintenance_generation", jgeneration(plan.maintenance_generation));
    value.set("policy_digest", jdigest(plan.policy_digest));
    value.set("dependency_digest", jdigest(plan.dependency_digest));
    value.set("digest", jdigest(plan.digest));
    return value;
}

[[nodiscard]] json::Value encode_condition(const ConditionResult& condition) {
    json::Value value = json::Value::object();
    value.set("condition", jstring(to_string(condition.condition)));
    value.set("satisfied", jbool(condition.satisfied));
    value.set("hard", jbool(condition.hard));
    value.set("waivable", jbool(condition.waivable));
    value.set("measured", jbool(condition.measured));
    value.set("waiver", jident(condition.waiver));
    value.set("subject", jtext(condition.subject));
    value.set("detail", jtext(condition.detail));
    value.set("observed", jint(condition.observed));
    value.set("required", jint(condition.required));
    return value;
}

[[nodiscard]] json::Value encode_conditions(const std::vector<ConditionResult>& conditions) {
    json::Value array = json::Value::array();
    for (const auto& condition : conditions) {
        array.push(encode_condition(condition));
    }
    return array;
}

[[nodiscard]] json::Value encode_evaluation(const PreconditionReport& report) {
    json::Value value = json::Value::object();
    value.set("plan_revision", jgeneration(report.plan_revision));
    value.set("plan_digest", jdigest(report.plan_digest));
    value.set("evaluated_at", jtime(report.evaluated_at));
    value.set("facility_epoch", jgeneration(report.facility_epoch));
    value.set("control_epoch", jgeneration(report.control_epoch));
    value.set("policy_generation", jgeneration(report.policy_generation));
    value.set("dependency_generation", jgeneration(report.dependency_generation));
    value.set("capacity_generation", jgeneration(report.capacity_generation));
    value.set("topology_generation", jgeneration(report.topology_generation));
    value.set("maintenance_generation", jgeneration(report.maintenance_generation));
    value.set("facility_digest", jdigest(report.facility_digest));
    value.set("policy_digest", jdigest(report.policy_digest));
    value.set("dependency_digest", jdigest(report.dependency_digest));
    value.set("satisfied", jbool(report.satisfied));
    value.set("satisfied_without_exceptions", jbool(report.satisfied_without_exceptions));
    value.set("applied_exceptions", jident_array(report.applied_exceptions));
    value.set("conditions", encode_conditions(report.conditions));
    value.set("digest", jdigest(report.digest));
    return value;
}

[[nodiscard]] json::Value encode_approval(const ApprovalRecord& approval) {
    json::Value value = json::Value::object();
    value.set("id", jident(approval.id));
    value.set("plan", jident(approval.plan));
    value.set("plan_revision", jgeneration(approval.plan_revision));
    value.set("approved_by", jtext(approval.approved_by));
    value.set("approval_evidence", jtext(approval.approval_evidence));
    value.set("witness", jtext(approval.witness));
    value.set("approved_at", jtime(approval.approved_at));
    value.set("expires_at", jtime(approval.expires_at));
    value.set("plan_digest", jdigest(approval.plan_digest));
    value.set("facility_digest", jdigest(approval.facility_digest));
    value.set("policy_digest", jdigest(approval.policy_digest));
    value.set("dependency_digest", jdigest(approval.dependency_digest));
    value.set("evaluation_digest", jdigest(approval.evaluation_digest));
    value.set("obligation_set_digest", jdigest(approval.obligation_set_digest));
    value.set("facility_epoch", jgeneration(approval.facility_epoch));
    value.set("control_epoch", jgeneration(approval.control_epoch));
    value.set("policy_generation", jgeneration(approval.policy_generation));
    value.set("dependency_generation", jgeneration(approval.dependency_generation));
    value.set("capacity_generation", jgeneration(approval.capacity_generation));
    value.set("topology_generation", jgeneration(approval.topology_generation));
    value.set("maintenance_generation", jgeneration(approval.maintenance_generation));
    value.set("exceptions", jident_array(approval.exceptions));
    value.set("digest", jdigest(approval.digest));
    return value;
}

[[nodiscard]] json::Value encode_obligation(const Obligation& obligation) {
    json::Value value = json::Value::object();
    value.set("id", jident(obligation.id));
    value.set("kind", jstring(to_string(obligation.kind)));
    value.set("authority", jstring(to_string(obligation.authority)));
    value.set("stage", jstring(to_string(stage_of(obligation.kind))));
    value.set("target", jstring(obligation.target.str()));
    value.set("requirement", jtext(obligation.requirement));
    value.set("mandatory", jbool(obligation.mandatory));
    value.set("requires_observation", jbool(obligation.requires_observation));
    value.set("guarded_by", jstring(to_string(obligation.guarded_by)));
    value.set("digest", jdigest(obligation.digest));
    return value;
}

[[nodiscard]] json::Value encode_obligation_set(const ObligationSet& set) {
    json::Value value = json::Value::object();
    value.set("digest", jdigest(set.digest));
    json::Value items = json::Value::array();
    for (const auto& obligation : set.obligations) {
        items.push(encode_obligation(obligation));
    }
    value.set("items", std::move(items));
    return value;
}

[[nodiscard]] json::Value encode_obligation_status(const ObligationStatus& status) {
    json::Value value = json::Value::object();
    value.set("id", jident(status.id));
    value.set("kind", jstring(to_string(status.kind)));
    value.set("authority", jstring(to_string(status.authority)));
    value.set("stage", jstring(to_string(status.stage)));
    value.set("disposition", jstring(to_string(status.disposition)));
    value.set("mandatory", jbool(status.mandatory));
    value.set("target", jstring(status.target.str()));
    value.set("detail", jtext(status.detail));
    value.set("supporting_receipts", jident_array(status.supporting_receipts));
    value.set("waiver", jident(status.waiver));
    return value;
}

[[nodiscard]] json::Value encode_obligations(const ObligationStatusReport& report) {
    json::Value value = json::Value::object();
    value.set("plan_revision", jgeneration(report.plan_revision));
    value.set("obligation_set_digest", jdigest(report.obligation_set_digest));
    value.set("all_satisfied", jbool(report.all_satisfied));
    value.set("ready_for_pre_drain", jbool(report.ready_for_pre_drain));
    value.set("ready_for_isolation", jbool(report.ready_for_isolation));
    value.set("ready_for_completion", jbool(report.ready_for_completion));
    json::Value statuses = json::Value::array();
    for (const auto& status : report.statuses) {
        statuses.push(encode_obligation_status(status));
    }
    value.set("statuses", std::move(statuses));
    value.set("digest", jdigest(report.digest));
    return value;
}

[[nodiscard]] json::Value encode_receipt(const Receipt& receipt) {
    json::Value value = json::Value::object();
    value.set("id", jident(receipt.id));
    value.set("obligation", jident(receipt.obligation));
    value.set("kind", jstring(to_string(receipt.kind)));
    value.set("issuer_authority", jstring(to_string(receipt.issuer_authority)));
    value.set("issuer", jtext(receipt.issuer));
    value.set("observation_sequence", jgeneration(receipt.observation_sequence));
    value.set("plan_revision", jgeneration(receipt.plan_revision));
    value.set("obligation_digest", jdigest(receipt.obligation_digest));
    value.set("evidence_digest", jdigest(receipt.evidence_digest));
    value.set("evidence", jtext(receipt.evidence));
    value.set("observed_at", jtime(receipt.observed_at));
    value.set("ingested_at", jtime(receipt.ingested_at));
    value.set("digest", jdigest(receipt.digest));
    return value;
}

[[nodiscard]] json::Value encode_exception(const ExceptionGrant& grant) {
    json::Value value = json::Value::object();
    value.set("id", jident(grant.id));
    value.set("plan", jident(grant.plan));
    value.set("waived", jstring_array<ErrorCode>(grant.waived, [](ErrorCode code) { return to_string(code); }));
    value.set("targets", encode_targets(grant.targets));
    value.set("granted_by", jtext(grant.granted_by));
    value.set("justification", jtext(grant.justification));
    value.set("granted_at", jtime(grant.granted_at));
    value.set("expires_at", jtime(grant.expires_at));
    value.set("digest", jdigest(grant.digest));
    return value;
}

[[nodiscard]] json::Value encode_progress(const ProgressEvent& event) {
    json::Value value = json::Value::object();
    value.set("kind", jstring(to_string(event.kind)));
    value.set("note", jtext(event.note));
    value.set("actor", jtext(event.actor));
    value.set("at", jtime(event.at));
    value.set("digest", jdigest(event.digest));
    return value;
}

[[nodiscard]] json::Value encode_transition(const PhaseTransition& transition) {
    json::Value value = json::Value::object();
    value.set("from", jstring(to_string(transition.from)));
    value.set("to", jstring(to_string(transition.to)));
    value.set("at", jtime(transition.at));
    value.set("actor", jtext(transition.actor));
    value.set("reason", jtext(transition.reason));
    value.set("cause", jstring(to_string(transition.cause)));
    value.set("sequence", jgeneration(transition.sequence));
    value.set("digest", jdigest(transition.digest));
    return value;
}

[[nodiscard]] json::Value encode_plan_record(const PlanRecord& record) {
    json::Value value = json::Value::object();
    value.set("plan", encode_plan_intent(record.plan));
    value.set("phase", jstring(to_string(record.phase)));
    json::Value history = json::Value::array();
    for (const auto& revision : record.history) {
        history.push(encode_plan_intent(revision));
    }
    value.set("history", std::move(history));
    if (record.evaluation.has_value()) {
        value.set("evaluation", encode_evaluation(*record.evaluation));
    }
    if (record.approval.has_value()) {
        value.set("approval", encode_approval(*record.approval));
    }
    value.set("obligations", encode_obligation_set(record.obligations));
    json::Value receipts = json::Value::array();
    for (const auto& receipt : record.receipts) {
        receipts.push(encode_receipt(receipt));
    }
    value.set("receipts", std::move(receipts));
    json::Value progress = json::Value::array();
    for (const auto& event : record.progress) {
        progress.push(encode_progress(event));
    }
    value.set("progress", std::move(progress));
    json::Value exceptions = json::Value::array();
    for (const auto& grant : record.exceptions) {
        exceptions.push(encode_exception(grant));
    }
    value.set("exceptions", std::move(exceptions));
    json::Value transitions = json::Value::array();
    for (const auto& transition : record.transitions) {
        transitions.push(encode_transition(transition));
    }
    value.set("transitions", std::move(transitions));
    value.set("blockers", encode_conditions(record.blockers));
    value.set("last_observation_sequence", jgeneration(record.last_observation_sequence));
    value.set("recovery_mark", jgeneration(record.recovery_mark));
    value.set("recovery_required", jbool(record.recovery_required));
    value.set("work_completed", jbool(record.work_completed()));
    value.set("work_stopped", jbool(record.work_stopped()));
    value.set("created_at", jtime(record.created_at));
    value.set("updated_at", jtime(record.updated_at));
    value.set("sequence", jgeneration(record.sequence));
    value.set("digest", jdigest(record.digest));
    return value;
}

[[nodiscard]] json::Value encode_plan_summary(const PlanSummary& summary) {
    json::Value value = json::Value::object();
    value.set("id", jident(summary.id));
    value.set("revision", jgeneration(summary.revision));
    value.set("phase", jstring(to_string(summary.phase)));
    value.set("activity", jstring(to_string(summary.activity)));
    value.set("priority", jstring(to_string(summary.priority)));
    value.set("risk", jstring(to_string(summary.risk)));
    value.set("window", encode_window(summary.window));
    value.set("target_count", jcount(summary.target_count));
    value.set("recovery_required", jbool(summary.recovery_required));
    value.set("updated_at", jtime(summary.updated_at));
    return value;
}

[[nodiscard]] json::Value encode_restoration(const RestorationReport& report) {
    json::Value value = json::Value::object();
    value.set("plan", jident(report.plan));
    value.set("plan_revision", jgeneration(report.plan_revision));
    value.set("verified_at", jtime(report.verified_at));
    value.set("outstanding", jident_array(report.outstanding));
    value.set("violated_protections", jident_array(report.violated_protections));
    value.set("redundancy_below_requirement", jident_array(report.redundancy_below_requirement));
    value.set("conditions", encode_conditions(report.conditions));
    value.set("restored", jbool(report.restored));
    value.set("digest", jdigest(report.digest));
    return value;
}

[[nodiscard]] json::Value encode_completion(const CompletionReport& report) {
    json::Value value = json::Value::object();
    value.set("plan", jident(report.plan));
    value.set("plan_revision", jgeneration(report.plan_revision));
    value.set("completed_at", jtime(report.completed_at));
    value.set("sequence", jgeneration(report.sequence));
    value.set("plan_digest", jdigest(report.plan_digest));
    value.set("approval_digest", jdigest(report.approval_digest));
    value.set("obligation_set_digest", jdigest(report.obligation_set_digest));
    value.set("receipt_set_digest", jdigest(report.receipt_set_digest));
    value.set("facility_digest", jdigest(report.facility_digest));
    value.set("policy_digest", jdigest(report.policy_digest));
    value.set("dependency_digest", jdigest(report.dependency_digest));
    value.set("satisfied", jident_array(report.satisfied));
    value.set("protected_obligations_satisfied", jbool(report.protected_obligations_satisfied));
    value.set("restoration_verified", jbool(report.restoration_verified));
    value.set("digest", jdigest(report.digest));
    return value;
}

[[nodiscard]] json::Value encode_recovery_report(const RecoveryReport& report) {
    json::Value value = json::Value::object();
    value.set("plan", jident(report.plan));
    value.set("phase_before", jstring(to_string(report.phase_before)));
    value.set("phase_after", jstring(to_string(report.phase_after)));
    value.set("recovered_at", jtime(report.recovered_at));
    value.set("prior_incarnation", jgeneration(report.prior_incarnation));
    value.set("current_incarnation", jgeneration(report.current_incarnation));
    json::Value notes = json::Value::array();
    for (const auto& note : report.notes) {
        notes.push(jtext(note));
    }
    value.set("notes", std::move(notes));
    value.set("outstanding", jident_array(report.outstanding));
    value.set("recovery_required_after", jbool(report.recovery_required_after));
    value.set("digest", jdigest(report.digest));
    return value;
}

[[nodiscard]] json::Value encode_explanation(const Explanation& explanation) {
    json::Value value = json::Value::object();
    value.set("plan", jident(explanation.plan));
    value.set("revision", jgeneration(explanation.revision));
    value.set("phase", jstring(to_string(explanation.phase)));
    value.set("blockers", encode_conditions(explanation.blockers));
    json::Value reasons = json::Value::array();
    for (const auto& reason : explanation.reasons) {
        reasons.push(jtext(reason));
    }
    value.set("reasons", std::move(reasons));
    json::Value obligations = json::Value::array();
    for (const auto& status : explanation.obligations) {
        obligations.push(encode_obligation_status(status));
    }
    value.set("obligations", std::move(obligations));
    json::Value next_steps = json::Value::array();
    for (const auto& step : explanation.next_steps) {
        next_steps.push(jtext(step));
    }
    value.set("next_steps", std::move(next_steps));
    value.set("can_evaluate", jbool(explanation.can_evaluate));
    value.set("can_approve", jbool(explanation.can_approve));
    value.set("can_begin", jbool(explanation.can_begin));
    value.set("can_complete", jbool(explanation.can_complete));
    value.set("can_cancel", jbool(explanation.can_cancel));
    value.set("digest", jdigest(explanation.digest));
    return value;
}

[[nodiscard]] json::Value encode_policy(const PolicyDocument& policy) {
    json::Value value = json::Value::object();
    value.set("id", jident(policy.id));
    value.set("revision", jgeneration(policy.revision));
    value.set("generation", jgeneration(policy.generation));
    value.set("min_redundancy_margin_units", jcount(policy.min_redundancy_margin_units));
    value.set("min_spare_capacity_units", jcount(policy.min_spare_capacity_units));
    value.set("min_power_headroom_milliwatts", jint(policy.min_power_headroom_milliwatts));
    value.set("min_cooling_headroom_units", jint(policy.min_cooling_headroom_units));
    value.set("approval_validity_hours", jint(policy.approval_validity_nanos / kNanosPerHour));
    value.set("max_window_hours", jint(policy.max_window_duration_nanos / kNanosPerHour));
    value.set("max_concurrent_windows_per_rack", jcount(policy.max_concurrent_windows_per_rack));
    value.set("require_personnel_evidence", jbool(policy.require_personnel_evidence));
    value.set("require_dual_approval", jbool(policy.require_dual_approval));
    value.set("require_work_stop_evidence", jbool(policy.require_work_stop_evidence));
    value.set("require_restoration_evidence", jbool(policy.require_restoration_evidence));
    value.set("digest", jdigest(policy.compute_digest()));
    return value;
}

[[nodiscard]] json::Value encode_facility(const FacilitySnapshot& facility) {
    json::Value value = json::Value::object();
    value.set("revision", jgeneration(facility.revision));
    value.set("facility_epoch", jgeneration(facility.facility_epoch));
    value.set("policy_generation", jgeneration(facility.policy_generation));
    value.set("dependency_generation", jgeneration(facility.dependency_generation));
    value.set("capacity_generation", jgeneration(facility.capacity_generation));
    value.set("topology_generation", jgeneration(facility.topology_generation));
    value.set("maintenance_generation", jgeneration(facility.maintenance_generation));
    value.set("control_epoch", jgeneration(facility.control_epoch));
    value.set("observed_at", jtime(facility.observed_at));
    value.set("digest", jdigest(facility.digest));
    value.set("dependency_digest", jdigest(facility.dependency_digest));
    value.set("policy", encode_policy(facility.policy));

    json::Value assets = json::Value::array();
    for (const auto& asset : facility.assets) {
        json::Value item = json::Value::object();
        item.set("id", jident(asset.id));
        item.set("rack", jident(asset.rack));
        item.set("site", jident(asset.site));
        item.set("lifecycle", jstring(to_string(asset.lifecycle)));
        item.set("service_class", jstring(to_string(asset.service_class)));
        item.set("lifecycle_generation", jgeneration(asset.lifecycle_generation));
        item.set("hardware_generation", jgeneration(asset.hardware_generation));
        item.set("firmware_generation", jgeneration(asset.firmware_generation));
        item.set("capacity_units", jcount(asset.capacity_units));
        item.set("isolatable", jbool(asset.isolatable));
        item.set("drainable", jbool(asset.drainable));
        item.set("healthy", jbool(asset.healthy));
        assets.push(std::move(item));
    }
    value.set("assets", std::move(assets));

    json::Value groups = json::Value::array();
    for (const auto& group : facility.redundancy_groups) {
        json::Value item = json::Value::object();
        item.set("id", jident(group.id));
        item.set("site", jident(group.site));
        item.set("mode", jstring(to_string(group.mode)));
        item.set("required_units", jcount(group.required_units));
        item.set("observed_available_units", jcount(group.observed_available_units));
        item.set("members", jident_array(group.members));
        groups.push(std::move(item));
    }
    value.set("redundancy_groups", std::move(groups));

    json::Value pools = json::Value::array();
    for (const auto& pool : facility.capacity_pools) {
        json::Value item = json::Value::object();
        item.set("id", jident(pool.id));
        item.set("site", jident(pool.site));
        item.set("units_total", jcount(pool.units_total));
        item.set("units_available", jcount(pool.units_available));
        item.set("units_protected", jcount(pool.units_protected));
        pools.push(std::move(item));
    }
    value.set("capacity_pools", std::move(pools));

    json::Value power = json::Value::array();
    for (const auto& domain : facility.power_domains) {
        json::Value item = json::Value::object();
        item.set("id", jident(domain.id));
        item.set("site", jident(domain.site));
        json::Value headroom = json::Value::object();
        headroom.set("measured", jbool(domain.headroom.measured));
        headroom.set("available_units", jint(domain.headroom.available_units));
        headroom.set("required_units", jint(domain.headroom.required_units));
        item.set("headroom", std::move(headroom));
        item.set("interlocked", jbool(domain.interlocked));
        item.set("interlock_reason", jtext(domain.interlock_reason));
        power.push(std::move(item));
    }
    value.set("power_domains", std::move(power));

    json::Value cooling = json::Value::array();
    for (const auto& zone : facility.cooling_zones) {
        json::Value item = json::Value::object();
        item.set("id", jident(zone.id));
        item.set("site", jident(zone.site));
        json::Value headroom = json::Value::object();
        headroom.set("measured", jbool(zone.headroom.measured));
        headroom.set("available_units", jint(zone.headroom.available_units));
        headroom.set("required_units", jint(zone.headroom.required_units));
        item.set("headroom", std::move(headroom));
        item.set("interlocked", jbool(zone.interlocked));
        item.set("interlock_reason", jtext(zone.interlock_reason));
        cooling.push(std::move(item));
    }
    value.set("cooling_zones", std::move(cooling));

    json::Value fabric = json::Value::array();
    for (const auto& segment : facility.fabric_segments) {
        json::Value item = json::Value::object();
        item.set("id", jident(segment.id));
        item.set("site", jident(segment.site));
        item.set("available_paths", jcount(segment.available_paths));
        item.set("required_paths", jcount(segment.required_paths));
        item.set("drainable", jbool(segment.drainable));
        fabric.push(std::move(item));
    }
    value.set("fabric_segments", std::move(fabric));

    json::Value blackouts = json::Value::array();
    for (const auto& blackout : facility.blackouts) {
        json::Value item = json::Value::object();
        item.set("id", jident(blackout.id));
        item.set("targets", encode_targets(blackout.targets));
        item.set("start", jtime(blackout.start));
        item.set("end", jtime(blackout.end));
        item.set("hard", jbool(blackout.hard));
        item.set("reason", jtext(blackout.reason));
        blackouts.push(std::move(item));
    }
    value.set("blackouts", std::move(blackouts));

    json::Value incidents = json::Value::array();
    for (const auto& incident : facility.incidents) {
        json::Value item = json::Value::object();
        item.set("id", jident(incident.id));
        item.set("targets", encode_targets(incident.targets));
        item.set("severity", jstring(to_string(incident.severity)));
        item.set("active", jbool(incident.active));
        item.set("hard_block", jbool(incident.hard_block));
        item.set("summary", jtext(incident.summary));
        incidents.push(std::move(item));
    }
    value.set("incidents", std::move(incidents));

    json::Value protections = json::Value::array();
    for (const auto& protection : facility.protected_obligations) {
        json::Value item = json::Value::object();
        item.set("id", jident(protection.id));
        item.set("targets", encode_targets(protection.targets));
        item.set("required_mode", jstring(to_string(protection.required_mode)));
        item.set("required_units", jcount(protection.required_units));
        item.set("tolerance_units", jcount(protection.tolerance_units));
        item.set("hard_interlock", jbool(protection.hard_interlock));
        item.set("description", jtext(protection.description));
        protections.push(std::move(item));
    }
    value.set("protected_obligations", std::move(protections));
    return value;
}

[[nodiscard]] json::Value encode_audit_entry(const AuditEntry& entry) {
    json::Value value = json::Value::object();
    value.set("kind", jstring(to_string(entry.kind)));
    value.set("sequence", jgeneration(entry.sequence));
    value.set("offset", jcount(entry.offset));
    value.set("payload_bytes", jcount(entry.payload_bytes));
    value.set("plan", jident(entry.plan));
    value.set("payload_digest", jdigest(entry.payload_digest));
    return value;
}

// The commands whose result carries a mutation record.  The read-only commands
// have no attempt identity, so a mutation member would be a fabricated fact.
[[nodiscard]] bool command_mutates(std::string_view command) {
    return command == "install-facility" || command == "propose" || command == "evaluate" ||
           command == "approve" || command == "derive-obligations" || command == "ingest" ||
           command == "grant-exception" || command == "begin" || command == "progress" ||
           command == "verify-restoration" || command == "complete" || command == "cancel" ||
           command == "recover" || command == "compact";
}

[[nodiscard]] json::Value encode_outcome(const CommandOutcome& outcome) {
    json::Value value = json::Value::object();
    value.set("ok", jbool(true));
    value.set("command", jtext(outcome.command));

    const CommandResult* result = outcome.result.has_value() ? &*outcome.result : nullptr;

    if (result != nullptr && command_mutates(outcome.command)) {
        value.set("mutation", encode_mutation(result->mutation));
    }

    json::Value store = json::Value::object();
    if (result != nullptr) {
        store.set("recovery", encode_recovery(result->store_recovery));
    } else if (outcome.has_recovery) {
        store.set("recovery", encode_recovery(*outcome.recovery));
    }
    if (outcome.has_audit) {
        json::Value entries = json::Value::array();
        for (const auto& entry : outcome.audit) {
            entries.push(encode_audit_entry(entry));
        }
        store.set("audit", std::move(entries));
    }
    value.set("store", std::move(store));

    if (result != nullptr) {
        if (result->has_plan) {
            value.set("plan", encode_plan_record(result->plan));
        }
        if (!result->plans.empty()) {
            json::Value plans = json::Value::array();
            for (const auto& summary : result->plans) {
                plans.push(encode_plan_summary(summary));
            }
            value.set("plans", std::move(plans));
        }
        if (result->facility.has_value()) {
            value.set("facility", encode_facility(*result->facility));
        }
        if (result->evaluation.has_value()) {
            value.set("evaluation", encode_evaluation(*result->evaluation));
        }
        if (result->approval.has_value()) {
            value.set("approval", encode_approval(*result->approval));
        }
        if (result->obligations.has_value()) {
            value.set("obligations", encode_obligations(*result->obligations));
        }
        if (result->receipt.has_value()) {
            value.set("receipt", encode_receipt(*result->receipt));
        }
        if (result->exception.has_value()) {
            value.set("exception", encode_exception(*result->exception));
        }
        if (result->restoration.has_value()) {
            value.set("restoration", encode_restoration(*result->restoration));
        }
        if (result->completion.has_value()) {
            value.set("completion", encode_completion(*result->completion));
        }
        if (result->recovery.has_value()) {
            value.set("recovery_report", encode_recovery_report(*result->recovery));
        }
        if (result->explanation.has_value()) {
            value.set("explanation", encode_explanation(*result->explanation));
        }
    }

    json::Value notes = json::Value::array();
    if (result != nullptr) {
        for (const auto& note : result->notes) {
            notes.push(jtext(note));
        }
    }
    for (const auto& note : outcome.notes) {
        notes.push(jtext(note));
    }
    if (notes.size() != 0U) {
        value.set("notes", std::move(notes));
    }
    return value;
}

[[nodiscard]] json::Value encode_error(const Status& status) {
    json::Value value = json::Value::object();
    value.set("code", jstring(to_string(status.code())));
    value.set("message", jtext(status.message()));
    value.set("subject", jtext(status.subject()));
    value.set("detail", jtext(status.detail()));
    json::Value suppressed = json::Value::array();
    for (const auto& inner : status.suppressed()) {
        json::Value item = json::Value::object();
        item.set("code", jstring(to_string(inner.code())));
        item.set("message", jtext(inner.message()));
        suppressed.push(std::move(item));
    }
    value.set("suppressed", std::move(suppressed));
    return value;
}

[[nodiscard]] json::Value encode_failure(const std::string& command, const Status& status) {
    json::Value value = json::Value::object();
    value.set("ok", jbool(false));
    value.set("command", jtext(command));
    value.set("error", encode_error(status));
    return value;
}

// ---------------------------------------------------------------------------
// Human rendering: one fact per line, fixed order.
// ---------------------------------------------------------------------------

void add_line(std::string& out, std::string_view key, const std::string& value) {
    out += key;
    out += ": ";
    out += value;
    out += '\n';
}

[[nodiscard]] std::string render_recovery(const RecoverySummary& recovery) {
    std::string text;
    text += "snapshot-loaded=" + std::string(recovery.snapshot_loaded ? "true" : "false");
    text += " snapshot-sequence=" +
            (recovery.snapshot_sequence.is_set() ? std::to_string(recovery.snapshot_sequence.value()) : "unset");
    text += " journal-records-replayed=" + std::to_string(recovery.journal_records_replayed);
    text += " journal-bytes-truncated=" + std::to_string(recovery.journal_bytes_truncated);
    text += " transactions-discarded=" + std::to_string(recovery.transactions_discarded);
    text += " plans-recovered=" + std::to_string(recovery.plans_recovered);
    text += " plans-requiring-recovery=" + std::to_string(recovery.plans_requiring_recovery);
    text += " prior-incarnation=" +
            (recovery.prior_incarnation.is_set() ? std::to_string(recovery.prior_incarnation.value()) : "unset");
    text += " current-incarnation=" + (recovery.current_incarnation.is_set()
                                           ? std::to_string(recovery.current_incarnation.value())
                                           : "unset");
    return text;
}

[[nodiscard]] std::string render_plan_summary(const PlanSummary& summary) {
    std::string text = "id=" + summary.id.name();
    text += " revision=" + std::to_string(summary.revision.value());
    text += " phase=" + std::string(to_string(summary.phase));
    text += " activity=" + std::string(to_string(summary.activity));
    text += " priority=" + std::string(to_string(summary.priority));
    text += " risk=" + std::string(to_string(summary.risk));
    text += " targets=" + std::to_string(summary.target_count);
    text += " recovery-required=" + std::string(summary.recovery_required ? "true" : "false");
    text += " window=" + format_timestamp(summary.window.start) + ".." + format_timestamp(summary.window.end);
    text += " updated-at=" + format_timestamp(summary.updated_at);
    return text;
}

[[nodiscard]] std::string render_condition(const ConditionResult& condition) {
    std::string text = "code=" + std::string(to_string(condition.condition));
    text += " satisfied=" + std::string(condition.satisfied ? "true" : "false");
    text += " hard=" + std::string(condition.hard ? "true" : "false");
    text += " waivable=" + std::string(condition.waivable ? "true" : "false");
    text += " measured=" + std::string(condition.measured ? "true" : "false");
    text += " subject=" + condition.subject;
    text += " observed=" + std::to_string(condition.observed);
    text += " required=" + std::to_string(condition.required);
    text += " detail=" + quote_text(condition.detail);
    return text;
}

[[nodiscard]] std::string render_obligation_status(const ObligationStatus& status) {
    std::string text = "id=" + status.id.name();
    text += " kind=" + std::string(to_string(status.kind));
    text += " authority=" + std::string(to_string(status.authority));
    text += " stage=" + std::string(to_string(status.stage));
    text += " disposition=" + std::string(to_string(status.disposition));
    text += " mandatory=" + std::string(status.mandatory ? "true" : "false");
    text += " target=" + status.target.str();
    if (!status.waiver.empty()) {
        text += " waiver=" + status.waiver.name();
    }
    if (!status.supporting_receipts.empty()) {
        text += " receipts=" + comma_list(status.supporting_receipts);
    }
    text += " detail=" + quote_text(status.detail);
    return text;
}

[[nodiscard]] std::string render_outcome_text(const CommandOutcome& outcome) {
    std::string out;
    add_line(out, "command", outcome.command);

    const CommandResult* result = outcome.result.has_value() ? &*outcome.result : nullptr;

    if (result != nullptr && command_mutates(outcome.command)) {
        const MutationOutcome& mutation = result->mutation;
        std::string text = "intent=" + mutation.intent_digest.hex();
        text += " outcome=" + mutation.outcome_digest.hex();
        text += " sequence=" + (mutation.sequence.is_set() ? std::to_string(mutation.sequence.value()) : "unset");
        text += " applied-at=" + (is_set(mutation.applied_at) ? format_timestamp(mutation.applied_at) : "unset");
        text += " replayed=" + std::string(mutation.replayed ? "true" : "false");
        add_line(out, "mutation", text);
    }

    const RecoverySummary* recovery = nullptr;
    if (result != nullptr) {
        recovery = &result->store_recovery;
    } else if (outcome.has_recovery) {
        recovery = &*outcome.recovery;
    }
    if (recovery != nullptr) {
        add_line(out, "store-recovery", render_recovery(*recovery));
    }

    if (result != nullptr) {
        if (result->has_plan) {
            const PlanRecord& record = result->plan;
            std::string plan = "id=" + record.plan.id.name();
            plan += " revision=" + std::to_string(record.plan.revision.value());
            plan += " phase=" + std::string(to_string(record.phase));
            plan += " activity=" + std::string(to_string(record.plan.activity));
            plan += " priority=" + std::string(to_string(record.plan.priority));
            plan += " risk=" + std::string(to_string(record.plan.risk));
            plan += " recovery-required=" + std::string(record.recovery_required ? "true" : "false");
            add_line(out, "plan", plan);
            add_line(out, "plan-scope", comma_list(record.plan.scope.targets()));
            add_line(out, "plan-window",
                     "start=" + format_timestamp(record.plan.window.start) +
                         " end=" + format_timestamp(record.plan.window.end) +
                         " flexible=" + std::string(record.plan.window.flexible ? "true" : "false"));
            add_line(out, "plan-state",
                     "created-at=" + format_timestamp(record.created_at) +
                         " updated-at=" + format_timestamp(record.updated_at) +
                         " sequence=" + (record.sequence.is_set() ? std::to_string(record.sequence.value()) : "unset") +
                         " last-observation-sequence=" +
                         (record.last_observation_sequence.is_set()
                              ? std::to_string(record.last_observation_sequence.value())
                              : "unset"));
            add_line(out, "plan-reason", quote_text(record.plan.reason));
            add_line(out, "plan-requested-by", record.plan.requested_by);
            add_line(out, "plan-digest", record.plan.digest.hex());
        }
        if (!result->plans.empty()) {
            add_line(out, "plans", std::to_string(result->plans.size()));
            for (const auto& summary : result->plans) {
                add_line(out, "plan-summary", render_plan_summary(summary));
            }
        }
        if (result->facility.has_value()) {
            const FacilitySnapshot& facility = *result->facility;
            std::string text = "revision=" + std::to_string(facility.revision.value());
            text += " facility-epoch=" + std::to_string(facility.facility_epoch.value());
            text += " policy=" + facility.policy.id.name() + "@" + std::to_string(facility.policy.revision.value());
            text += " observed-at=" + format_timestamp(facility.observed_at);
            text += " assets=" + std::to_string(facility.assets.size());
            text += " redundancy-groups=" + std::to_string(facility.redundancy_groups.size());
            text += " capacity-pools=" + std::to_string(facility.capacity_pools.size());
            text += " power-domains=" + std::to_string(facility.power_domains.size());
            text += " cooling-zones=" + std::to_string(facility.cooling_zones.size());
            text += " fabric-segments=" + std::to_string(facility.fabric_segments.size());
            text += " blackouts=" + std::to_string(facility.blackouts.size());
            text += " incidents=" + std::to_string(facility.incidents.size());
            text += " protected-obligations=" + std::to_string(facility.protected_obligations.size());
            text += " digest=" + facility.digest.hex();
            add_line(out, "facility", text);
        }
        if (result->evaluation.has_value()) {
            const PreconditionReport& report = *result->evaluation;
            std::string text = "revision=" + std::to_string(report.plan_revision.value());
            text += " satisfied=" + std::string(report.satisfied ? "true" : "false");
            text += " satisfied-without-exceptions=" +
                    std::string(report.satisfied_without_exceptions ? "true" : "false");
            text += " evaluated-at=" + format_timestamp(report.evaluated_at);
            text += " conditions=" + std::to_string(report.conditions.size());
            text += " applied-exceptions=[" + comma_list(report.applied_exceptions) + "]";
            add_line(out, "evaluation", text);
            for (const auto& condition : report.conditions) {
                add_line(out, "condition", render_condition(condition));
            }
        }
        if (result->approval.has_value()) {
            const ApprovalRecord& approval = *result->approval;
            std::string text = "id=" + approval.id.name();
            text += " plan=" + approval.plan.name();
            text += " revision=" + std::to_string(approval.plan_revision.value());
            text += " approved-by=" + approval.approved_by;
            text += " witness=" + (approval.witness.empty() ? std::string("-") : approval.witness);
            text += " approved-at=" + format_timestamp(approval.approved_at);
            text += " expires-at=" + format_timestamp(approval.expires_at);
            text += " evidence=" + quote_text(approval.approval_evidence);
            add_line(out, "approval", text);
        }
        if (result->obligations.has_value()) {
            const ObligationStatusReport& report = *result->obligations;
            std::string text = "revision=" + std::to_string(report.plan_revision.value());
            text += " all-satisfied=" + std::string(report.all_satisfied ? "true" : "false");
            text += " ready-for-pre-drain=" + std::string(report.ready_for_pre_drain ? "true" : "false");
            text += " ready-for-isolation=" + std::string(report.ready_for_isolation ? "true" : "false");
            text += " ready-for-completion=" + std::string(report.ready_for_completion ? "true" : "false");
            text += " statuses=" + std::to_string(report.statuses.size());
            add_line(out, "obligations", text);
            for (const auto& status : report.statuses) {
                add_line(out, "obligation", render_obligation_status(status));
            }
        }
        if (result->receipt.has_value()) {
            const Receipt& receipt = *result->receipt;
            std::string text = "id=" + receipt.id.name();
            text += " obligation=" + receipt.obligation.name();
            text += " kind=" + std::string(to_string(receipt.kind));
            text += " authority=" + std::string(to_string(receipt.issuer_authority));
            text += " issuer=" + receipt.issuer;
            text += " sequence=" + (receipt.observation_sequence.is_set()
                                        ? std::to_string(receipt.observation_sequence.value())
                                        : "unset");
            text += " observed-at=" + format_timestamp(receipt.observed_at);
            text += " ingested-at=" + format_timestamp(receipt.ingested_at);
            text += " evidence=" + quote_text(receipt.evidence);
            add_line(out, "receipt", text);
        }
        if (result->exception.has_value()) {
            const ExceptionGrant& grant = *result->exception;
            std::string text = "id=" + grant.id.name();
            text += " plan=" + grant.plan.name();
            std::string waived;
            for (const auto code : grant.waived) {
                if (!waived.empty()) {
                    waived += ',';
                }
                waived += to_string(code);
            }
            text += " waived=[" + waived + "]";
            text += " targets=" + comma_list(grant.targets);
            text += " granted-by=" + grant.granted_by;
            text += " granted-at=" + format_timestamp(grant.granted_at);
            text += " expires-at=" + format_timestamp(grant.expires_at);
            text += " justification=" + quote_text(grant.justification);
            add_line(out, "exception", text);
        }
        if (result->restoration.has_value()) {
            const RestorationReport& report = *result->restoration;
            std::string text = "restored=" + std::string(report.restored ? "true" : "false");
            text += " revision=" + std::to_string(report.plan_revision.value());
            text += " verified-at=" + format_timestamp(report.verified_at);
            text += " outstanding=[" + comma_list(report.outstanding) + "]";
            text += " violated-protections=[" + comma_list(report.violated_protections) + "]";
            text += " redundancy-below-requirement=[" + comma_list(report.redundancy_below_requirement) + "]";
            add_line(out, "restoration", text);
        }
        if (result->completion.has_value()) {
            const CompletionReport& report = *result->completion;
            std::string text = "completed-at=" + format_timestamp(report.completed_at);
            text += " revision=" + std::to_string(report.plan_revision.value());
            text += " sequence=" + (report.sequence.is_set() ? std::to_string(report.sequence.value()) : "unset");
            text += " satisfied=" + std::to_string(report.satisfied.size());
            text += " protected-obligations-satisfied=" +
                    std::string(report.protected_obligations_satisfied ? "true" : "false");
            text += " restoration-verified=" + std::string(report.restoration_verified ? "true" : "false");
            add_line(out, "completion", text);
        }
        if (result->recovery.has_value()) {
            const RecoveryReport& report = *result->recovery;
            std::string text = "plan=" + report.plan.name();
            text += " phase-before=" + std::string(to_string(report.phase_before));
            text += " phase-after=" + std::string(to_string(report.phase_after));
            text += " recovered-at=" + format_timestamp(report.recovered_at);
            text += " prior-incarnation=" +
                    (report.prior_incarnation.is_set() ? std::to_string(report.prior_incarnation.value()) : "unset");
            text += " current-incarnation=" + (report.current_incarnation.is_set()
                                                   ? std::to_string(report.current_incarnation.value())
                                                   : "unset");
            text += " outstanding=[" + comma_list(report.outstanding) + "]";
            text += " recovery-required-after=" +
                    std::string(report.recovery_required_after ? "true" : "false");
            add_line(out, "recovery-report", text);
            for (const auto& note : report.notes) {
                add_line(out, "recovery-note", note);
            }
        }
        if (result->explanation.has_value()) {
            const Explanation& explanation = *result->explanation;
            std::string text = "plan=" + explanation.plan.name();
            text += " revision=" + std::to_string(explanation.revision.value());
            text += " phase=" + std::string(to_string(explanation.phase));
            text += " can-evaluate=" + std::string(explanation.can_evaluate ? "true" : "false");
            text += " can-approve=" + std::string(explanation.can_approve ? "true" : "false");
            text += " can-begin=" + std::string(explanation.can_begin ? "true" : "false");
            text += " can-complete=" + std::string(explanation.can_complete ? "true" : "false");
            text += " can-cancel=" + std::string(explanation.can_cancel ? "true" : "false");
            add_line(out, "explanation", text);
            for (const auto& condition : explanation.blockers) {
                add_line(out, "blocker", render_condition(condition));
            }
            for (const auto& reason : explanation.reasons) {
                add_line(out, "reason", reason);
            }
            for (const auto& step : explanation.next_steps) {
                add_line(out, "next-step", step);
            }
        }
        for (const auto& note : result->notes) {
            add_line(out, "note", note);
        }
    }

    if (outcome.has_audit) {
        add_line(out, "audit-entries", std::to_string(outcome.audit.size()));
        for (const auto& entry : outcome.audit) {
            std::string text = "sequence=" +
                               (entry.sequence.is_set() ? std::to_string(entry.sequence.value()) : "unset");
            text += " kind=" + std::string(to_string(entry.kind));
            text += " offset=" + std::to_string(entry.offset);
            text += " payload-bytes=" + std::to_string(entry.payload_bytes);
            text += " plan=" + (entry.plan.empty() ? std::string("-") : entry.plan.name());
            text += " payload-digest=" + entry.payload_digest.hex();
            add_line(out, "audit-entry", text);
        }
    }
    for (const auto& note : outcome.notes) {
        add_line(out, "note", note);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Usage and version
// ---------------------------------------------------------------------------

[[nodiscard]] std::string usage_text() {
    std::string text;
    text += "Usage: maintenance-coordinator <command> [options]\n";
    text += "\n";
    text += "Global options (valid for every command):\n";
    text += "  --store <dir>        required; the store directory (created when missing)\n";
    text += "  --read-only          open the store without the single-writer lock\n";
    text += "  --json               machine readable JSON output on stdout\n";
    text += "  --now <rfc3339>      evaluate the command at this instant instead of the host clock\n";
    text += "  --actor <id>         accountable operator identity (default \"" + std::string(kDefaultActor) +
            "\")\n";
    text += "  --attempt <id>       idempotency identity (default \"auto-\" plus the first 16 hex\n";
    text += "                       characters of the intent digest of this request)\n";
    text += "  --help, --version\n";
    text += "\n";
    text += "Commands:\n";
    text += "  init\n";
    text += "  install-facility --file <path|->          the facility model document (see below)\n";
    text += "  propose --reason <text> --requester <id> --activity <name> --priority <name>\n";
    text += "          --risk <name> --target <kind:name> (repeatable) --start <rfc3339>\n";
    text += "          --end <rfc3339> [--plan-id <id>] [--flexible]\n";
    text += "  evaluate --plan <id> --revision <n>\n";
    text += "  approve --plan <id> --revision <n> --approver <id> --evidence <text> [--witness <id>]\n";
    text += "  derive-obligations --plan <id> --revision <n>\n";
    text += "  ingest --plan <id> --revision <n> --receipt <id> --obligation <id> --kind <name>\n";
    text += "         --authority <name> --issuer <id> --sequence <n> --evidence <text>\n";
    text += "         [--observed-at <rfc3339>]   (default: this command's instant)\n";
    text += "  grant-exception --plan <id> --revision <n> --waive <ErrorCode> (repeatable)\n";
    text += "                  --target <kind:name> (repeatable) --grantor <id>\n";
    text += "                  --justification <text> [--validity-hours <n>]\n";
    text += "  begin --plan <id> --revision <n>\n";
    text += "  progress --plan <id> --revision <n> --kind <name> [--note <text>]\n";
    text += "  verify-restoration --plan <id> --revision <n>\n";
    text += "  complete --plan <id> --revision <n> [--note <text>]\n";
    text += "  cancel --plan <id> --revision <n> [--reason <text>]\n";
    text += "  recover [--plan <id>]\n";
    text += "  explain --plan <id>\n";
    text += "  show --plan <id>\n";
    text += "  list [--active-only]\n";
    text += "  audit\n";
    text += "  compact\n";
    text += "\n";
    text += "Enum spellings are the spellings this library prints:\n";
    text += "  activity       " +
            join_names({to_string(MaintenanceActivity::Inspection), to_string(MaintenanceActivity::HardwareRepair),
                        to_string(MaintenanceActivity::HardwareUpgrade),
                        to_string(MaintenanceActivity::FirmwareUpgrade), to_string(MaintenanceActivity::PowerWork),
                        to_string(MaintenanceActivity::CoolingWork), to_string(MaintenanceActivity::NetworkWork),
                        to_string(MaintenanceActivity::RackInstall), to_string(MaintenanceActivity::Decommission),
                        to_string(MaintenanceActivity::DataWipe)}) +
            "\n";
    text += "  priority       " + join_names({to_string(PlanPriority::Routine), to_string(PlanPriority::Elevated),
                                               to_string(PlanPriority::Emergency)}) +
            "\n";
    text += "  risk           " +
            join_names({to_string(ServiceRiskClass::Low), to_string(ServiceRiskClass::Moderate),
                        to_string(ServiceRiskClass::High), to_string(ServiceRiskClass::Critical)}) +
            "\n";
    text += "  progress kind  " +
            join_names({to_string(ProgressKind::Started), to_string(ProgressKind::StepCompleted),
                        to_string(ProgressKind::WorkCompleted), to_string(ProgressKind::WorkStopped),
                        to_string(ProgressKind::IssueObserved)}) +
            "\n";
    text += "  receipt kind   " +
            join_names({to_string(ReceiptKind::DrainRequested), to_string(ReceiptKind::DrainAcknowledged),
                        to_string(ReceiptKind::DrainObserved), to_string(ReceiptKind::QuiesceObserved),
                        to_string(ReceiptKind::TrafficDrainObserved), to_string(ReceiptKind::PathIsolationObserved),
                        to_string(ReceiptKind::PowerIsolationObserved),
                        to_string(ReceiptKind::CoolingAdjustmentObserved),
                        to_string(ReceiptKind::LifecycleTransitionObserved),
                        to_string(ReceiptKind::PersonnelOnSiteObserved),
                        to_string(ReceiptKind::ApprovalWitnessObserved),
                        to_string(ReceiptKind::MaintenanceVerifiedObserved),
                        to_string(ReceiptKind::WorkStopObserved), to_string(ReceiptKind::DrainReleaseObserved),
                        to_string(ReceiptKind::PowerRestoreObserved), to_string(ReceiptKind::CoolingRestoreObserved),
                        to_string(ReceiptKind::TrafficRestoreObserved),
                        to_string(ReceiptKind::RedundancyRestoreObserved), to_string(ReceiptKind::FailureObserved)}) +
            "\n";
    text += "  authority      " +
            join_names({to_string(ObligationAuthority::Asi), to_string(ObligationAuthority::Dfi),
                        to_string(ObligationAuthority::DccpPlant), to_string(ObligationAuthority::DccpInventory),
                        to_string(ObligationAuthority::Operator)}) +
            "\n";
    text += "  target kind    " +
            join_names({to_string(TargetKind::Site), to_string(TargetKind::Rack), to_string(TargetKind::Asset),
                        to_string(TargetKind::PowerDomain), to_string(TargetKind::CoolingZone),
                        to_string(TargetKind::FabricSegment)}) +
            "\n";
    text += "  --waive        the ErrorCode spellings of mc::to_string(ErrorCode); hard safety\n";
    text += "                 interlocks are never waivable and are refused by the engine\n";
    text += "\n";
    text += "Facility model document (strict JSON; an unknown member is MalformedInput, a\n";
    text += "missing required member is MissingArgument, omitted arrays mean empty):\n";
    text += "  { \"revision\": n, \"facility_epoch\": n, \"policy_generation\": n,\n";
    text += "    \"dependency_generation\": n, \"capacity_generation\": n, \"topology_generation\": n,\n";
    text += "    \"maintenance_generation\": n, \"control_epoch\": n, \"observed_at\": \"<rfc3339>\",\n";
    text += "    \"policy\": { \"id\": s, \"revision\": n, \"min_redundancy_margin_units\": n,\n";
    text += "                \"min_spare_capacity_units\": n, \"min_power_headroom_milliwatts\": n,\n";
    text += "                \"min_cooling_headroom_units\": n, \"approval_validity_hours\": n,\n";
    text += "                \"max_window_hours\": n, \"max_concurrent_windows_per_rack\": n,\n";
    text += "                \"require_personnel_evidence\": b, \"require_dual_approval\": b,\n";
    text += "                \"require_work_stop_evidence\": b, \"require_restoration_evidence\": b },\n";
    text += "    \"assets\": [ { \"id\": s, \"rack\": s, \"site\": s, \"lifecycle\": s,\n";
    text += "                  \"service_class\": s, \"lifecycle_generation\": n, \"hardware_generation\": n,\n";
    text += "                  \"firmware_generation\": n, \"capacity_units\": n, \"isolatable\": b,\n";
    text += "                  \"drainable\": b, \"healthy\": b } ],\n";
    text += "    \"redundancy_groups\": [ { \"id\": s, \"site\": s, \"mode\": s, \"required_units\": n,\n";
    text += "                             \"observed_available_units\": n, \"members\": [s, ...] } ],\n";
    text += "    \"capacity_pools\": [ { \"id\": s, \"site\": s, \"units_total\": n,\n";
    text += "                          \"units_available\": n, \"units_protected\": n } ],\n";
    text += "    \"power_domains\": [ { \"id\": s, \"site\": s, \"headroom\": { \"measured\": b,\n";
    text += "                         \"available_units\": n, \"required_units\": n },\n";
    text += "                         \"interlocked\": b, \"interlock_reason\": s } ],\n";
    text += "    \"cooling_zones\": [ ... same shape as power_domains ... ],\n";
    text += "    \"fabric_segments\": [ { \"id\": s, \"site\": s, \"available_paths\": n,\n";
    text += "                           \"required_paths\": n, \"drainable\": b } ],\n";
    text += "    \"blackouts\": [ { \"id\": s, \"targets\": [\"kind:name\"], \"start\": s, \"end\": s,\n";
    text += "                     \"hard\": b, \"reason\": s } ],\n";
    text += "    \"incidents\": [ { \"id\": s, \"targets\": [\"kind:name\"], \"severity\": s,\n";
    text += "                     \"active\": b, \"hard_block\": b, \"summary\": s } ],\n";
    text += "    \"protected_obligations\": [ { \"id\": s, \"targets\": [\"kind:name\"],\n";
    text += "                                 \"required_mode\": s, \"required_units\": n,\n";
    text += "                                 \"tolerance_units\": n, \"hard_interlock\": b,\n";
    text += "                                 \"description\": s } ] }\n";
    text += "\n";
    text += "With --json exactly one JSON object is written to stdout; on failure it is\n";
    text += "{\"ok\":false,\"command\":...,\"error\":{\"code\":...,\"message\":...,\"subject\":...,\n";
    text += "\"detail\":...,\"suppressed\":[...]}} and stderr stays empty.\n";
    text += "\n";
    text += "Exit codes: 0 ok, 1 usage, 2 identity, 3 authority/fencing, 4 lifecycle,\n";
    text += "            5 policy/preconditions, 6 I/O and bounds, 7 replay, 8 internal.\n";
    return text;
}

[[nodiscard]] std::string version_text() {
    std::string text;
    text += std::string(kProductName) + " " + std::string(kLibraryVersion) + "\n";
    text += "journal-format-version: " + std::to_string(kJournalFormatVersion) + "\n";
    text += "snapshot-format-version: " + std::to_string(kSnapshotFormatVersion) + "\n";
    text += "journal-record-magic: " + magic_text(kJournalRecordMagic) + "\n";
    text += "snapshot-magic: " + magic_text(kSnapshotMagic) + "\n";
    text += "max-record-payload-bytes: " + std::to_string(kMaxRecordPayloadBytes) + "\n";
    text += "max-text-document-bytes: " + std::to_string(kMaxTextDocumentBytes) + "\n";
    return text;
}

[[nodiscard]] json::Value version_object() {
    json::Value value = json::Value::object();
    value.set("ok", jbool(true));
    value.set("command", jtext("version"));
    value.set("product", jtext(kProductName));
    value.set("library_version", jtext(kLibraryVersion));
    value.set("journal_format_version", jcount(kJournalFormatVersion));
    value.set("snapshot_format_version", jcount(kSnapshotFormatVersion));
    value.set("journal_record_magic", jtext(magic_text(kJournalRecordMagic)));
    value.set("snapshot_magic", jtext(magic_text(kSnapshotMagic)));
    value.set("max_record_payload_bytes", jcount(kMaxRecordPayloadBytes));
    return value;
}

[[nodiscard]] bool contains_token(const std::vector<std::string>& tokens, std::string_view token) {
    for (const auto& item : tokens) {
        if (item == token) {
            return true;
        }
    }
    return false;
}

void write_line(const std::string& text) {
    std::cout << text << '\n';
}

void report_failure(bool json, const std::string& command, const Status& status) {
    if (json) {
        write_line(encode_failure(command, status).dump());
        return;
    }
    write_line(status.render());
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int run(const std::vector<std::string>& tokens) {
    const bool json_requested = contains_token(tokens, "--json");

    if (contains_token(tokens, "--help")) {
        if (json_requested) {
            json::Value value = json::Value::object();
            value.set("ok", jbool(true));
            value.set("command", jtext("help"));
            value.set("usage", jtext(usage_text()));
            write_line(value.dump());
        } else {
            std::cout << usage_text();
        }
        return 0;
    }
    if (contains_token(tokens, "--version")) {
        if (json_requested) {
            write_line(version_object().dump());
        } else {
            std::cout << version_text();
        }
        return 0;
    }

    if (tokens.empty()) {
        const Status status = fail(ErrorCode::InvalidUsage, "a command is required");
        report_failure(json_requested, std::string(), status);
        return exit_code_of(status.code());
    }

    const std::string command = tokens.front();
    if (command[0] == '-') {
        // Options are only ever accepted after the command, so a leading option
        // means the command itself is missing rather than misspelled.
        const Status status = fail(ErrorCode::InvalidUsage, "a command is required", "argument", command);
        report_failure(json_requested, std::string(), status);
        return exit_code_of(status.code());
    }
    const std::vector<std::string> tail(tokens.begin() + 1, tokens.end());

    ArgList args;
    if (Status status = parse_arguments(tail, args); !status.ok()) {
        report_failure(json_requested, command, status);
        return exit_code_of(status.code());
    }

    const CommandSpec* spec = find_command(command);
    if (spec == nullptr) {
        const Status status = fail(ErrorCode::UnknownCommand, "unknown command", "command", command);
        report_failure(json_requested, command, status);
        return exit_code_of(status.code());
    }

    Invocation invocation;
    if (Status status = build_invocation(command, args, *spec, invocation); !status.ok()) {
        report_failure(json_requested, command, status);
        return exit_code_of(status.code());
    }

    CommandOutcome outcome = make_outcome(invocation);
    if (Status status = dispatch(invocation, outcome); !status.ok()) {
        report_failure(invocation.json, command, status);
        return exit_code_of(status.code());
    }
    if (invocation.json) {
        write_line(encode_outcome(outcome).dump());
    } else {
        std::cout << render_outcome_text(outcome);
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> tokens;
    tokens.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        tokens.push_back(to_utf8(argv[index]));
    }
    return run(tokens);
}

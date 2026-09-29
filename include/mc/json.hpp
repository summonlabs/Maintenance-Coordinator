// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// A small, strict JSON value model used by the CLI boundary.
//
// Strictness is deliberate: integers only (a floating point number is rejected
// rather than silently rounded, because every number in this domain is a
// count, a generation or a timestamp), no duplicate object keys, no trailing
// content, no invalid UTF-8, no lone surrogates, bounded nesting depth and
// bounded document size.  Values are rendered in insertion order, which makes
// output deterministic for a given input.

#ifndef MC_JSON_HPP
#define MC_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mc/status.hpp"

namespace mc::json {

inline constexpr std::size_t kMaxJsonDepth = 64;
inline constexpr std::size_t kMaxJsonDocumentBytes = 32U * 1024U * 1024U;

class Value {
public:
    using Array = std::vector<Value>;
    using Member = std::pair<std::string, Value>;
    using Object = std::vector<Member>;

    enum class Kind : std::uint8_t { Null, Bool, Int, String, Array, Object };

    Value() noexcept = default;
    Value(std::nullptr_t) noexcept {}
    Value(bool value) noexcept : kind_(Kind::Bool), bool_(value) {}
    Value(std::int64_t value) noexcept : kind_(Kind::Int), int_(value) {}
    Value(int value) noexcept : kind_(Kind::Int), int_(value) {}
    Value(std::uint32_t value) noexcept : kind_(Kind::Int), int_(value) {}
    Value(std::string value) : kind_(Kind::String), string_(std::move(value)) {}
    Value(const char* value) : kind_(Kind::String), string_(value == nullptr ? "" : value) {}

    [[nodiscard]] static Value array() { Value v; v.kind_ = Kind::Array; return v; }
    [[nodiscard]] static Value array(Array items);
    [[nodiscard]] static Value object() { Value v; v.kind_ = Kind::Object; return v; }
    [[nodiscard]] static Value object(Object members);

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return kind_ == Kind::Bool; }
    [[nodiscard]] bool is_int() const noexcept { return kind_ == Kind::Int; }
    [[nodiscard]] bool is_string() const noexcept { return kind_ == Kind::String; }
    [[nodiscard]] bool is_array() const noexcept { return kind_ == Kind::Array; }
    [[nodiscard]] bool is_object() const noexcept { return kind_ == Kind::Object; }

    [[nodiscard]] const bool* as_bool() const noexcept { return is_bool() ? &bool_ : nullptr; }
    [[nodiscard]] const std::int64_t* as_int() const noexcept { return is_int() ? &int_ : nullptr; }
    [[nodiscard]] const std::string* as_string() const noexcept { return is_string() ? &string_ : nullptr; }
    [[nodiscard]] const Array* as_array() const noexcept { return is_array() ? &array_ : nullptr; }
    [[nodiscard]] const Object* as_object() const noexcept { return is_object() ? &object_ : nullptr; }

    // Object mutation.  Insertion order is preserved; re-setting an existing key
    // replaces its value in place.
    Value& set(std::string key, Value value);
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    [[nodiscard]] bool has(std::string_view key) const noexcept { return find(key) != nullptr; }

    // Array mutation.
    Value& push(Value value);
    [[nodiscard]] std::size_t size() const noexcept;

    // Rendering.  indent == 0 produces a single compact line.
    [[nodiscard]] std::string dump(std::size_t indent = 0) const;

private:
    void dump_into(std::string& out, std::size_t indent, std::size_t depth) const;

    Kind kind_{Kind::Null};
    bool bool_{false};
    std::int64_t int_{0};
    std::string string_;
    Array array_;
    Object object_;
};

// Strict parse.  Malformed syntax - including a missing member separator, an
// unknown escape, a lone surrogate, a fractional number or a duplicate member
// name - is ErrorCode::MalformedInput with a precise detail; content that
// follows a complete document is ErrorCode::TrailingBytes; a document larger
// than kMaxJsonDocumentBytes or nested deeper than kMaxJsonDepth is
// ErrorCode::LimitExceeded.
[[nodiscard]] Result<Value> parse(std::string_view text, std::string_view what = "json");

[[nodiscard]] std::string escape(std::string_view text);
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

// Field readers used by the CLI: they return a Status that names the field so
// a missing or mistyped key is reported precisely rather than defaulted.
[[nodiscard]] Status require_object(const Value& value, std::string_view what);
[[nodiscard]] Status require_string(const Value& object, std::string_view key, std::string& out, bool required = true);
[[nodiscard]] Status require_int(const Value& object, std::string_view key, std::int64_t& out, bool required = true);
[[nodiscard]] Status require_bool(const Value& object, std::string_view key, bool& out, bool required = true);

}  // namespace mc::json

#endif  // MC_JSON_HPP

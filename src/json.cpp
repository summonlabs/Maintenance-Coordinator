// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.

#include "mc/json.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mc/version.hpp"

namespace mc::json {
namespace {

[[nodiscard]] bool is_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] std::uint32_t hex_to_u32(std::string_view text) noexcept {
    std::uint32_t value = 0;
    for (const char c : text) {
        value <<= 4U;
        if (c >= '0' && c <= '9') {
            value |= static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<std::uint32_t>(c - 'a' + 10);
        } else {
            value |= static_cast<std::uint32_t>(c - 'A' + 10);
        }
    }
    return value;
}

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point <= 0x7FU) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFU) {
        out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point <= 0xFFFFU) {
        out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) noexcept : text_(text) {}

    [[nodiscard]] Result<Value> run() {
        skip_whitespace();
        auto value = parse_value(0U);
        if (!value.ok()) {
            return value.status();
        }
        skip_whitespace();
        if (offset_ != text_.size()) {
            return fail(ErrorCode::TrailingBytes, "unexpected content after the end of the JSON document",
                        "json", describe());
        }
        return value;
    }

private:
    [[nodiscard]] std::string describe() const {
        const std::size_t start = offset_ > 16U ? offset_ - 16U : 0U;
        return "at offset " + std::to_string(offset_) + " near '" +
               std::string(text_.substr(start, 24U)) + "'";
    }

    void skip_whitespace() noexcept {
        while (offset_ < text_.size()) {
            const char c = text_[offset_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++offset_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] bool consume(char expected) noexcept {
        if (offset_ < text_.size() && text_[offset_] == expected) {
            ++offset_;
            return true;
        }
        return false;
    }

    [[nodiscard]] Result<Value> parse_value(std::size_t depth) {
        if (depth > kMaxJsonDepth) {
            return fail(ErrorCode::LimitExceeded, "JSON nesting depth exceeds the accepted limit", "json",
                        std::to_string(depth));
        }
        if (offset_ >= text_.size()) {
            return fail(ErrorCode::MalformedInput, "JSON document ends where a value was expected", "json",
                        describe());
        }
        switch (text_[offset_]) {
            case '{':
                return parse_object(depth);
            case '[':
                return parse_array(depth);
            case '"': {
                std::string text;
                if (auto status = parse_string(text); !status.ok()) {
                    return status;
                }
                return Value(std::move(text));
            }
            case 't':
                if (text_.compare(offset_, 4U, "true") == 0) {
                    offset_ += 4U;
                    return Value(true);
                }
                break;
            case 'f':
                if (text_.compare(offset_, 5U, "false") == 0) {
                    offset_ += 5U;
                    return Value(false);
                }
                break;
            case 'n':
                if (text_.compare(offset_, 4U, "null") == 0) {
                    offset_ += 4U;
                    return Value(nullptr);
                }
                break;
            default:
                break;
        }
        if (text_[offset_] == '-' || (text_[offset_] >= '0' && text_[offset_] <= '9')) {
            return parse_number();
        }
        return fail(ErrorCode::MalformedInput, "JSON value is not well formed", "json", describe());
    }

    [[nodiscard]] Result<Value> parse_number() {
        const std::size_t start = offset_;
        if (consume('-')) {
            // sign consumed
        }
        if (offset_ >= text_.size() || text_[offset_] < '0' || text_[offset_] > '9') {
            return fail(ErrorCode::MalformedInput, "JSON number has no digits", "json", describe());
        }
        if (text_[offset_] == '0' && offset_ + 1U < text_.size() && text_[offset_ + 1U] >= '0' &&
            text_[offset_ + 1U] <= '9') {
            return fail(ErrorCode::MalformedInput, "JSON number has a leading zero", "json", describe());
        }
        while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9') {
            ++offset_;
        }
        if (offset_ < text_.size() && (text_[offset_] == '.' || text_[offset_] == 'e' || text_[offset_] == 'E')) {
            return fail(ErrorCode::MalformedInput,
                        "fractional JSON numbers are rejected: this domain uses whole counts only", "json",
                        describe());
        }
        const std::string_view digits_text = text_.substr(start, offset_ - start);
        // The magnitude is accumulated unsigned so that the most negative
        // 64-bit integer, whose magnitude does not fit in a signed value, is
        // still representable rather than reported as out of range.
        std::uint64_t magnitude = 0;
        bool negative = false;
        std::size_t index = 0;
        if (digits_text[0] == '-') {
            negative = true;
            index = 1;
        }
        const std::uint64_t limit = negative ? 9223372036854775808ULL : 9223372036854775807ULL;
        for (; index < digits_text.size(); ++index) {
            const auto digit = static_cast<std::uint64_t>(digits_text[index] - '0');
            if (magnitude > (limit - digit) / 10U) {
                return fail(ErrorCode::LimitExceeded, "JSON integer does not fit in a signed 64-bit value",
                            "json", std::string(digits_text));
            }
            magnitude = (magnitude * 10U) + digit;
        }
        if (!negative) {
            return Value(static_cast<std::int64_t>(magnitude));
        }
        if (magnitude == 9223372036854775808ULL) {
            return Value((std::numeric_limits<std::int64_t>::min)());
        }
        return Value(-static_cast<std::int64_t>(magnitude));
    }

    [[nodiscard]] Status parse_string(std::string& out) {
        if (!consume('"')) {
            return fail(ErrorCode::MalformedInput, "expected a JSON string", "json", describe());
        }
        out.clear();
        while (true) {
            if (offset_ >= text_.size()) {
                return fail(ErrorCode::MalformedInput, "JSON string is not terminated", "json", describe());
            }
            const unsigned char byte = static_cast<unsigned char>(text_[offset_]);
            if (byte == static_cast<unsigned char>('"')) {
                ++offset_;
                break;
            }
            if (byte < 0x20U) {
                return fail(ErrorCode::MalformedInput, "JSON string contains an unescaped control character",
                            "json", describe());
            }
            if (byte == static_cast<unsigned char>('\\')) {
                ++offset_;
                if (offset_ >= text_.size()) {
                    return fail(ErrorCode::MalformedInput, "JSON escape is not terminated", "json", describe());
                }
                const char escape = text_[offset_++];
                switch (escape) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (offset_ + 4U > text_.size()) {
                            return fail(ErrorCode::MalformedInput, "JSON \\u escape is truncated", "json",
                                        describe());
                        }
                        for (std::size_t index = 0; index < 4U; ++index) {
                            if (!is_hex(text_[offset_ + index])) {
                                return fail(ErrorCode::MalformedInput, "JSON \\u escape is not hexadecimal",
                                            "json", describe());
                            }
                        }
                        std::uint32_t code_point = hex_to_u32(text_.substr(offset_, 4U));
                        offset_ += 4U;
                        if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
                            if (offset_ + 6U > text_.size() || text_[offset_] != '\\' ||
                                text_[offset_ + 1U] != 'u') {
                                return fail(ErrorCode::MalformedInput,
                                            "JSON high surrogate is not followed by a low surrogate", "json",
                                            describe());
                            }
                            for (std::size_t index = 0; index < 4U; ++index) {
                                if (!is_hex(text_[offset_ + 2U + index])) {
                                    return fail(ErrorCode::MalformedInput,
                                                "JSON surrogate escape is not hexadecimal", "json", describe());
                                }
                            }
                            const std::uint32_t low = hex_to_u32(text_.substr(offset_ + 2U, 4U));
                            if (low < 0xDC00U || low > 0xDFFFU) {
                                return fail(ErrorCode::MalformedInput,
                                            "JSON high surrogate is not followed by a low surrogate", "json",
                                            describe());
                            }
                            offset_ += 6U;
                            code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (low - 0xDC00U);
                        } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
                            return fail(ErrorCode::MalformedInput, "JSON string contains a lone low surrogate",
                                        "json", describe());
                        }
                        append_utf8(out, code_point);
                        break;
                    }
                    default:
                        return fail(ErrorCode::MalformedInput, "JSON string contains an unknown escape",
                                    "json", describe());
                }
                continue;
            }
            out.push_back(static_cast<char>(byte));
            ++offset_;
        }
        if (!is_valid_utf8(out)) {
            return fail(ErrorCode::MalformedInput, "JSON string is not valid UTF-8", "json", describe());
        }
        return Status::success();
    }

    [[nodiscard]] Result<Value> parse_array(std::size_t depth) {
        // The caller dispatched on the byte, so a false result here is
        // impossible; the explicit void cast documents that.
        (void)consume('[');
        Value array_value = Value::array();
        skip_whitespace();
        if (consume(']')) {
            return array_value;
        }
        while (true) {
            skip_whitespace();
            auto element = parse_value(depth + 1U);
            if (!element.ok()) {
                return element.status();
            }
            array_value.push(std::move(element).value());
            skip_whitespace();
            if (consume(',')) {
                continue;
            }
            if (consume(']')) {
                break;
            }
            return fail(ErrorCode::MalformedInput, "JSON array element is not followed by ',' or ']'", "json",
                        describe());
        }
        return array_value;
    }

    [[nodiscard]] Result<Value> parse_object(std::size_t depth) {
        (void)consume('{');
        Value object_value = Value::object();
        skip_whitespace();
        if (consume('}')) {
            return object_value;
        }
        while (true) {
            skip_whitespace();
            std::string key;
            if (auto status = parse_string(key); !status.ok()) {
                return status;
            }
            skip_whitespace();
            if (!consume(':')) {
                return fail(ErrorCode::MalformedInput, "JSON member is missing ':'", "json", describe());
            }
            skip_whitespace();
            auto member = parse_value(depth + 1U);
            if (!member.ok()) {
                return member.status();
            }
            if (object_value.has(key)) {
                return fail(ErrorCode::MalformedInput, "JSON object repeats a member name", "json", key);
            }
            object_value.set(std::move(key), std::move(member).value());
            skip_whitespace();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                break;
            }
            return fail(ErrorCode::MalformedInput, "JSON object member is not followed by ',' or '}'", "json",
                        describe());
        }
        return object_value;
    }

    std::string_view text_;
    std::size_t offset_{0};
};

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto byte = static_cast<unsigned char>(text[index]);
        std::size_t extra = 0;
        std::uint32_t code_point = 0;
        if (byte < 0x80U) {
            ++index;
            continue;
        }
        if ((byte & 0xE0U) == 0xC0U) {
            extra = 1;
            code_point = byte & 0x1FU;
            if (code_point == 0) {
                return false;  // overlong
            }
        } else if ((byte & 0xF0U) == 0xE0U) {
            extra = 2;
            code_point = byte & 0x0FU;
        } else if ((byte & 0xF8U) == 0xF0U) {
            extra = 3;
            code_point = byte & 0x07U;
        } else {
            return false;
        }
        if (index + extra >= text.size()) {
            return false;
        }
        for (std::size_t continuation = 0; continuation < extra; ++continuation) {
            const auto next = static_cast<unsigned char>(text[index + 1U + continuation]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3FU);
        }
        if (extra == 2U && (code_point < 0x800U || (code_point >= 0xD800U && code_point <= 0xDFFFU))) {
            return false;
        }
        if (extra == 3U && (code_point < 0x10000U || code_point > 0x10FFFFU)) {
            return false;
        }
        index += extra + 1U;
    }
    return true;
}

Value Value::array(Array items) {
    Value value;
    value.kind_ = Kind::Array;
    value.array_ = std::move(items);
    return value;
}

Value Value::object(Object members) {
    Value value;
    value.kind_ = Kind::Object;
    value.object_ = std::move(members);
    return value;
}

Value& Value::set(std::string key, Value value) {
    if (kind_ != Kind::Object) {
        kind_ = Kind::Object;
        object_.clear();
    }
    for (auto& member : object_) {
        if (member.first == key) {
            member.second = std::move(value);
            return member.second;
        }
    }
    object_.emplace_back(std::move(key), std::move(value));
    return object_.back().second;
}

const Value* Value::find(std::string_view key) const noexcept {
    if (kind_ != Kind::Object) {
        return nullptr;
    }
    for (const auto& member : object_) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

Value& Value::push(Value value) {
    if (kind_ != Kind::Array) {
        kind_ = Kind::Array;
        array_.clear();
    }
    array_.push_back(std::move(value));
    return array_.back();
}

std::size_t Value::size() const noexcept {
    if (kind_ == Kind::Array) {
        return array_.size();
    }
    if (kind_ == Kind::Object) {
        return object_.size();
    }
    return 0U;
}

std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8U);
    for (const char raw : text) {
        switch (raw) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(raw) < 0x20U) {
                    static constexpr char kDigits[] = "0123456789abcdef";
                    const auto byte = static_cast<unsigned char>(raw);
                    out += "\\u00";
                    out.push_back(kDigits[(byte >> 4U) & 0x0FU]);
                    out.push_back(kDigits[byte & 0x0FU]);
                } else {
                    out.push_back(raw);
                }
                break;
        }
    }
    return out;
}

void Value::dump_into(std::string& out, std::size_t indent, std::size_t depth) const {
    const auto newline = [&](std::size_t level) {
        if (indent != 0U) {
            out.push_back('\n');
            out.append(level * indent, ' ');
        }
    };
    switch (kind_) {
        case Kind::Null:
            out += "null";
            break;
        case Kind::Bool:
            out += bool_ ? "true" : "false";
            break;
        case Kind::Int:
            out += std::to_string(int_);
            break;
        case Kind::String:
            out.push_back('"');
            out += escape(string_);
            out.push_back('"');
            break;
        case Kind::Array: {
            if (array_.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            bool first = true;
            for (const auto& element : array_) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                newline(depth + 1U);
                element.dump_into(out, indent, depth + 1U);
            }
            newline(depth);
            out.push_back(']');
            break;
        }
        case Kind::Object: {
            if (object_.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            bool first = true;
            for (const auto& member : object_) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                newline(depth + 1U);
                out.push_back('"');
                out += escape(member.first);
                out.push_back('"');
                out += ": ";
                member.second.dump_into(out, indent, depth + 1U);
            }
            newline(depth);
            out.push_back('}');
            break;
        }
    }
}

std::string Value::dump(std::size_t indent) const {
    std::string out;
    dump_into(out, indent, 0U);
    return out;
}

Result<Value> parse(std::string_view text, std::string_view what) {
    if (text.size() > kMaxJsonDocumentBytes) {
        return fail(ErrorCode::LimitExceeded, "JSON document exceeds the accepted size", std::string(what),
                    std::to_string(text.size()) + " bytes");
    }
    Parser parser(text);
    return parser.run();
}

Status require_object(const Value& value, std::string_view what) {
    if (!value.is_object()) {
        return fail(ErrorCode::MalformedInput, "expected a JSON object", std::string(what));
    }
    return Status::success();
}

Status require_string(const Value& object, std::string_view key, std::string& out, bool required) {
    const Value* member = object.find(key);
    if (member == nullptr) {
        if (!required) {
            return Status::success();
        }
        return fail(ErrorCode::MissingArgument, "required string member is missing", std::string(key));
    }
    const std::string* text = member->as_string();
    if (text == nullptr) {
        return fail(ErrorCode::MalformedInput, "member is not a JSON string", std::string(key));
    }
    out = *text;
    return Status::success();
}

Status require_int(const Value& object, std::string_view key, std::int64_t& out, bool required) {
    const Value* member = object.find(key);
    if (member == nullptr) {
        if (!required) {
            return Status::success();
        }
        return fail(ErrorCode::MissingArgument, "required integer member is missing", std::string(key));
    }
    const std::int64_t* value = member->as_int();
    if (value == nullptr) {
        return fail(ErrorCode::MalformedInput, "member is not a JSON integer", std::string(key));
    }
    out = *value;
    return Status::success();
}

Status require_bool(const Value& object, std::string_view key, bool& out, bool required) {
    const Value* member = object.find(key);
    if (member == nullptr) {
        if (!required) {
            return Status::success();
        }
        return fail(ErrorCode::MissingArgument, "required boolean member is missing", std::string(key));
    }
    const bool* value = member->as_bool();
    if (value == nullptr) {
        return fail(ErrorCode::MalformedInput, "member is not a JSON boolean", std::string(key));
    }
    out = *value;
    return Status::success();
}

}  // namespace mc::json

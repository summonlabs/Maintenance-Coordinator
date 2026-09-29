// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.

#include "mc/time.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace mc {
namespace {

// Days from 1970-01-01 for a proleptic Gregorian date.  Deterministic and
// independent of the C library's time zone handling.
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
    year -= month <= 2U ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto year_of_era = static_cast<unsigned>(year - (era * 400));
    const unsigned shifted_month = (month > 2U) ? (month - 3U) : (month + 9U);
    const unsigned day_of_year = ((153U * shifted_month) + 2U) / 5U + day - 1U;
    const unsigned day_of_era = (year_of_era * 365U) + (year_of_era / 4U) - (year_of_era / 100U) + day_of_year;
    return (era * 146097) + static_cast<std::int64_t>(day_of_era) - 719468;
}

void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) noexcept {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned>(days - (era * 146097));
    const unsigned year_of_era =
        (day_of_era - (day_of_era / 1460U) + (day_of_era / 36524U) - (day_of_era / 146096U)) / 365U;
    year = static_cast<std::int64_t>(year_of_era) + (era * 400);
    const unsigned day_of_year = day_of_era - ((365U * year_of_era) + (year_of_era / 4U) - (year_of_era / 100U));
    const unsigned month_prime = (5U * day_of_year + 2U) / 153U;
    day = day_of_year - ((153U * month_prime + 2U) / 5U) + 1U;
    month = (month_prime < 10U) ? (month_prime + 3U) : (month_prime - 9U);
    year += month <= 2U ? 1 : 0;
}

[[nodiscard]] bool digits(std::string_view text, std::size_t offset, std::size_t count, std::int64_t& out) noexcept {
    if (offset + count > text.size()) {
        return false;
    }
    std::int64_t value = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char c = text[offset + index];
        if (c < '0' || c > '9') {
            return false;
        }
        value = (value * 10) + (c - '0');
    }
    out = value;
    return true;
}

}  // namespace

Timestamp SystemClock::now_nanos() const {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return static_cast<Timestamp>(nanos);
}

std::string format_timestamp(Timestamp value) {
    if (!is_set(value)) {
        return "unset";
    }
    std::int64_t seconds = value / kNanosPerSecond;
    std::int64_t nanos = value % kNanosPerSecond;
    if (nanos < 0) {
        nanos += kNanosPerSecond;
        --seconds;
    }
    // Floor division, not truncation: with truncation a negative instant that
    // is not a whole number of days renders one day late.
    std::int64_t days = seconds / 86400;
    std::int64_t remainder = seconds % 86400;
    if (remainder < 0) {
        remainder += 86400;
        --days;
    }
    std::int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, year, month, day);
    const auto hour = static_cast<unsigned>(remainder / 3600);
    const auto minute = static_cast<unsigned>((remainder % 3600) / 60);
    const auto second = static_cast<unsigned>(remainder % 60);

    char buffer[64];
    if (nanos == 0) {
        std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02u:%02u:%02uZ",
                      static_cast<long long>(year), month, day, hour, minute, second);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02u:%02u:%02u.%09lldZ",
                      static_cast<long long>(year), month, day, hour, minute, second,
                      static_cast<long long>(nanos));
    }
    return std::string(buffer);
}

bool parse_timestamp(std::string_view text, Timestamp& out) {
    if (text.size() < 20U || text.size() > 30U) {
        return false;
    }
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return false;
    }
    if (text.back() != 'Z') {
        return false;
    }
    std::int64_t year = 0;
    std::int64_t month = 0;
    std::int64_t day = 0;
    std::int64_t hour = 0;
    std::int64_t minute = 0;
    std::int64_t second = 0;
    if (!digits(text, 0, 4, year) || !digits(text, 5, 2, month) || !digits(text, 8, 2, day) ||
        !digits(text, 11, 2, hour) || !digits(text, 14, 2, minute) || !digits(text, 17, 2, second)) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    static constexpr unsigned kDaysPerMonth[12] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
    const auto month_index = static_cast<std::size_t>(month - 1);
    std::int64_t days_in_month = kDaysPerMonth[month_index];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (month == 2 && leap) {
        days_in_month = 29;
    }
    if (day > days_in_month) {
        return false;
    }
    std::int64_t nanos = 0;
    std::size_t offset = 19;
    if (text[offset] == '.') {
        ++offset;
        std::size_t fraction_digits = 0;
        std::int64_t fraction = 0;
        while (offset < text.size() - 1U && text[offset] >= '0' && text[offset] <= '9') {
            if (fraction_digits < 9U) {
                fraction = (fraction * 10) + (text[offset] - '0');
                ++fraction_digits;
            }
            ++offset;
        }
        if (fraction_digits == 0U) {
            return false;
        }
        while (fraction_digits < 9U) {
            fraction *= 10;
            ++fraction_digits;
        }
        nanos = fraction;
    }
    if (offset != text.size() - 1U) {
        return false;
    }
    const std::int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    out = ((days * 86400) + (hour * 3600) + (minute * 60) + second) * kNanosPerSecond + nanos;
    return true;
}

}  // namespace mc

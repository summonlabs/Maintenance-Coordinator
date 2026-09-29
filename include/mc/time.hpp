// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Time is injected, never sampled implicitly.  Every decision that depends on
// "now" - window eligibility, blackout overlap, exception expiry, approval
// validity - reads the injected Clock, so a test can reproduce any instant
// exactly and the engine never consults the wall clock behind the caller's back.

#ifndef MC_TIME_HPP
#define MC_TIME_HPP

#include <cstdint>
#include <string>

#include "mc/ident.hpp"

namespace mc {

inline constexpr std::int64_t kNanosPerSecond = 1000000000LL;
inline constexpr std::int64_t kNanosPerMinute = 60LL * kNanosPerSecond;
inline constexpr std::int64_t kNanosPerHour = 60LL * kNanosPerMinute;
inline constexpr std::int64_t kNanosPerDay = 24LL * kNanosPerHour;

class Clock {
public:
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    virtual ~Clock() = default;

    // Nanoseconds since the Unix epoch.  A clock never returns kNoTimestamp.
    [[nodiscard]] virtual Timestamp now_nanos() const = 0;
};

// Real host clock.  Used by the CLI and by tests that need a monotonic source.
class SystemClock final : public Clock {
public:
    [[nodiscard]] Timestamp now_nanos() const override;
};

// Deterministic clock for tests and for replaying recorded scenarios.
class ManualClock final : public Clock {
public:
    ManualClock() = default;
    explicit ManualClock(Timestamp start) : now_(start) {}

    [[nodiscard]] Timestamp now_nanos() const override { return now_; }

    void set(Timestamp value) { now_ = value; }
    void advance(Timestamp delta) { now_ += delta; }

private:
    Timestamp now_{1};
};

// RFC 3339 rendering in UTC with nanosecond precision, used by the CLI and by
// the human readable reports.  Parsing accepts the same shape; both are
// locale independent.
[[nodiscard]] std::string format_timestamp(Timestamp value);
[[nodiscard]] bool parse_timestamp(std::string_view text, Timestamp& out);

}  // namespace mc

#endif  // MC_TIME_HPP

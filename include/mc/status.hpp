// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Deterministic error model.
//
// The numeric value of every ErrorCode is part of the public contract: it *is*
// the validation precedence.  When several independent faults are detected for
// one request, the engine reports the fault with the lowest numeric value, so
// the same invalid request always resolves to the same primary error
// regardless of incidental map ordering, thread scheduling, or unrelated
// state.  Codes are therefore never renumbered and never reused.
//
// Class order == precedence order:
//   1xx request usability, 2xx identity/existence, 3xx authority/fencing,
//   4xx lifecycle, 5xx operating policy and exceptions, 6xx I/O and integrity,
//   7xx resource bounds, 8xx idempotent replay, 9xx internal fault.

#ifndef MC_STATUS_HPP
#define MC_STATUS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mc {

enum class ErrorCode : std::uint16_t {
    Ok = 0,

    // --- 100-199 : request usability.  Nothing about stored state is examined
    //     until the request itself is well formed.
    InvalidUsage = 100,
    UnknownCommand = 101,
    MissingArgument = 102,
    InvalidArgument = 103,
    MalformedInput = 104,
    UnsupportedFormatVersion = 105,

    // --- 200-299 : identity and existence.
    UnknownPlan = 200,
    UnknownTarget = 201,
    UnknownReceipt = 202,
    DuplicateIdentity = 203,
    IdentityMismatch = 204,
    UnknownObligation = 205,
    UnknownException = 206,
    UnknownAsset = 207,
    UnknownIncident = 208,

    // --- 300-399 : authority, generation and fencing.
    StaleRevision = 300,
    StaleAuthority = 301,
    StalePolicyGeneration = 302,
    StaleDependencyGeneration = 303,
    StaleCapacityGeneration = 304,
    StaleTopologyGeneration = 305,
    StaleMaintenanceGeneration = 306,
    StaleFacilityEpoch = 307,
    StaleHardwareGeneration = 308,
    StaleFirmwareGeneration = 309,
    StaleObservation = 310,
    MissingGeneration = 311,
    StaleApproval = 312,
    StaleLifecycleGeneration = 313,
    StalePlanDigest = 314,

    // --- 400-499 : lifecycle / state machine.
    IllegalPhase = 400,
    AlreadyTerminal = 401,
    NotApproved = 402,
    NotReady = 403,
    ObligationsOutstanding = 404,
    CompletionEvidenceMissing = 405,
    RestorationOutstanding = 406,
    ProtectedObligationViolated = 407,
    PlanBlocked = 408,
    RecoveryRequired = 409,
    WindowNotOpen = 410,
    ProgressOutOfOrder = 411,
    PhaseTransitionInvalid = 412,

    // --- 500-599 : operating policy and explicitly granted exceptions.
    ProtectedWindowActive = 500,
    BlackoutPeriodActive = 501,
    RedundancyInsufficient = 502,
    SpareCapacityInsufficient = 503,
    PowerHeadroomInsufficient = 504,
    CoolingHeadroomInsufficient = 505,
    ActiveIncident = 506,
    LifecycleStateInvalid = 507,
    ServiceClassViolation = 508,
    HardInterlock = 510,
    ExceptionExpired = 511,
    ExceptionInvalid = 512,
    ConcurrentMaintenanceConflict = 513,
    PersonnelEvidenceMissing = 514,
    ApprovalEvidenceMissing = 515,
    ScopeViolation = 516,
    WindowExpired = 517,
    ExceptionNotApplicable = 518,

    // --- 600-699 : I/O, durability and external process facts.
    IoError = 600,
    LockHeld = 601,
    LockError = 602,
    RecordCorrupt = 603,
    RecordTruncated = 604,
    ChecksumMismatch = 605,
    TrailingBytes = 606,
    ReservedFieldViolation = 607,
    LayoutInvalid = 608,
    CommitSequenceRegression = 609,
    PublicationFailed = 610,
    PathInvalid = 611,
    PathTraversalRejected = 612,
    ReadOnlyStore = 613,
    StoreNotOpen = 614,

    // --- 700-799 : resource bounds.
    LimitExceeded = 700,
    OutOfMemory = 701,

    // --- 800-899 : idempotent replay.
    DuplicateOperation = 800,
    ReplayIntentMismatch = 801,

    // --- 900-999 : internal fault.  Always last.
    InternalError = 900,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

// Parses the exact spelling produced by to_string().  The name "ok" is not
// accepted because no failing code may be spelled "ok"; use Status::success().
[[nodiscard]] bool parse_error_code(std::string_view name, ErrorCode& out) noexcept;

// Precedence helper: the more primary of two error codes.
[[nodiscard]] constexpr ErrorCode primary_of(ErrorCode a, ErrorCode b) noexcept {
    return static_cast<std::uint16_t>(a) <= static_cast<std::uint16_t>(b) ? a : b;
}

[[nodiscard]] constexpr bool is_precondition_code(ErrorCode code) noexcept {
    const auto v = static_cast<std::uint16_t>(code);
    return v >= 500 && v < 600;
}

[[nodiscard]] constexpr bool is_integrity_code(ErrorCode code) noexcept {
    const auto v = static_cast<std::uint16_t>(code);
    return v >= 600 && v < 700;
}

[[nodiscard]] constexpr bool is_authority_code(ErrorCode code) noexcept {
    const auto v = static_cast<std::uint16_t>(code);
    return v >= 300 && v < 400;
}

[[nodiscard]] constexpr bool is_lifecycle_code(ErrorCode code) noexcept {
    const auto v = static_cast<std::uint16_t>(code);
    return v >= 400 && v < 500;
}

// Conditions that may be waived by an explicit, scoped, attributable and
// expiring exception grant.  Hard safety interlocks are deliberately absent:
// an exception can never waive them.
[[nodiscard]] bool is_waivable_condition(ErrorCode code) noexcept;

// A structured failure.  `detail` carries the precise fact that failed, and
// `subject` names the object it failed on, so an operator can act without
// guessing.  `suppressed` records additional faults that were detected but
// outranked by `code`; it exists for evidence, never for control flow.
class Status {
public:
    Status() = default;

    Status(ErrorCode code, std::string message, std::string subject = {}, std::string detail = {})
        : code_(code),
          message_(std::move(message)),
          subject_(std::move(subject)),
          detail_(std::move(detail)) {}

    // The success value.  It is not called ok() because ok() is the predicate
    // every caller already uses, and overloading a factory onto a predicate
    // makes both harder to read.
    [[nodiscard]] static Status success() noexcept { return Status{}; }

    [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] std::string_view message() const noexcept { return message_; }
    [[nodiscard]] std::string_view subject() const noexcept { return subject_; }
    [[nodiscard]] std::string_view detail() const noexcept { return detail_; }
    [[nodiscard]] const std::vector<Status>& suppressed() const noexcept { return suppressed_; }

    // Records a lower-priority fault.  If `other` outranks the current code the
    // two are swapped so that the primary error stays the most significant one.
    void merge(const Status& other);

    [[nodiscard]] std::string render() const;

private:
    ErrorCode code_{ErrorCode::Ok};
    std::string message_;
    std::string subject_;
    std::string detail_;
    std::vector<Status> suppressed_;
};

// Convenience constructors keep call sites short and keep message text uniform.
[[nodiscard]] Status fail(ErrorCode code, std::string message);
[[nodiscard]] Status fail(ErrorCode code, std::string message, std::string subject);
[[nodiscard]] Status fail(ErrorCode code, std::string message, std::string subject, std::string detail);

// Result<T>: either a value or a Status.  Never both, never neither.
template <typename T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Status status) : status_(std::move(status)) {
        if (status_.ok()) {
            status_ = fail(ErrorCode::InternalError, "result constructed with an ok status");
        }
    }

    [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Status& status() const noexcept { return status_; }
    [[nodiscard]] const T& value() const& { return *value_; }
    [[nodiscard]] T& value() & { return *value_; }
    [[nodiscard]] T&& value() && { return std::move(*value_); }
    [[nodiscard]] const T& operator*() const& { return *value_; }
    [[nodiscard]] T& operator*() & { return *value_; }
    [[nodiscard]] T&& operator*() && { return std::move(*value_); }
    [[nodiscard]] T* operator->() { return &*value_; }
    [[nodiscard]] const T* operator->() const { return &*value_; }

private:
    std::optional<T> value_;
    Status status_;
};

}  // namespace mc

#endif  // MC_STATUS_HPP

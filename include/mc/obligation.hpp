// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Typed obligations and typed receipts.
//
// An obligation is a demand this coordinator places on an adjacent authority:
// "ASI must drain these assets", "DFI must isolate this path", "the plant must
// prove power is isolated".  A receipt is evidence that something happened.
//
// The central rule is that a receipt is never an obligation's state.  The
// disposition of an obligation is *derived* from the receipts that bind to it,
// so there is no stored field that a bug, a partial write or a replay could
// set to "Satisfied" without evidence.  Acknowledging a request is not an
// effect, so an acknowledgement-shaped receipt can never satisfy an obligation
// whose evidence must be an observation.

#ifndef MC_OBLIGATION_HPP
#define MC_OBLIGATION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "mc/digest.hpp"
#include "mc/ident.hpp"
#include "mc/scope.hpp"
#include "mc/status.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// Obligations
// ---------------------------------------------------------------------------

enum class ObligationKind : std::uint8_t {
    // Pre-drain: workload and traffic leave the target before anything is touched.
    AsiDrain = 0,
    AsiQuiesce = 1,
    DfiDrainTraffic = 2,
    // Isolation: the plant and the fabric are made safe to work on.
    DfiIsolatePath = 3,
    PowerIsolate = 4,
    CoolingAdjust = 5,
    LifecycleTransition = 6,
    // Presence and approval evidence owned by the operator.
    PersonnelOnSite = 7,
    ApprovalWitness = 8,
    // Completion: work is finished and verified.
    MaintenanceVerified = 9,
    WorkStopConfirmed = 10,
    // Restoration: the facility is returned to its protected posture.
    DrainRelease = 11,
    PowerRestore = 12,
    CoolingRestore = 13,
    TrafficRestore = 14,
    RedundancyRestore = 15,
};

[[nodiscard]] std::string_view to_string(ObligationKind kind) noexcept;
[[nodiscard]] bool parse_obligation_kind(std::string_view text, ObligationKind& out) noexcept;

// Which authority the obligation is placed on.  This coordinator requests; it
// never performs ASI, DFI or plant actuation itself.
enum class ObligationAuthority : std::uint8_t {
    Asi = 0,
    Dfi = 1,
    DccpPlant = 2,
    DccpInventory = 3,
    Operator = 4,
};

[[nodiscard]] std::string_view to_string(ObligationAuthority authority) noexcept;
[[nodiscard]] bool parse_obligation_authority(std::string_view text, ObligationAuthority& out) noexcept;

// The lifecycle stage an obligation belongs to.  Readiness gates on the
// pre-drain and isolation classes; completion gates on the final class.
enum class ObligationStage : std::uint8_t { PreDrain = 0, Isolation = 1, Completion = 2 };
[[nodiscard]] std::string_view to_string(ObligationStage stage) noexcept;

// Derived disposition.  Never stored.
enum class Disposition : std::uint8_t {
    Outstanding = 0,  // requested, no satisfying evidence yet
    Acknowledged = 1, // an authority accepted the request; not an effect
    Satisfied = 2,    // satisfying observed evidence is bound to it
    Failed = 3,       // the authority reported that the obligation failed
    Waived = 4,       // an explicit, scoped, unexpired exception covers it
    NotApplicable = 5 // the derived obligation set excludes it for this activity
};

[[nodiscard]] std::string_view to_string(Disposition disposition) noexcept;

class Obligation {
public:
    ObligationId id;
    ObligationKind kind{ObligationKind::AsiDrain};
    ObligationAuthority authority{ObligationAuthority::Asi};
    TargetRef target;
    std::string requirement;   // human readable statement of what must be proven
    bool mandatory{true};      // mandatory obligations are never waivable
    bool requires_observation{true};  // acknowledgement alone never satisfies
    // The precondition condition this obligation guards, when it has one.
    // A waived condition removes the obligation; a hard condition never does.
    ErrorCode guarded_by{ErrorCode::Ok};
    Digest digest;

    [[nodiscard]] Status validate() const;
    [[nodiscard]] Digest compute_digest() const;
    void finalize() { digest = compute_digest(); }

    [[nodiscard]] friend bool operator==(const Obligation&, const Obligation&) noexcept = default;
};

class ObligationSet {
public:
    std::vector<Obligation> obligations;  // canonical order by (stage, kind, target, id)
    Digest digest;

    void canonicalize();
    void finalize();
    [[nodiscard]] Status validate() const;
    [[nodiscard]] const Obligation* find(const ObligationId& id) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return obligations.empty(); }

    [[nodiscard]] friend bool operator==(const ObligationSet&, const ObligationSet&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Receipts
// ---------------------------------------------------------------------------

enum class ReceiptKind : std::uint8_t {
    DrainRequested = 0,
    DrainAcknowledged = 1,
    DrainObserved = 2,
    QuiesceObserved = 3,
    TrafficDrainObserved = 4,
    PathIsolationObserved = 5,
    PowerIsolationObserved = 6,
    CoolingAdjustmentObserved = 7,
    LifecycleTransitionObserved = 8,
    PersonnelOnSiteObserved = 9,
    ApprovalWitnessObserved = 10,
    MaintenanceVerifiedObserved = 11,
    WorkStopObserved = 12,
    DrainReleaseObserved = 13,
    PowerRestoreObserved = 14,
    CoolingRestoreObserved = 15,
    TrafficRestoreObserved = 16,
    RedundancyRestoreObserved = 17,
    FailureObserved = 18,
};

[[nodiscard]] std::string_view to_string(ReceiptKind kind) noexcept;
[[nodiscard]] bool parse_receipt_kind(std::string_view text, ReceiptKind& out) noexcept;

// True when a receipt of this kind is an observation of effect rather than a
// request or an acknowledgement.
[[nodiscard]] bool is_observation(ReceiptKind kind) noexcept;

// A receipt is accepted only when it binds to the exact obligation, plan
// revision, obligation digest and generation set it claims to be about, and
// only when its observation sequence is fresher than everything already known
// for the plan.
class Receipt {
public:
    ReceiptId id;
    ObligationId obligation;
    ReceiptKind kind{ReceiptKind::DrainObserved};
    ObligationAuthority issuer_authority{ObligationAuthority::Asi};
    std::string issuer;             // the accountable identity that issued it
    ObservationSequence observation_sequence;
    Revision plan_revision;
    Digest obligation_digest;       // the obligation set digest it binds to
    Digest evidence_digest;         // digest of the evidence payload
    std::string evidence;           // provenance: change record, sensor, ticket
    Timestamp observed_at{kNoTimestamp};  // when the issuer observed it
    Timestamp ingested_at{kNoTimestamp};  // when this coordinator accepted it
    Digest digest;

    [[nodiscard]] Status validate() const;
    [[nodiscard]] Digest compute_digest() const;
    void finalize() { digest = compute_digest(); }

    [[nodiscard]] friend bool operator==(const Receipt&, const Receipt&) noexcept = default;
};

// Derived status of one obligation, with the receipts that support it.
struct ObligationStatus {
    ObligationId id;
    ObligationKind kind{ObligationKind::AsiDrain};
    ObligationAuthority authority{ObligationAuthority::Asi};
    ObligationStage stage{ObligationStage::PreDrain};
    Disposition disposition{Disposition::Outstanding};
    bool mandatory{true};
    TargetRef target;
    std::string detail;
    std::vector<ReceiptId> supporting_receipts;  // canonical order
    ExceptionId waiver;                          // set only for Disposition::Waived

    [[nodiscard]] friend bool operator==(const ObligationStatus&, const ObligationStatus&) noexcept = default;
};

class ObligationStatusReport {
public:
    Revision plan_revision;
    Digest obligation_set_digest;
    std::vector<ObligationStatus> statuses;  // canonical order
    bool all_satisfied{false};
    bool ready_for_pre_drain{false};  // every PreDrain obligation satisfied or waived
    bool ready_for_isolation{false};  // every PreDrain and Isolation obligation satisfied or waived
    bool ready_for_completion{false}; // every obligation satisfied or waived
    Digest digest;

    void finalize();
    [[nodiscard]] std::vector<ObligationId> outstanding(ObligationStage stage) const;
    [[nodiscard]] const ObligationStatus* find(const ObligationId& id) const noexcept;

    [[nodiscard]] friend bool operator==(const ObligationStatusReport&, const ObligationStatusReport&) noexcept = default;
};

// The stage an obligation kind belongs to.
[[nodiscard]] ObligationStage stage_of(ObligationKind kind) noexcept;
// The authority that owns an obligation kind.
[[nodiscard]] ObligationAuthority authority_of(ObligationKind kind) noexcept;
// The receipt kind that satisfies an obligation kind, or ErrorCode::Ok when the
// obligation cannot be satisfied by a receipt at all.
[[nodiscard]] ReceiptKind satisfying_receipt(ObligationKind kind) noexcept;
// True when the receipt kind is the request/acknowledgement shape of the
// obligation, which must never satisfy it.
[[nodiscard]] bool is_request_or_acknowledgement(ReceiptKind kind) noexcept;

}  // namespace mc

#endif  // MC_OBLIGATION_HPP

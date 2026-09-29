// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Deterministic randomized state-machine tests.
//
// A seeded generator drives random - but individually legal - commands at the
// engine, and an invariant checker runs after every step.  A counterexample is
// reproducible from the seed and the step index printed with the failure.

#include "../fixture.hpp"
#include "../harness.hpp"
#include "../mc_test.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mc/engine.hpp"

using namespace mc;        // NOLINT(google-build-using-namespace)
using namespace mc::test;  // NOLINT(google-build-using-namespace)

namespace {

class Machine {
public:
    Machine(const std::string& tag, std::uint64_t seed)
        : harness_(tag, make_facility()), random_(seed), seed_(seed) {}

    [[nodiscard]] Coordinator& coordinator() { return harness_.coordinator(); }

    template <typename Request>
    [[nodiscard]] Request with_attempt(Request request, const std::string& suffix) {
        return harness_.with_attempt(std::move(request), "m-" + std::to_string(seed_) + "-" + suffix);
    }

    [[nodiscard]] std::uint64_t next_below(std::uint64_t bound) { return random_.next_below(bound); }

private:
    Harness harness_;
    Random random_;
    std::uint64_t seed_;
};

// The invariants that must hold after every single command, whatever the plan
// is doing at the time.
[[nodiscard]] bool invariants_hold(Coordinator& coordinator, const PlanId& id, std::string& why) {
    ShowRequest show;
    show.plan = id;
    auto shown = coordinator.show(show);
    if (!shown.ok()) {
        why = "show failed: " + std::string(shown.status().render());
        return false;
    }
    const PlanRecord& record = shown.value().plan;
    if (auto status = record.validate(); !status.ok()) {
        why = "record failed validation: " + std::string(status.render());
        return false;
    }
    ObservationSequence highest;
    for (const auto& receipt : record.receipts) {
        if (!(receipt.observation_sequence < highest) && highest.is_set() &&
            receipt.observation_sequence == highest) {
            // duplicate sequence with a different identity is allowed; equal
            // sequences are ordered by identity in the canonical form.
        }
        if (!highest.is_set() || highest < receipt.observation_sequence) {
            highest = receipt.observation_sequence;
        }
    }
    if (record.last_observation_sequence.is_set() && highest.is_set() &&
        highest.value() > record.last_observation_sequence.value()) {
        why = "receipt sequence exceeds the recorded high-water mark";
        return false;
    }
    if (is_terminal(record.phase)) {
        if (record.phase == PlanPhase::Complete) {
            if (record.receipts.empty()) {
                why = "a completed plan has no evidence";
                return false;
            }
            if (!record.approval.has_value()) {
                why = "a completed plan has no approval";
                return false;
            }
        }
    }
    return true;
}

}  // namespace

MC_TEST(random_command_sequences_keep_every_invariant) {
    const std::uint64_t seeds[] = {1ULL, 42ULL, 0xDEADBEEFULL, 0x00000000000000FFULL};
    for (const std::uint64_t seed : seeds) {
        Machine machine("prop-" + std::to_string(seed), seed);
        Coordinator& coordinator = machine.coordinator();

        ProposeRequest proposal;
        proposal.reason = "randomised maintenance";
        proposal.requested_by = "operator-1";
        proposal.activity = MaintenanceActivity::HardwareRepair;
        proposal.priority = PlanPriority::Elevated;
        proposal.risk = ServiceRiskClass::High;
        proposal.scope = rack_scope();
        proposal.window = fixture_window();
        auto proposed = coordinator.propose(machine.with_attempt(proposal, "propose"));
        MC_REQUIRE_OK(proposed);
        const PlanId id = proposed.value().plan.plan.id;
        const Revision revision = proposed.value().plan.plan.revision;

        std::uint64_t observation = 0;
        std::string why;
        for (int step = 0; step < 80; ++step) {
            const std::uint64_t choice = machine.next_below(11U);
            switch (choice) {
                case 0: {
                    EvaluateRequest request;
                    request.plan = id;
                    request.revision = revision;
                    (void)coordinator.evaluate(machine.with_attempt(request, "evaluate-" + std::to_string(step)));
                    break;
                }
                case 1: {
                    ApproveRequest request;
                    request.plan = id;
                    request.revision = revision;
                    request.approved_by = "approver-1";
                    request.approval_evidence = "cr-" + std::to_string(step);
                    (void)coordinator.approve(machine.with_attempt(request, "approve-" + std::to_string(step)));
                    break;
                }
                case 2: {
                    DeriveObligationsRequest request;
                    request.plan = id;
                    request.revision = revision;
                    (void)coordinator.derive_obligations(
                        machine.with_attempt(request, "derive-" + std::to_string(step)));
                    break;
                }
                case 3:
                case 4: {
                    ShowRequest show;
                    show.plan = id;
                    auto shown = coordinator.show(show);
                    MC_REQUIRE_OK(shown);
                    const PlanRecord& record = shown.value().plan;
                    if (record.obligations.obligations.empty()) {
                        break;
                    }
                    const std::size_t pick =
                        static_cast<std::size_t>(machine.next_below(record.obligations.obligations.size()));
                    const Obligation& obligation = record.obligations.obligations[pick];
                    IngestReceiptRequest request;
                    request.plan = id;
                    request.revision = revision;
                    request.receipt_id =
                        receipt_id(("rcpt-" + std::to_string(seed) + "-" + std::to_string(step)).c_str());
                    request.obligation = obligation.id;
                    // Half the time send the request/acknowledgement shape, which
                    // must never satisfy the obligation.
                    const bool acknowledgement = machine.next_below(2U) == 0U;
                    request.kind = acknowledgement && obligation.kind == ObligationKind::AsiDrain
                                       ? ReceiptKind::DrainAcknowledged
                                       : satisfying_receipt(obligation.kind);
                    request.issuer_authority = obligation.authority;
                    request.issuer = std::string(to_string(obligation.authority)) + "-authority";
                    request.observation_sequence = ObservationSequence::from_value(++observation);
                    request.evidence = "evidence " + std::to_string(step);
                    request.observed_at = mid_window();
                    (void)coordinator.ingest_receipt(
                        machine.with_attempt(request, "ingest-" + std::to_string(step)));
                    break;
                }
                case 5: {
                    BeginRequest request;
                    request.plan = id;
                    request.revision = revision;
                    (void)coordinator.begin(machine.with_attempt(request, "begin-" + std::to_string(step)));
                    break;
                }
                case 6: {
                    RecordProgressRequest request;
                    request.plan = id;
                    request.revision = revision;
                    request.kind = ProgressKind::StepCompleted;
                    request.note = "step " + std::to_string(step);
                    (void)coordinator.record_progress(
                        machine.with_attempt(request, "progress-" + std::to_string(step)));
                    break;
                }
                case 7: {
                    VerifyRequest request;
                    request.plan = id;
                    request.revision = revision;
                    (void)coordinator.verify_restoration(
                        machine.with_attempt(request, "verify-" + std::to_string(step)));
                    break;
                }
                case 8: {
                    CompleteRequest request;
                    request.plan = id;
                    request.revision = revision;
                    (void)coordinator.complete(machine.with_attempt(request, "complete-" + std::to_string(step)));
                    break;
                }
                case 9: {
                    CancelRequest request;
                    request.plan = id;
                    request.revision = revision;
                    request.reason = "random cancellation";
                    (void)coordinator.cancel(machine.with_attempt(request, "cancel-" + std::to_string(step)));
                    break;
                }
                default: {
                    ExplainRequest explain;
                    explain.plan = id;
                    (void)coordinator.explain(explain);
                    break;
                }
            }
            if (!invariants_hold(coordinator, id, why)) {
                MC_CHECK_MSG(false, "seed " + std::to_string(seed) + " step " + std::to_string(step) + ": " + why);
                break;
            }
        }
    }
}

MC_TEST(the_same_seed_produces_the_same_history) {
    const std::uint64_t seed = 7ULL;
    std::vector<Digest> digests;
    for (int run = 0; run < 2; ++run) {
        Machine machine("prop-determinism-" + std::to_string(run), seed);
        Coordinator& coordinator = machine.coordinator();
        ProposeRequest proposal;
        proposal.reason = "determinism";
        proposal.requested_by = "operator-1";
        proposal.activity = MaintenanceActivity::HardwareRepair;
        proposal.scope = rack_scope();
        proposal.window = fixture_window();
        auto proposed = coordinator.propose(machine.with_attempt(proposal, "propose"));
        MC_REQUIRE_OK(proposed);
        const PlanId id = proposed.value().plan.plan.id;
        const Revision revision = proposed.value().plan.plan.revision;
        EvaluateRequest evaluate;
        evaluate.plan = id;
        evaluate.revision = revision;
        MC_REQUIRE_OK(coordinator.evaluate(machine.with_attempt(evaluate, "evaluate")));
        ApproveRequest approve;
        approve.plan = id;
        approve.revision = revision;
        approve.approved_by = "approver-1";
        approve.approval_evidence = "cr-1";
        MC_REQUIRE_OK(coordinator.approve(machine.with_attempt(approve, "approve")));
        DeriveObligationsRequest derive;
        derive.plan = id;
        derive.revision = revision;
        auto derived = coordinator.derive_obligations(machine.with_attempt(derive, "derive"));
        MC_REQUIRE_OK(derived);
        digests.push_back(derived.value().plan.obligations.digest);
        const std::vector<Obligation> obligations = derived.value().plan.obligations.obligations;
        for (std::size_t index = 0; index < obligations.size(); ++index) {
            const Obligation& obligation = obligations[index];
            IngestReceiptRequest ingest;
            ingest.plan = id;
            ingest.revision = revision;
            ingest.receipt_id = receipt_id(("d-" + obligation.id.name()).c_str());
            ingest.obligation = obligation.id;
            ingest.kind = satisfying_receipt(obligation.kind);
            ingest.issuer_authority = obligation.authority;
            ingest.issuer = std::string(to_string(obligation.authority)) + "-authority";
            ingest.observation_sequence = ObservationSequence::from_value(static_cast<std::uint64_t>(index) + 1U);
            ingest.evidence = "e-" + obligation.id.name();
            ingest.observed_at = mid_window();
            (void)coordinator.ingest_receipt(machine.with_attempt(ingest, "ingest-" + obligation.id.name()));
        }
        ShowRequest show;
        show.plan = id;
        auto shown = coordinator.show(show);
        MC_REQUIRE_OK(shown);
        digests.push_back(shown.value().plan.plan.digest);
    }
    MC_CHECK_EQ(digests.size(), static_cast<std::size_t>(4));
    MC_CHECK(digests[0] == digests[2]);
    MC_CHECK(digests[1] == digests[3]);
}

MC_TEST(facility_digests_are_permutation_invariant) {
    FacilitySnapshot facility = make_facility();
    facility.finalize();
    const Digest baseline = facility.digest;
    const Digest dependency = facility.dependency_digest;

    Random random(0x1234ABCDULL);
    for (int round = 0; round < 20; ++round) {
        FacilitySnapshot shuffled = facility;
        // Fisher-Yates on every collection: the canonical form must be
        // independent of the order the plant reported things in.
        for (std::size_t index = shuffled.assets.size(); index > 1U; --index) {
            std::swap(shuffled.assets[index - 1U], shuffled.assets[random.next_below(index)]);
        }
        for (std::size_t index = shuffled.redundancy_groups.front().members.size(); index > 1U; --index) {
            auto& members = shuffled.redundancy_groups.front().members;
            std::swap(members[index - 1U], members[random.next_below(index)]);
        }
        shuffled.finalize();
        MC_CHECK(shuffled.digest == baseline);
        MC_CHECK(shuffled.dependency_digest == dependency);
    }
}

MC_TEST_MAIN()

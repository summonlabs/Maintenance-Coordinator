# Maintenance Coordinator

Facility maintenance-window coordination for the Data Center Control Plane (DCCP).

The Maintenance Coordinator answers one question and refuses to pretend it knows the answer to any
other:

> **Can this maintenance activity proceed now without violating dependencies, redundancy, protected
> obligations, service classes, or active facility constraints — and what sequence of
> drains, isolations and verifications is required before, during and after the window?**

It is DCCP repository 38 of 72, and it sits in tranche 5 (physical fleet lifecycle). It is C++20,
dependency-free beyond the standard library, and builds warning-clean under `/W4 /WX` (MSVC) and
`-Wall -Wextra -Wpedantic -Werror` (GCC/Clang).

---

## 1. Systems boundary and explicit non-ownership

**This repository owns** the coordination of facility-level maintenance windows across physical
dependencies, redundancy, protected obligations, asset and rack lifecycle, power and cooling
constraints, ASI/DFI consumers, service risk, and maintenance completion evidence. Concretely it:

* records and revisions maintenance plans with the exact view they were planned against;
* evaluates preconditions against a supplied facility model and reports every condition, satisfied
  or not, with the numbers behind it;
* derives typed obligations for adjacent authorities (ASI drains, DFI drainage and path isolation,
  plant isolation and cooling, inventory lifecycle transitions, operator presence) and tracks the
  evidence that answers them;
* authorises the start of work and refuses it when the evidence, the window or the generations do
  not support it;
* fences stale authority: any relevant incident, dependency, redundancy, capacity, policy,
  topology, maintenance or control-epoch change invalidates an approval instead of silently
  inheriting it;
* tracks verified completion, and refuses to call a window complete without evidence that the
  protected obligations and restoration conditions hold;
* records explicit, scoped, attributable, expiring exceptions — which can never set aside a hard
  safety interlock.

**This repository does not own**, and contains no code that attempts to do any of the following:

| Not owned here | Owner |
| --- | --- |
| Executing hardware repair, installing parts, walking the floor | the physical operation |
| Scheduling or migrating accelerator jobs | ASI |
| Rerouting, draining or isolating network traffic and paths | DFI |
| Switching, locking out or restoring power | the plant controls |
| Actuating cooling | the plant controls |
| Owning tenant policy, contracts or protection entitlements | tenancy/policy authority |
| Diagnosing incidents | incident management |
| Deciding that a facility is safe because a sensor is missing | nobody: a missing measurement is reported as unmeasured and blocks |

There is no telemetry, no phone-home, no network access at all.

## 2. Core question, answered as a value

Asking the core question is a command, and the answer is a document:

```
maintenance-coordinator explain --plan plan-000001 --json
```

returns the current phase, every blocking condition with its measured and required values, the
status of every obligation with the receipts that support it, the reasons the plan is where it is,
the next steps that would move it, and five booleans (`can_evaluate`, `can_approve`,
`can_begin`, `can_complete`, `can_cancel`) derived from the same gates the mutating commands use.
The engine never reports "probably fine": every claim in that document is either evidence bound to
an identity, or an explicit statement that something is missing.

## 3. Lifecycle model

```
Proposed ──evaluate──▶ Evaluating ──approve──▶ Approved ──derive-obligations──▶ PreDrain
                                                                                   │
                                                      (all pre-drain + isolation obligations satisfied)
                                                                                   ▼
                                                                                 Ready
                                                                                   │ begin
                                                                                   ▼
                                                                              InProgress
                                                        work-completed ──────────┐  │ work-stopped
                                                                                 ▼  ▼
                                                                          Verification  Restore
                                                                                 │ verify-restoration
                                                                                 ▼
                                                                              Restore ──complete──▶ Complete
                                                                                 │
                                                                                 └──cancel (only when
                                                                                     restoration is proven)──▶ Cancelled

Blocked is reachable from Proposed/Evaluating/Blocked by an unsatisfied precondition and is
re-entered through evaluate. Blocked is not terminal.
```

The invariants this state machine enforces, each with a test that fails if it stops holding:

* **Scheduled is not approved.** A plan is recorded by `propose` and nothing else; no obligation
  exists and no gate is open.
* **Approved is not ready.** `approve` binds an approval; `begin` still refuses until every
  pre-drain and isolation obligation is satisfied.
* **Drain requested is not drained.** A `drain-requested` or `drain-acknowledged` receipt is
  recorded as evidence and leaves the obligation `acknowledged`; only an observed effect satisfies it.
* **Maintenance started is not completed.** `begin` moves to `in-progress`; completion needs the
  completed obligations *and* a verified restoration.
* **Completion requires evidence.** `complete` refuses while any obligation is outstanding, while a
  protected obligation is violated, or while the approval no longer describes the facility.
* **Cancellation after work has started requires restoration.** An interrupted window can only be
  cancelled once the facility is demonstrably back in its protected posture.
* **A process exit is not authoritative completion.** A window that was in flight when the process
  died is forced into restoration and needs *fresh* evidence (a strictly newer observation
  sequence) before it can move again.

## 4. Authority, generations and fencing

Every decision binds to the exact identity, generation, epoch, revision and evidence it was taken
against. The facility model carries eight independent fences:

| Fence | Meaning | Error when it moves |
| --- | --- | --- |
| `FacilityEpoch` | the facility view itself was replaced | `StaleFacilityEpoch` |
| `ControlEpoch` | authority moved | `StaleAuthority` |
| `PolicyGeneration` | the operating policy changed | `StalePolicyGeneration` |
| `DependencyGeneration` | the dependency projection changed | `StaleDependencyGeneration` |
| `CapacityGeneration` | spare capacity changed | `StaleCapacityGeneration` |
| `TopologyGeneration` | the fabric topology changed | `StaleTopologyGeneration` |
| `MaintenanceGeneration` | concurrent maintenance state changed | `StaleMaintenanceGeneration` |
| `Revision` | the plan itself was revised | `StaleRevision` |

An approval also carries the digests of the plan, the facility view, the policy document, the
dependency projection and the evaluation it was granted from; a change that keeps a generation
number but alters content is caught by `StaleApproval` at the digest level. Per-asset
`LifecycleGeneration`, `HardwareGeneration` and `FirmwareGeneration` bind the evidence that
concerns that asset.

Every identity, generation, epoch and counter is a distinct strong type
(`AssetId`, `LifecycleGeneration`, `PolicyGeneration`, `CommitSequence`, …). Mixing a
lifecycle generation with a hardware generation, or an asset identity with a rack identity, does
not compile. A generation that was never observed is `unset`, never zero: `MissingGeneration` is
reported rather than a decision taken against a made-up `0`.

## 5. Preconditions

`evaluate` reports every condition that applies to the plan's scope and window, whether it passes
or fails. A failure is either **hard** (a safety interlock: never waivable, always blocking) or
**soft** (waivable only by an explicit exception).

| Condition | Raised when | Hard? |
| --- | --- | --- |
| `ProtectedObligationViolated` | maintenance would remove redundancy a protection requires | yes when the protection says so |
| `ActiveIncident` | an incident is active in scope | yes when the incident is a hard block |
| `HardInterlock` | the power domain or cooling zone is interlocked | always |
| `LifecycleStateInvalid` | a target is not in a serviceable lifecycle state | always |
| `ProtectedWindowActive` | a facility-wide protected window covers the requested time | per the window |
| `BlackoutPeriodActive` | a scoped blackout covers the requested time | per the blackout |
| `RedundancyInsufficient` | surviving units would fall below required + policy margin | no |
| `SpareCapacityInsufficient` | spare capacity would fall below the policy floor | no |
| `PowerHeadroomInsufficient` | power headroom below policy, **or not measured at all** | no |
| `CoolingHeadroomInsufficient` | cooling headroom below policy, **or not measured at all** | no |
| `ServiceClassViolation` | invasive work on a mission-critical asset without a high risk class | no |
| `ConcurrentMaintenanceConflict` | another plan already holds an overlapping scope | yes |

Redundancy is computed, not asserted: the surviving units of every redundancy group in scope are
the observed units minus the capacity of the scoped member assets, compared against the group's own
requirement plus the policy margin. An unmeasured headroom is reported as unmeasured with
`measured=false` and blocks the plan — a missing sensor is never rounded up to "healthy".

## 6. Obligations and evidence

An obligation is a demand this coordinator places on an adjacent authority; a receipt is evidence
that something happened. **An obligation has no stored disposition**: its state is derived from the
receipts bound to it, so no partial write, replay or bug can set an obligation to "satisfied"
without evidence. A receipt is accepted only when it binds to:

* the exact obligation identity (and it must be the kind that answers that obligation);
* the current plan revision and the current obligation-set digest;
* the authority that owns the obligation (a DFI receipt cannot answer an ASI obligation);
* a strictly newer observation sequence than every receipt already accepted for that plan;
* non-empty provenance, an observation instant, and a digest over that provenance.

Obligations are derived deterministically from the activity and the scope: work on four assets
across two racks produces per-asset drains and quiesces, per-segment traffic drains and path
isolations, per-domain power isolation, per-zone cooling adjustment, per-asset lifecycle
transitions, operator presence, and the symmetric restoration obligations (drain release, power
restore, cooling restore, traffic restore, redundancy restore), plus the verification obligations
(`maintenance-verified`, `work-stop-confirmed`).

## 7. Exceptions

The only way a policy condition may be set aside is an `ExceptionGrant`, which is:

* **explicit** — it names the conditions it waives by error code;
* **scoped** — its targets must lie inside the plan scope (`ScopeViolation` otherwise);
* **attributable** — a named grantor and a non-empty justification;
* **expiring** — a positive validity period, checked at every gate;
* **bounded** — an attempt to waive anything outside the waivable set is refused with
  `HardInterlock`, and an exception can never cover a hard safety interlock, a protected
  obligation with `hard_interlock`, an invalid lifecycle state, or a concurrent-window conflict.

Granting an exception changes the evaluation, so it is only accepted before approval; the plan must
be re-evaluated and re-approved for the waiver to take effect, and the approval records which
exceptions it relied on.

## 8. Persistence, recovery and the commit point

The durable store is a directory containing three files:

| File | Contents |
| --- | --- |
| `journal.mcl` | append-only, framed transactions; the commit record is the atomic commit point |
| `snapshot.mcs` | a compacted ledger image, published by stage → flush → read back and verify → atomic replace |
| `mc.lock` | the single-writer lock, taken with `LockFileEx` and released by the kernel if the owner dies |

Every frame is 48 bytes of header plus a payload:

```
u32 magic "MCJ1" | u16 format version | u16 reserved (must be zero) |
u32 payload length | u32 CRC-32 of the payload | 32 byte SHA-256 of the payload | payload
```

A transaction is a mutation record followed by its commit record. The commit record carries the
commit sequence and a digest of the mutation payload. Recovery reads the snapshot, then replays
only the journal records whose sequence is strictly greater than the snapshot's, and:

* an incomplete trailing record is treated as an interrupted append and truncated away;
* a mutation record whose commit record never arrived is **discarded**, never merged;
* a complete record that fails its CRC-32 or SHA-256 is an error (`ChecksumMismatch`) and the
  store refuses to open rather than guess;
* an unknown format version, a non-zero reserved field, an impossible length or an out-of-range
  enum is rejected with a specific code;
* exactly one generation is authoritative after recovery: partially committed states are never
  merged, and replayed records never rewind a plan.

Recovery also advances the process incarnation, and reports how many records were replayed, how
many transactions were discarded, how many bytes were truncated. A read-only open takes no lock and
can read while a writer is running: it observes either the previous committed generation or the
next one, never a torn one, because the snapshot is replaced atomically and the journal is
self-framing.

**Platform boundary.** The durable store is implemented against the Win32 file API, because the
claims it makes — crash consistency, atomic publication, cross-process exclusion — are claims about
an operating system and are only worth making where they have been proven on the host. A build for
another platform fails with a clear message. Everything above the store (model, gates, engine,
reports, CLI) is portable C++20.

**What process-death tests prove, and what they do not.** The durability suite terminates real
processes with `TerminateProcess` at four meaningful points (after the mutation record, after the
commit record, after staging a snapshot, after publishing a snapshot but before truncating the
journal). Process termination does not lose data the operating system has already accepted into its
file cache, so these tests prove the framing and recovery rules — that an uncommitted transaction
is discarded even though its bytes are on disk, and that a committed one is recovered exactly once
— not the behaviour of a power loss. That boundary is stated here rather than blurred in a test
name.

## 9. Concurrency model

* One state mutex serialises the commands a `Coordinator` executes.
* Lock order is always coordinator mutex → store mutex, and never the reverse; the store never
  calls back into the coordinator.
* No callback, sink, allocator hook or user code runs while the state mutex is held. Events are
  emitted after it is released, which is why a sink may legally call back into the same coordinator
  — the concurrency suite proves exactly that, and would hang if the rule were ever broken.
* A command either commits a complete record or leaves both the durable state and the in-memory
  ledger exactly as they were; the in-memory record is byte-identical to the one written, which is
  what makes a restart invisible to a client.
* Writers are excluded across processes by the OS lock; readers are not blocked by it.

## 10. Error model

The numeric value of every `ErrorCode` **is** the validation precedence, and codes are never
renumbered or reused:

| Range | Class | Meaning |
| --- | --- | --- |
| 100–199 | request usability | nothing about stored state is examined first |
| 200–299 | identity and existence | |
| 300–399 | authority, generation, fencing | |
| 400–499 | lifecycle | |
| 500–599 | operating policy and exceptions | |
| 600–699 | I/O, durability and integrity | |
| 700–799 | resource bounds | |
| 800–899 | idempotent replay | |
| 900–999 | internal fault | |

When one request contains several faults, the lowest-numbered one is reported as primary and the
others are attached as `suppressed` evidence, so the same invalid request always resolves to the
same primary error regardless of container ordering, thread scheduling or unrelated state. The CLI
maps the class to the exit code (1…8), and prints the same object in `--json` mode on success and
failure.

## 11. Idempotent replay

Every mutating command carries an `AttemptId` and the digest of the intent it believes it is
executing. A command that has already been applied is answered from the durable attempt table with
`replayed: true` and the current authoritative state, instead of being applied twice — so a retry
after a dropped response is safe. The same attempt identity with a *different* intent is refused
with `ReplayIntentMismatch`, and a tampered intent digest is refused before anything else happens.
The command line tool derives the attempt identity from the intent digest when none is given, so
running the same command line twice applies once.

## 12. Command line tool

```
maintenance-coordinator <command> [options]

global:  --store <dir>   --read-only   --json   --now <rfc3339>   --actor <id>   --attempt <id>
         --help          --version
```

| Command | Purpose |
| --- | --- |
| `init` | open or create the store and report its recovery summary |
| `install-facility --file <path\|->` | install a facility model (strict JSON; unknown members rejected) |
| `propose` | record a plan: reason, requester, activity, priority, risk, targets, window |
| `evaluate` | evaluate every precondition and record the report |
| `approve` | approve this revision against this generation set |
| `derive-obligations` | derive the typed obligation set for the plan |
| `ingest` | accept one receipt as evidence for one obligation |
| `grant-exception` | grant an explicit, scoped, expiring waiver |
| `begin` | start the window (ready + fence + window + fresh preconditions) |
| `progress --kind <started\|step-completed\|work-completed\|work-stopped\|issue-observed>` | record progress; `work-completed` moves to verification, `work-stopped` to restoration |
| `verify-restoration` | verify the maintenance outcome and move to restoration |
| `complete` | close the window with evidence |
| `cancel` | cancel (before work, or after restoration is proven) |
| `recover [--plan <id>]` | recover an interrupted window; forces restoration and requires fresh evidence |
| `explain` / `show` / `list` | read-only: explanation, full record, summaries |
| `audit` | list the durable journal framing |
| `compact` | publish a verified snapshot and truncate the replayed journal |

Exit codes: `0` success, `1` usage (1xx), `2` identity (2xx), `3` authority/fencing (3xx),
`4` lifecycle (4xx), `5` policy (5xx), `6` I/O and integrity (6xx/7xx), `7` replay (8xx),
`8` internal (9xx). With `--json`, exactly one JSON object is printed on stdout — on success and
on failure — and stderr stays empty.

A complete window, abridged:

```bash
maintenance-coordinator init --store ./store --json
maintenance-coordinator install-facility --store ./store --file facility.json --json
maintenance-coordinator propose --store ./store --reason "replace failed accelerator" \
    --requester operator-1 --activity hardware-repair --priority elevated --risk high \
    --target rack:rack-01 --start 2026-01-01T00:00:00Z --end 2026-01-01T08:00:00Z --json
maintenance-coordinator evaluate  --store ./store --plan plan-000001 --revision 1 --json
maintenance-coordinator approve   --store ./store --plan plan-000001 --revision 1 \
    --approver approver-1 --evidence change-record-4711 --json
maintenance-coordinator derive-obligations --store ./store --plan plan-000001 --revision 1 --json
maintenance-coordinator ingest --store ./store --plan plan-000001 --revision 1 \
    --receipt receipt-1 --obligation obligation-1-asi-drain --kind drain-observed \
    --authority asi --issuer asi-authority --sequence 1 --evidence "drain observed by ASI" --json
# ... the remaining obligations ...
maintenance-coordinator begin    --store ./store --plan plan-000001 --revision 1 --now 2026-01-01T02:00:00Z --json
maintenance-coordinator progress --store ./store --plan plan-000001 --revision 1 --kind work-completed --json
maintenance-coordinator verify-restoration --store ./store --plan plan-000001 --revision 1 --json
maintenance-coordinator complete --store ./store --plan plan-000001 --revision 1 --json
```

The facility document is strict JSON: an unknown member is `MalformedInput`, a missing required
member is `MissingArgument`, arrays may be omitted to mean "empty", timestamps are RFC 3339 UTC and
targets are spelled `kind:name`.

```json
{
  "revision": 1, "facility_epoch": 1, "policy_generation": 1, "dependency_generation": 1,
  "capacity_generation": 1, "topology_generation": 1, "maintenance_generation": 1, "control_epoch": 1,
  "observed_at": "2026-01-01T00:00:00Z",
  "policy": {"id": "policy-1", "revision": 1, "min_redundancy_margin_units": 0,
             "min_spare_capacity_units": 0, "min_power_headroom_milliwatts": 1,
             "min_cooling_headroom_units": 1, "approval_validity_hours": 8, "max_window_hours": 168,
             "max_concurrent_windows_per_rack": 1, "require_personnel_evidence": true,
             "require_dual_approval": false, "require_work_stop_evidence": true,
             "require_restoration_evidence": true},
  "assets": [{"id": "asset-01", "rack": "rack-01", "site": "site-a", "lifecycle": "in-service",
              "service_class": "business-critical", "lifecycle_generation": 1,
              "hardware_generation": 1, "firmware_generation": 1, "capacity_units": 1,
              "isolatable": true, "drainable": true, "healthy": true}],
  "redundancy_groups": [{"id": "rg-a", "site": "site-a", "mode": "n+2", "required_units": 2,
                         "observed_available_units": 4, "members": ["asset-01", "asset-02"]}],
  "capacity_pools": [{"id": "pool-a", "site": "site-a", "units_total": 10, "units_available": 8,
                      "units_protected": 2}],
  "power_domains": [{"id": "pd-a", "site": "site-a",
                     "headroom": {"measured": true, "available_units": 20, "required_units": 5},
                     "interlocked": false, "interlock_reason": ""}],
  "cooling_zones": [{"id": "cz-a", "site": "site-a",
                     "headroom": {"measured": true, "available_units": 20, "required_units": 5},
                     "interlocked": false, "interlock_reason": ""}],
  "fabric_segments": [{"id": "fs-a", "site": "site-a", "available_paths": 4, "required_paths": 2,
                       "drainable": true}],
  "blackouts": [{"id": "b1", "targets": ["rack:rack-01"], "start": "2026-02-01T00:00:00Z",
                 "end": "2026-02-01T06:00:00Z", "hard": false, "reason": "tenant change freeze"}],
  "incidents": [{"id": "i1", "targets": ["site:site-a"], "severity": "high", "active": true,
                 "hard_block": false, "summary": "cooling degradation"}],
  "protected_obligations": [{"id": "p1", "targets": ["site:site-a"], "required_mode": "n+1",
                             "required_units": 2, "tolerance_units": 0, "hard_interlock": true,
                             "description": "tenant A requires N+1 throughout its contract"}]
}
```

## 13. Library use

```cpp
#include "mc/engine.hpp"
#include "mc/store.hpp"
#include "mc/time.hpp"

mc::StoreOptions options;
options.directory = "store";
auto store = mc::Store::open(options).value();      // Result<T>: value or Status

mc::SystemClock clock;
mc::Coordinator coordinator(*store, clock);

mc::ProposeRequest request;
request.reason = "replace failed accelerator";
request.requested_by = "operator-1";
request.activity = mc::MaintenanceActivity::HardwareRepair;
request.scope = /* MaintenanceScope of targets */;
request.window = /* WindowSpec */;
request.header.attempt = /* AttemptId */;
request.header.intent_digest = mc::intent_digest_of(request);

auto result = coordinator.propose(request);
if (!result.ok()) {
    // result.status().code() is the primary error; .render() explains it.
}
auto explained = coordinator.explain(mc::ExplainRequest{result.value().plan.plan.id});
```

Every request that mutates state carries the same two fields, and every result carries the same
shape: a `MutationOutcome` (intent digest, outcome digest, commit sequence, replay flag), the
current authoritative `PlanRecord`, and whichever report the command produced.

## 14. Installation and downstream consumption

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMC_BUILD_TESTS=ON -DMC_BUILD_CLI=ON
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /some/prefix
```

The install tree contains the headers, the static library, the command line tool, the licence and
notice files, and the CMake package:

```cmake
find_package(MaintenanceCoordinator CONFIG REQUIRED)
target_link_libraries(my-target PRIVATE SummonDCCP::MaintenanceCoordinator)
```

`tests/downstream` is an independent out-of-tree project that consumes exactly that exported
target and drives a complete maintenance lifecycle against the installed artifact at runtime;
`scripts/validate-packaging.ps1` runs the whole chain — configure, build, test, install, downstream
configure/build/run, CPack source and binary archives — from scratch and reports each step. The
target's interface references only the installed include directory; the installed package never
points back into the source or build tree.

## 15. Validation actually performed

All of the following was run on the development host (Windows 10.0.19045, MSVC 19.44.35222.0,
CMake 4.3.2, Ninja) on the release commit. Nothing here is projected or extrapolated.

**Build configurations.** Debug, Release and AddressSanitizer (`/fsanitize=address`) all configure
and build with **zero first-party warnings**, with `/W4 /WX`, `/permissive-`, `/utf-8`,
`/Zc:__cplusplus` and `/Zc:preprocessor` in force for the library, the tool and the tests.

**Test suites.** Eight `ctest` entries, every case passing in Debug, Release and the sanitizer
build:

| Suite | Cases | Checks | Kind |
| --- | --- | --- | --- |
| `unit_foundation` | 22 | 2968 | SHA-256 known answers and streaming equivalence, CRC-32, canonical codec bounds, error-code table, strict JSON (including generated round trips), identifiers, timestamps |
| `unit_model` | 23 | 3696 | scope/digest canonicalisation, enum round trips, facility validation, plan and record binding rules, obligation tables, codec round trips |
| `unit_engine` | 13 | 128 | lifecycle end to end, fencing, exceptions, replay, recovery, cancellation rules, concurrency conflicts, determinism |
| `unit_adversarial` | 9 | 42 | truncation and mutation of canonical encodings, absurd timestamps, path traversal, long paths, random journals, malformed JSON, authority and generation confusion |
| `unit_property` | 3 | 111 | seeded random command sequences with invariants checked after every step, seed reproducibility, permutation-invariant digests |
| `system_durability` | 11 | 91 | real crashes at four commit points, torn tails, corruption, lock exclusion and release on process death, concurrent readers |
| `system_concurrency` | 4 | 172 | parallel writers, re-entrant sinks, readers during writes, snapshot consistency |
| `system_cli_e2e` | 1 | 194 | a complete lifecycle driven through the command line tool as separate processes, with the printed JSON re-parsed |
| **total** | **86** | **7402** | |

**Installed artifact and downstream consumption.** `scripts/validate-packaging.ps1` was run from a
clean `build/pkg` and reported `PACKAGING VALIDATION OK` with all thirteen steps passing:
configure, build (zero warnings), the full test suite (8/8), install into a prefix, the installed
tree checked file by file, an independent out-of-tree consumer configured against that prefix with
`find_package(MaintenanceCoordinator CONFIG REQUIRED)`, that consumer built and **run** (26 checks
against the installed library, ending in `DOWNSTREAM CONSUMER OK`), the exported target verified as
`SummonDCCP::MaintenanceCoordinator` with `INTERFACE_INCLUDE_DIRECTORIES` pointing only at
`${_IMPORT_PREFIX}/include`, and both CPack archives produced and inspected
(`MaintenanceCoordinator-1.0.0-Source.zip`, 73 entries; `MaintenanceCoordinator-1.0.0-win64.zip`,
29 entries).

**Fresh clone.** After the release commit was pushed, the repository was cloned from the remote
into an empty directory: the clone reported `v1.0.0` and HEAD
`3a970f1e22b6e7eb6737630a18fc51da5a87dd0f`, its working tree was clean, it configured and built
Release with zero warnings, all **8/8** test suites passed from that clone, the install step
produced the prefix, and the out-of-tree consumer built against that prefix ran to
`DOWNSTREAM CONSUMER OK: 26 checks`. Nothing outside the clone was needed to reproduce any of it.

**Defects this work found and fixed** (each one was a real failure observed before it was fixed):

| Defect | Consequence before the fix | Fix |
| --- | --- | --- |
| The journal was reopened with ordinary write access | a second writing session overwrote every earlier transaction from offset zero; recovery then failed with `RecordCorrupt` | open the journal with `FILE_APPEND_DATA`, plus a regression test that writes in three sessions and reads all of them back |
| The installed package config called `check_required_components` without defining it | every `find_package(MaintenanceCoordinator)` aborted with `Unknown CMake command` | stop suppressing the macro |
| The exported target was named after the internal target | a consumer following the documentation could not link `SummonDCCP::MaintenanceCoordinator` | `EXPORT_NAME MaintenanceCoordinator` |
| The installed include directory was listed twice | a duplicated exported interface | drop the redundant `INCLUDES DESTINATION` |
| The commit sequence was assigned after the payload was encoded | a plan replayed from the journal lost its commit sequence | stamp the sequence before encoding; the in-memory record is byte-identical to the durable one |
| `format_timestamp` truncated a negative instant toward zero | an instant before 1970 rendered one day late and did not parse back to itself | floor division, with round-trip tests across six negative instants |
| `kNoTimestamp` was zero | the Unix epoch was indistinguishable from "no timestamp" | the sentinel is the smallest representable instant |
| `parse_timestamp` accepted impossible dates and leap seconds | `2026-02-30` silently normalised to March | month-aware day validation, leap years included, seconds bounded to 59 |
| Scope membership of site-level plant objects ignored racks | a rack-scoped plan saw no power domain, cooling zone or fabric segment, so headroom was never evaluated | resolve plant objects through the sites the scope touches |
| A satisfied power/cooling condition still said "below the policy floor" | the report contradicted itself | the detail text follows the outcome |
| Directory creation used the legacy path form | a store beyond the path limit could not be created | create each component through the extended-length prefix |
| `compact` and a no-op `recover` returned a zero outcome digest | an outcome that named nothing | both now bind the command, the sequence and every plan digest |
| An unsatisfied guard let a nested `Result` be unwrapped unchecked | a rejected identity could have thrown instead of returning a status | explicit presence checks, everywhere |
| The test macro held a reference into a temporary `Status` | a dangling read, visible only under AddressSanitizer | the macro copies the status, and the sanitizer suite is now clean |

**Durability and multiprocess.** Real child processes: a writer killed with `TerminateProcess`
after its mutation record leaves no trace after restart; killed after its commit record leaves
exactly the committed state; a snapshot staged but not published is ignored; a snapshot published
without truncating the journal is recovered exactly once; a torn journal tail is truncated and its
transaction discarded; a corrupted complete record fails closed with `ChecksumMismatch`; a second
writer is refused with `LockHeld` while the first holds the lock, in another process and in the
same process; killing the lock holder releases the lock; a reader process reading continuously
while a writer commits 120 transactions never observes a torn generation.

**Adversarial.** Every truncation of a valid encoding is rejected; single-byte mutations are either
rejected or canonicalised consistently (decoding is idempotent); absurd windows are rejected rather
than wrapped; a store path containing a parent reference is refused; a store path beyond the legacy
path limit works through the extended-length prefix; random bytes in the journal never decode into
a ledger; malformed JSON is rejected with a precise code.

**Sanitizers.** The complete suite — including the crash and multiprocess tests — passes under
AddressSanitizer with no reports. The sanitizer run is what found the one lifetime defect in the
test harness itself (a macro that held a reference into a temporary `Status`); the macro now copies.

## 16. Benchmarks

Measured with `maintenance-coordinator-bench` (build with `-DMC_BUILD_BENCHMARK=ON`), Release,
single thread, **one measurement per row on one host**:

| Operation | Count | Seconds | Per second | Durable |
| --- | --- | --- | --- | --- |
| `propose` — one commit and flush each | 200 | 0.2634 | 759 | yes |
| ledger snapshot — in-process read | 20000 | 1.3389 | 14 938 | no |
| `explain` — re-derive every obligation | 2000 | 0.0144 | 138 847 | no |
| `compact` — publish a verified snapshot | 1 | 0.0229 | — | yes |
| open + recover — 200-plan ledger | 5 | 0.0240 | 208 | no |
| `propose` with the flush disabled (comparison only) | 200 | 0.0103 | 19 421 | — |

**Methodology.** Wall-clock time around completed calls (`std::chrono::steady_clock`), counted per
operation; nothing is enqueued, so nothing here is submission latency. Durable operations are
measured with the flush enabled, and the flush-disabled row exists only to show where the durable
cost sits: at roughly 1.3 ms per commit, the commit point and its flush dominate the cost of a
mutation by about twenty-five times on this host. The facility model is **SYNTHETIC** (four assets
on two racks); the store, journal, snapshot, flush, lock and recovery are **REAL**. These are not
statistically controlled numbers, they were not taken before and after any change, and they are not
a claim about any other host or filesystem.

## 17. Limitations and non-goals

* The durable store targets Windows; see the platform boundary above.
* The facility model is supplied, never discovered: this repository does not poll the plant, and an
  unmeasured value blocks rather than defaults.
* There is no HTTP or RPC surface: the process boundary is the command line tool and the C++ API.
* Risk is classified and explained, but the tenant-facing risk model is not owned here.
* `SYNTHETIC`: no physical data-centre hardware was available, so no plant, sensor, power or
  cooling behaviour is claimed. Every process, file, lock, crash and recovery claim above is
  `REAL` and was executed on the host.

## 18. Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the build, test and style expectations a change is
expected to satisfy.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

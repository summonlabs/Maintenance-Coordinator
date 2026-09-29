# Contributing to Maintenance Coordinator

Thanks for your interest in Maintenance Coordinator, a C++20 library for
facility-level maintenance window coordination, maintained by Summon Software
Labs.

This document describes how to report issues, how to propose changes, and the
build, test, style, and commit expectations a change must satisfy before it is
accepted. Contributions are accepted under the Apache License, Version 2.0, and
no Contributor License Agreement (CLA) or copyright assignment is required. See
[Licensing](#licensing) below.

## Reporting issues

- Search the existing issues first; if an equivalent report exists, add your
  details to it instead of opening a duplicate.
- Report a bug with enough information to reproduce it:
  - the commit or version you are building, and the target platform;
  - compiler and version, and the CMake generator you used;
  - the exact configure and build commands, including any `-D` options;
  - the expected result and the actual result, with the full error or warning
    text;
  - the smallest snippet, input, or test case that shows the problem.
- Keep reports neutral and factual. Describe behavior and evidence, not
  individuals.
- Security-sensitive reports (memory-safety bugs, unsafe parsing of untrusted
  input, credential exposure) should not be filed as a public issue with
  exploit details. Use the repository's private vulnerability reporting channel
  when it is available; otherwise open a minimal issue that describes the class
  of problem and ask for a private channel.

## Proposing changes

- For anything larger than a small fix, open an issue or discussion first so
  the approach can be agreed before you invest in the implementation.
- Work from a fork on a topic branch, and open a pull request against the
  default branch.
- Keep a pull request focused on one logical change. Unrelated refactoring,
  reformatting, and behavior changes belong in separate pull requests.
- In the description, state what the change does, why it is needed, how it was
  tested, and any alternatives you considered or rejected.
- Call out user-visible behavior changes, public header or API changes, and
  anything that could affect existing callers.

## Build and test expectations

A change is not accepted until it configures, builds, and tests cleanly. The
project is C++20 (no compiler extensions) and is configured with CMake 3.20 or
later.

Configure and build a Release tree:

```
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --config Release
```

Configure and build a Debug tree:

```
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --config Debug
```

Run the full test suite in both trees:

```
ctest --test-dir build/release --output-on-failure -C Release
ctest --test-dir build/debug   --output-on-failure -C Debug
```

The `-C <config>` argument is required for multi-configuration generators such
as Visual Studio and is harmless elsewhere.

Warning cleanliness is a hard requirement:

- MSVC: `/W4` with `/WX`.
- GCC and Clang: `-Wall -Wextra -Wpedantic -Werror`, plus the additional
  warnings the project enables for first-party targets (shadowing, conversions,
  old-style casts, format checking, and similar).

Both Release and Debug builds must be warning-clean. Do not land a change by
disabling warnings, dropping `MC_WARNINGS_AS_ERRORS`, or widening a suppression
to the whole project. Fix the underlying cause. If a warning is genuinely
incorrect for a specific construct, restrict the suppression to the smallest
possible scope and add a comment explaining why it is safe.

Tests are proof obligations for behavior changes:

- Every behavior change, and every bug fix, needs at least one test that fails
  before the change and passes after it.
- Prefer assertions on observable behavior over assertions on implementation
  details.
- Tests must be deterministic: no network access, no dependence on wall-clock
  time, the local time zone, the locale, environment variables, or absolute
  machine-specific paths, and no sleeping to paper over a timing race. Seed any
  random number generator explicitly.
- Do not weaken or delete an existing test to make a change pass. If a test
  encodes the wrong expectation, say so in the pull request and explain why.
- `-DMC_ENABLE_SANITIZERS=ON` (GCC and Clang only) is encouraged for changes
  that touch memory ownership, lifetimes, or concurrency.

Dependency and runtime discipline:

- Keep the library dependency-light. The standard library comes first; add a
  third-party dependency only when it is clearly justified, and explain the
  justification, the license compatibility with Apache-2.0, and the maintenance
  cost in the pull request.
- Keep behavior deterministic: the same inputs must produce the same outputs,
  independent of container iteration order, platform, or ambient state.
- No telemetry. The library must not phone home, collect analytics, or perform
  network I/O at runtime. Any networked feature must be explicit, documented,
  and off by default.

## Code style

- C++20, standard library first. Do not rely on compiler extensions; the build
  sets `CMAKE_CXX_EXTENSIONS OFF`.
- Match the surrounding code. Consistency with the existing files counts for
  more than personal preference.
- Make ownership explicit. Use RAII, and let smart pointers express owning
  relationships; raw pointers and references are non-owning observers. Avoid
  naked `new` and `delete`.
- Use strong types for identities and generations. Do not pass bare integers
  where a distinct identity or generation type exists; strong types keep
  different kinds of identifiers from being mixed up, and they make generation
  checks intentional rather than accidental. Keep the existing headers under
  `include/mc` as the home for these types and extend them rather than
  introducing parallel conventions.
- Prefer `const` correctness, `[[nodiscard]]` on queries that return a value
  the caller must consider, and `noexcept` where it is actually true.
- Avoid undefined behavior, unchecked narrowing conversions, and
  `reinterpret_cast` without a comment justifying it. The enabled conversion
  and cast warnings exist to catch these early.
- Do not hard-code machine-specific absolute paths in sources, tests, CMake
  files, or documentation. Use relative paths, CMake variables such as
  `CMAKE_CURRENT_SOURCE_DIR`, and standard filesystem facilities.
- Never commit credentials, tokens, keys, certificates, or personal data -
  not in code, tests, fixtures, sample configuration, or documentation. Treat
  anything committed as public forever.
- Do not commit generated junk: build trees, binaries, object files, caches,
  IDE project files, editor backups, or large generated artifacts belong
  outside the repository.
- Comment the why, not the what. Delete commented-out code rather than leaving
  it behind.

## Commit messages

- Write a concise, neutral, imperative subject line: "Add bounds check to
  digest parsing", not "Added ...", "Adding ...", or "Fixes stuff".
- Keep the subject near 72 characters, without a trailing period.
- Follow the subject with a blank line and a body that explains what changed
  and why, when the reason is not obvious from the diff.
- Do not include AI attribution of any kind. No "generated by", no assistant or
  tool attribution, and no `Co-authored-by` trailers. Authorship must reflect
  the humans who did the work.
- `Signed-off-by` is not required; this project does not use a DCO.
- Reference issues in the body (for example, "Refs #123"), not in the subject.
- Keep each commit to one logical change where practical, so the history stays
  reviewable and bisectable.

## Licensing

By submitting a contribution - a pull request, a patch, or any other form - you
agree that your contribution is licensed under the Apache License, Version 2.0,
with no additional terms or conditions. This is an inbound-equals-outbound
model: what comes in under Apache-2.0 goes out under Apache-2.0.

- No CLA is required. You do not sign anything, and you do not assign your
  copyright. You keep the copyright in your contribution.
- You must have the right to submit the work. It must be your own original
  creation, or you must have explicit permission from whoever holds the rights
  to license it under Apache-2.0.
- Do not submit code, fixtures, or documentation copied from a source whose
  license is incompatible with Apache-2.0. If a contribution includes
  third-party material, disclose it in the pull request along with its license
  and the reason it is compatible.
- New source files should carry an `SPDX-License-Identifier: Apache-2.0`
  line. The full license text lives in `LICENSE`, and attribution notices live
  in `NOTICE`; do not edit `NOTICE` to add personal attribution.
- Contributions are provided "AS IS", without warranties or conditions of any
  kind, as set out in the license.

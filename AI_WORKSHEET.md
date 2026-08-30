# CODEC AI Worksheet

Canonical work loop for ChatGPT, Codex, and other agentic contributors.

**Purpose:** complete one bounded unit of work without overstating capability, weakening CODA truth semantics, or coupling CODEC core to one profile.

> Read `README.md` first. Read deeper design docs only when the task touches their subject. Do not reread large historical plans unless they are directly relevant.

## Active work record — Remove watermark and generic-stream CLI surfaces

```yaml
task: Remove the complete watermark feature and the generic stream list/extract CLI surfaces, make source-exact the documented feed-extraction default, and retain generic stream library infrastructure and all unrelated CLI behavior.
base_ref: origin/main
base_head_sha: a73b15c474b6974bf74b2c622b81bdcafee7890c
work_branch: codex/remove-watermark-stream-cli
current_version: 0.3.0
active_roadmap_stage: F — F.1-F.7 remain implemented C++ library/package primitives; this user-directed removal retires the Audio Profile watermark capability and generic stream CLI exposure without removing the generic stream substrate.
continuity_evidence:
  - git_head: clean main at a73b15c474b6974bf74b2c622b81bdcafee7890c before the work branch was created
  - open_prs: none returned by the GitHub repository query at task start
  - exact_head_ci: no pull-request-triggered workflow runs returned for the exact base head
  - roadmap_issue: issue 10 is the unique exact-title CODEC v1.0 roadmap execution log; runtime code and tests remain authoritative
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: [watermark, core, docs]
touched_truth_classes: []
current_behavior_verified_from: [code, tests, cli, cmake, changelog]
new_capability_claim: none; this change removes capabilities and CLI surfaces
change_class: profile_specific_behavior
verification:
  tested_pre_record_sha: 069a7e5bf6e486ec41f8dd7ee7c96ee10f47fb23
  release_configure: fail
  release_build: fail
  tests: fail
  sanitizer_build: fail
  sanitizer_tests: fail
  installed_package_consumers: fail
  cli_capabilities: fail
  targeted_proof: fail
  blockers:
    release_configure: "cmake is unavailable on PATH (command -v cmake produced no path)."
    release_build: "Not run: Release configuration cannot be created because cmake is unavailable on PATH."
    tests: "Not run: no Release build exists because cmake is unavailable on PATH; ctest is also unavailable on PATH (command -v ctest produced no path)."
    sanitizer_build: "Not run: sanitizer configuration cannot be created because cmake is unavailable on PATH."
    sanitizer_tests: "Not run: no sanitizer build exists because cmake is unavailable on PATH; ctest is also unavailable on PATH."
    installed_package_consumers: "Not run: an exact-head install cannot be created because cmake is unavailable on PATH."
    cli_capabilities: "Not run: no source-exact ./build/codec was produced; stale or non-exact binaries were not used."
    targeted_proof: "Not run: no source-exact ./build/codec was produced; stale or non-exact binaries were not used."
```

```text
BEFORE: CODEC exposes watermark CLI/library behavior plus generic `list streams` and `extract --stream` CLI selection.
AFTER: Watermark implementation and public surfaces are absent; the CLI lists and extracts only by feed with source-exact fidelity defaulted, while generic stream C++ APIs and unrelated CLI behavior remain available.
```

```yaml
proof:
  regression_test: tests/cli_integration.sh proves removed commands fail and regular/follow feed extraction succeeds without --fidelity
  exactness_test: existing byte-exact feed extraction and live-follow tests remain green
  compatibility_test: retired archive record codes remain preservable as unknown raw codes; non-watermark distributed wire error numbers remain stable
  failure_path_test: watermark, list streams, and extract --stream return status 2 without creating output
  security_test: removed signing/key paths leave no callable watermark credential surface; existing capture security tests remain green
  benchmark: n/a — no performance or scale claim
```

Invariant decisions:

- [x] Accepted S0, profile-defined S1, D provenance, archive envelopes, and generic stream library semantics are unchanged.
- [x] Generic `Stream*`, query, extraction, follow, processing, transport, recovery, and distributed primitives remain public C++ capabilities.
- [x] Retired archive codes 20 and 21 remain readable, verifiable, repairable, and raw-extractable as unknown compatible record types.
- [x] Retired distributed error slots 10-14 are not reassigned; every retained error preserves its existing wire number.
- [x] `record`, `verify`, `inspect`, `repair`, `list feeds`, and `extract --feed` behavior remains unchanged apart from truthful capability/help documentation.
- [x] Historical release notes remain historical truth; current manifests receive explicit removal language.

## 0. Work record

Fill this before editing:

```yaml
task: <one-sentence requested outcome>
base_ref: <branch/commit>
base_head_sha: <exact SHA verified at start>
work_branch: <branch>
current_version: <from CMakeLists.txt>
active_roadmap_stage: <current README roadmap item and unmet exit evidence>
continuity_evidence: [git_head, open_prs, exact_head_ci, roadmap_issue] # mark unavailable sources
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: core | archive | capture | audio-profile | inference | watermark | recovery | distributed | trust | other-profile | docs
touched_truth_classes: [S0, S1, D]   # remove unused
current_behavior_verified_from: [code, tests, cli, changelog]
new_capability_claim: none | <exact claim>
```

## 0.5. Cold-start state recovery

Never assume a previous chat, local branch name, or scheduled task describes the current repository state.

- [ ] Record the exact checked-out HEAD and confirm the intended base branch is current before editing.
- [ ] Inspect the working tree and preserve unrelated changes.
- [ ] When GitHub access is available, inspect open pull requests, their head SHAs, reviews, and CI; do not duplicate or overwrite active work.
- [ ] Search open and closed issues for the exact title `CODEC v1.0 roadmap execution log`. If exactly one exists, read it and compare it with code/tests rather than treating it as runtime truth; if none or multiple exist, record that state instead of guessing.
- [ ] Select the active roadmap stage from merged implementation evidence and identify its next unmet exit gate. Dates alone never advance a stage.
- [ ] Record any unavailable continuity source and proceed only from evidence you can verify.
- [ ] Re-read state and reverify if the base or PR head moves before merge.

## 1. Read the minimum necessary context

- [ ] `README.md` manifest and invariants.
- [ ] `CMakeLists.txt` for version, targets, dependencies, test surface.
- [ ] Relevant public headers in `include/`.
- [ ] Relevant implementation under `src/`.
- [ ] Relevant tests under `tests/`.
- [ ] `CHANGELOG.md` when changing released/user-visible behavior.
- [ ] Stream-first design spec only if the task affects architecture/core-profile boundaries.

Record the existing behavior in one sentence before changing it:

```text
BEFORE: <observable current behavior and evidence>
AFTER:  <observable intended behavior>
```

## 2. Classify the change

Choose exactly one primary class:

```yaml
change_class:
  - preservation_invariant
  - archive_format_or_compatibility
  - generic_stream_abstraction
  - profile_specific_behavior
  - inference_or_derived_output
  - security_or_authorization
  - performance_or_scale
  - documentation_only
```

Then answer:

- [ ] Is this generic CODEC/CODA behavior or profile-specific behavior?
- [ ] If profile-specific, is every profile-only field/assumption kept out of generic core structures?
- [ ] Does the change alter S0, S1, or D semantics? If yes, stop and validate against the architecture spec before implementation.
- [ ] Does it create a new capability claim? If yes, define the exact test that proves the claim.

## 3. Non-negotiable invariants

Every implementation MUST satisfy all applicable items:

- [ ] S0 remains the exact accepted source representation.
- [ ] S1 is used only for a deterministic, profile-defined exact canonical state.
- [ ] D remains visibly derived and provenance-bearing.
- [ ] No D artifact silently replaces S0/S1.
- [ ] Committed preservation does not depend on optional inference/profile success.
- [ ] Logical stream identity is not bound to a port, file, worker, connection, or archive segment.
- [ ] Unknown compatible payload types remain preservable/extractable.
- [ ] Generic records do not gain profile-only semantics without a universal reason.
- [ ] Unavailable functionality returns explicit unavailability; no fabricated output.
- [ ] Scale/performance claims include workload and hardware evidence.

If any item cannot be satisfied, do not work around it silently; revise the design or narrow the claim.

## 4. Test contract before implementation

Define proof first.

```yaml
proof:
  regression_test: <test/file or none for docs-only>
  exactness_test: <required for new S0/S1 claim or n/a>
  compatibility_test: <required for archive/API compatibility change or n/a>
  failure_path_test: <expected failure/unavailability path or n/a>
  security_test: <boundary test or n/a>
  benchmark: <required for performance/scale claim or n/a>
```

For behavior changes:

- [ ] Add/identify a test that fails for the old behavior and proves the requested behavior.
- [ ] Verify corruption/truncation/unknown-type behavior when archive semantics change.
- [ ] Verify overload/unavailable-model behavior when inference/profile execution changes.
- [ ] Verify C ABI/API compatibility when public interfaces change.

For documentation-only changes:

- [ ] Cross-check every current-status claim against code/CMake/tests.
- [ ] Mark every aspirational capability as planned/not implemented.
- [ ] Avoid duplicating large specifications already linked elsewhere.

## 5. Implementation loop

Use the smallest coherent change that satisfies the proof contract.

1. Change tests/proof first when behavior is modified.
2. Implement the minimum generic primitive or profile-scoped behavior.
3. Keep preservation and inference/profile processing separable.
4. Update public API/CLI/docs only when implementation requires it.
5. Update status/CHANGELOG when a capability actually changes.
6. Re-read the diff for accidental scope expansion.

Prefer:

```text
generic Stream primitive
  -> profile extension
  -> tested implementation
```

over:

```text
profile convenience
  -> audio/media assumption in core
  -> later architectural repair
```

## 6. Mandatory local/CI-equivalent verification

Release configuration:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Sanitizers:

```bash
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel
ctest --test-dir build-san --output-on-failure
```

Capability sanity check when CLI behavior is relevant:

```bash
./build/codec capabilities
```

Verification record:

```yaml
verification:
  release_configure: pass | fail | n/a
  release_build: pass | fail | n/a
  tests: pass | fail | n/a
  sanitizer_build: pass | fail | n/a
  sanitizer_tests: pass | fail | n/a
  cli_capabilities: pass | fail | n/a
  targeted_proof: pass | fail | n/a
```

Never report success from an older run or a different commit.

## 7. Claim audit

Before opening/merging a PR, classify every changed capability statement:

| Claim type | Allowed wording |
|---|---|
| Proven by current code/tests | `implemented`, with exact scope |
| Interface exists, backend absent | `interface available; implementation/model unavailable` |
| Designed but not coded | `planned`, `architectural direction`, or `not implemented` |
| Probabilistic inference | confidence/quality + provenance; never deterministic source truth |
| Cryptographic signature | authenticates the signed statement only |
| Recovery | specify encoded-data recovery vs pre-lossy reconstruction separately |
| Scale | state measured workload/hardware; never infer from identifier/port capacity |

- [ ] README manifest still matches runtime truth.
- [ ] `CHANGELOG.md` matches user-visible implementation changes.
- [ ] No comment/docs claim exceeds test evidence.

## 8. Diff audit

Before PR creation:

- [ ] Only intended files changed.
- [ ] No credentials, keys, generated artifacts, build outputs, or temporary automation files.
- [ ] No unrelated refactor.
- [ ] Generic/profile boundary remains clean.
- [ ] Public compatibility changes are explicit and versioned where required.
- [ ] No `TODO`/placeholder substitutes for required behavior.

Summarize the diff in at most five bullets.

## 9. Merge gate

A branch is merge-ready only when all applicable conditions are true:

```yaml
merge_gate:
  requested_behavior_proven: true
  invariants_preserved: true
  release_ci_equivalent_green: true
  sanitizer_ci_equivalent_green: true
  docs_and_status_truthful: true
  diff_scoped: true
  pr_mergeable: true
  ci_green_on_exact_head_sha: true
```

**Automatic merge rule:** merge only the exact head SHA whose required CI completed successfully. If the head moves, re-verify. If any gate is false/unknown, do not merge.

## 10. Completion report

Use this compact format:

```yaml
result:
  outcome: <what changed>
  files: [<paths>]
  proof: <tests/CI evidence>
  capability_delta: <newly implemented behavior or none>
  remaining_planned: <relevant unimplemented items>
  merge_commit: <sha or not-merged>
```

Do not repeat design history. Report facts from the final verified tree.

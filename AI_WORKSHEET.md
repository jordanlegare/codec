# CODEC AI Worksheet

Canonical work loop for ChatGPT, Codex, and other agentic contributors.

**Purpose:** complete one bounded unit of work without overstating capability, weakening CODA truth semantics, or coupling CODEC core to one profile.

> Read `README.md` first. Read deeper design docs only when the task touches their subject. Do not reread large historical plans unless they are directly relevant.

## Active work record — Preservation-first HLS video ingest

```yaml
task: Extend the H.1 FFmpeg Video Profile bridge with bounded same-origin unencrypted HTTP/HTTPS HLS capture and versioned source-frontier provenance.
base_ref: main
base_head_sha: f68515068a022ef4f16eefdc1df0512b94bcec77
work_branch: codex/video-hls-ingest
current_version: 0.3.0
active_roadmap_stage: H — H.1 Video Stream Profile integration follow-on; H.2 telemetry remains paused on its separate branch and Stage G remains deferred.
continuity_evidence:
  - git_head: GitHub main at f68515068a022ef4f16eefdc1df0512b94bcec77 when the HLS branch was created
  - open_prs: draft PR 50 is the active HLS pull request
  - exact_head_ci: Task 3 lifecycle fix GREEN in run 375 at 1bc0e6363a5475b1e958b3893739cc1d74a4ff5d; Task 4 reader RED in run 377, reader GREEN in run 378, actual-ingest RED in run 379, and complete Task 4 GREEN in run 380 at cae82fea0b14a848ef9716322511342a70a1c436; Task 5 CLI RED in run 381; final documentation/evidence head still requires exact-head CI
  - roadmap_issue: issue 10 is the unique exact-title CODEC v1.0 roadmap execution log; runtime code and tests remain authoritative
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: [other-profile, docs]
touched_truth_classes: [S0, S1]
current_behavior_verified_from: [code, tests, cli, cmake, changelog, HLS design]
new_capability_claim: With the FFmpeg backend enabled, CODEC can ingest bounded same-origin unencrypted HTTP/HTTPS HLS while preserving the primary manifest and each accepted child snapshot as exact S0 before read, then emit canonical VFR1 S1 under exact versioned source-frontier provenance.
change_class: profile_specific_behavior
verification:
  task3_exact_head: pass — CI run 375 at 1bc0e6363a5475b1e958b3893739cc1d74a4ff5d passed GCC, Clang, sanitizers/LSan, FFmpeg-disabled, install, and package consumers after the callback-open lifecycle fix
  task4_tdd: pass — run 377 failed only on valid HLS frontier rejection after fixture correction; run 379 failed only on the legacy direct process emitted by actual HLS ingest
  task4_green: pass — CI run 380 at cae82fea0b14a848ef9716322511342a70a1c436 passed GCC, Clang, sanitizers, FFmpeg-disabled, install, and package consumers
  task5_cli_red: pass — CI run 381 at 432d5b2f615b40f0283d457f0411cbcd7d7395b8 failed at the new CLI contract before parser/output implementation
  local_compile: pass for dependency-free reader/tests, CLI, and installed consumer with GCC warnings-as-errors; FFmpeg development headers are unavailable locally, so enabled production proof is authoritative in GitHub CI
  final_exact_head_ci: pending — required after CLI/package/docs commit and before merge
```

```text
BEFORE: The FFmpeg bridge can preserve and decode one self-contained source object, but HLS secondary opens are denied and the verified reader accepts only same-stream direct provenance.
AFTER: Same-origin unencrypted HTTP/HTTPS HLS is captured through CODEC-owned secondary AVIO, each accepted object is exact S0 on its own stream, live decode is bounded, and each canonical HLS VFR1 S1 validates against its ordered versioned source frontier; the direct-media contract remains unchanged.
```

```yaml
proof:
  regression_test: tests/test_video_hls_ingest.cpp proves exact manifest/segment preservation, HLS process identity, ordered source frontiers, and public verified-reader retrieval; tests/test_video_state_reader.cpp proves manual valid/malformed HLS lineage while retaining direct rules
  exactness_test: the primary manifest and committed TS fixtures are extracted byte-for-byte, child payload hashes/lengths match, and emitted VFR1 remains canonical H.1 state
  compatibility_test: direct MP4/BMP provenance and nested-open denial remain unchanged; FFmpeg-disabled, installed-package, CLI, C ABI, audio, archive, transport, recovery, distributed, and generic unknown-record tests remain green
  failure_path_test: encrypted/cross-origin/file/crypto/private-denied/malformed children, per-resource/aggregate/count exhaustion, callback lifecycle failures, and live-duration termination preserve every accepted S0 prefix and write no partial S1 on profile failure
  security_test: FFmpeg never receives native network/file protocol authority; each HLS child is independently same-origin authorized, captured, bounded, archived as S0, and only then exposed through read-only memory AVIO
  benchmark: n/a — no performance, latency, codec coverage, or scale claim is made
```

Invariant decisions:

- [x] The primary manifest and every accepted HLS child are distinct exact S0 objects committed before FFmpeg reads the child; generic source extraction is never fabricated by concatenation.
- [x] Direct media retains the exact `codec.video.raw-frame.canonicalize` same-stream contract; HLS alone uses `codec.video.raw-frame.canonicalize.hls` with the exact version byte and ordered parent/opaque-child frontier.
- [x] FFmpeg has no native HTTP/HTTPS/file/crypto/data/concat fallback; HLS expansion is limited to CODEC-authorized same-origin HTTP/HTTPS capture.
- [x] Encrypted and cross-origin HLS, DASH, browser-session behavior, playback, export/transcoding, GPU/model/inference, quality, performance, and scale remain explicit non-capabilities.
- [x] Live HLS duration is a decoded-media-timeline bound, not a wall-clock recording guarantee; resource count/bytes remain independent bounds.
- [x] Generic archive/stream/capture authorization, CLI `record`, C ABI, audio, transport, distributed, telemetry, and Stage G semantics are unchanged.
- [x] FFmpeg-disabled builds retain the media-library-independent H.1 schema/reader and explicit backend-unavailable behavior.

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

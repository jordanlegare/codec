# CODEC AI Worksheet

Canonical work loop for ChatGPT, Codex, and other agentic contributors.

**Purpose:** complete one bounded unit of work without overstating capability, weakening CODA truth semantics, or coupling CODEC core to one profile.

> Read `README.md` first. Read deeper design docs only when the task touches their subject. Do not reread large historical plans unless they are directly relevant.

## Active work record — User-directed H.1 FFmpeg video ingest follow-on

```yaml
task: Add an optional FFmpeg-backed Video Profile ingest bridge that preserves encoded/container bytes as S0 and emits canonical provenance-linked VFR1 S1 frames.
base_ref: main
base_head_sha: 226e7d099a3ebaf8fc12b38a8464881ed7608b04
work_branch: codex/video-ffmpeg-ingest
current_version: 0.3.0
active_roadmap_stage: H — H.1 Video Stream Profile foundation is merged; this user-directed video integration follow-on does not claim H.2-H.6 complete and leaves Stage G deferred.
continuity_evidence:
  - git_head: GitHub main at 226e7d099a3ebaf8fc12b38a8464881ed7608b04 when the branch was created
  - open_prs: none at task start
  - exact_head_ci: H.1 merge head is repository truth; this branch requires its own RED/GREEN/final exact-head evidence
  - roadmap_issue: issue 10 is the unique exact-title CODEC v1.0 roadmap execution log; runtime code and tests remain authoritative
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: [other-profile, docs]
touched_truth_classes: [S0, S1]
current_behavior_verified_from: [code, tests, cmake, changelog, H.1 design]
new_capability_claim: When explicitly built with supported FFmpeg development libraries, CODEC can ingest one encoded video source into a CODA archive, preserve the exact accepted source bytes as S0, decode bounded video frames to one H.1 canonical pixel layout, and attach exact same-stream S0 provenance to each VFR1 S1 frame.
change_class: profile_specific_behavior
verification:
  tdd_red_run: pending
  release_configure: pending
  release_build: pending
  tests: pending
  sanitizer_build: pending
  sanitizer_tests: pending
  package_consumer: pending
  final_exact_head_ci: pending
```

```text
BEFORE: H.1 can encode/decode/query canonical raw video frames supplied by callers, but it has no container demuxer or encoded-video decoder integration.
AFTER: An optional FFmpeg-backed profile integration can capture a bounded encoded source, preserve those bytes as S0 first, decode the first selected video stream into canonical H.1 VFR1 frames, and bind every S1 frame to the exact committed S0 record; builds without FFmpeg remain supported and the generic core is unchanged.
```

```yaml
proof:
  regression_test: tests/test_video_ffmpeg_ingest.cpp proves optional backend availability, deterministic fixture ingest, source preservation, canonical frame decode, frame timing, and verified-reader retrieval
  exactness_test: extracted S0 bytes equal the input media fixture byte-for-byte; emitted VFR1 payloads decode to exact bounded canonical pixels and re-encode identically
  compatibility_test: existing H.1 VPD1/VFR1, CLI, C ABI, audio, archive, transport, recovery, and distributed tests remain green; no generic RecordType or CODA envelope changes
  failure_path_test: backend-unavailable build, invalid request, no video stream, malformed media, unsupported/oversized dimensions, decode failure, and output-path conflicts fail explicitly; preservation-first ingest finalizes source-only archive when decoding fails after S0 commit
  security_test: FFmpeg input is decoded only from already captured bounded in-memory S0 bytes; FFmpeg is not allowed to open arbitrary nested URLs/protocols on CODEC's behalf
  benchmark: n/a — no performance, latency, codec coverage, or scale claim
```

Invariant decisions:

- [x] Accepted encoded/container/source bytes remain byte-exact S0 and are committed before optional decode interpretation.
- [x] VFR1 remains S1 only under the existing H.1 canonical frame contract and exact same-stream provenance.
- [x] FFmpeg integration is profile-owned and compile-time optional; no media-specific field or dependency enters generic CODEC/CODA structures.
- [x] FFmpeg demux/decode consumes the captured in-memory bytes through a custom AVIO reader rather than reopening the source URI, preventing nested protocol/network access during interpretation.
- [x] Initial canonical decode target is YUV420P8 when conversion is required; direct supported layouts may be copied only when their exact byte layout matches the H.1 canonical representation.
- [x] Audio/subtitle/data streams are ignored by this integration; playback, transcoding/export, streaming inference, models, and quality/performance claims remain out of scope.
- [x] Builds without FFmpeg remain valid and expose explicit backend unavailability rather than silently changing H.1 behavior.
- [x] Stage G remains deferred and H.2-H.6 are not claimed complete by this user-directed follow-on.

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

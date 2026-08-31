# CODEC AI Worksheet

Canonical work loop for ChatGPT, Codex, and other agentic contributors.

**Purpose:** complete one bounded unit of work without overstating capability, weakening CODA truth semantics, or coupling CODEC core to one profile.

> Read `README.md` first. Read deeper design docs only when the task touches their subject. Do not reread large historical plans unless they are directly relevant.

## Active work record — H.1 encoded-audio preservation

```yaml
task: Replace new H.1 PCM16 audio-state writes with a bounded versioned bundle of original compressed audio packets while retaining legacy PCM16 reads and exports.
base_ref: main
base_head_sha: bbc286348d0a78474a4569588b91743300483c16
work_branch: codex/encoded-audio-state
current_version: 0.3.0
active_roadmap_stage: H — user-directed H.1 storage/runtime correction before H.2 telemetry; Stage G remains deferred.
continuity_evidence:
  - git_head: GitHub main and origin/main at bbc286348d0a78474a4569588b91743300483c16 when this isolated worktree was created
  - open_prs: none returned by the GitHub plugin at task start; draft PR 56 now carries codex/encoded-audio-state
  - exact_head_ci: no combined statuses or pull-request workflow runs are attached to merge commit bbc286348d0a78474a4569588b91743300483c16; exact branch-head CI is required before merge
  - roadmap_issue: issue 10 is the unique exact-title CODEC v1.0 roadmap execution log; its merged HLS and video-export evidence is consistent with current code, while runtime code and tests remain authoritative
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: [other-profile, docs]
touched_truth_classes: [S1]
current_behavior_verified_from: [code, tests, cli, cmake, changelog, H.1 audio design]
new_capability_claim: New H.1 audiovisual ingest preserves a bounded, versioned, provenance-verified bundle of original compressed audio packets and compatible MP4 export remuxes those packets without PCM16 persistence or AAC re-encoding; legacy 0x0102 PCM16 archives remain readable and exportable.
change_class: profile_specific_behavior
verification:
  release_configure: pass — GitHub Actions run 476, exact implementation head d77bcaa4d9ebba46474374a473878fc117d319df, GCC and Clang
  release_build: pass — run 476 build jobs for GCC and Clang
  tests: pass — run 476 GCC/Clang CTest, including direct/HLS ingest, export, CLI, and legacy compatibility
  sanitizer_build: pass — run 476 ASan/UBSan/LSan build
  sanitizer_tests: pass — run 476 sanitizer CTest
  ffmpeg_disabled: pass — run 476 configure/build/CTest/install with CODEC_ENABLE_FFMPEG_VIDEO=OFF
  install_and_package_consumer: pass — run 476 GCC, Clang, and FFmpeg-disabled installed-consumer gates
  exact_head_ci: required on the final documentation commit and recorded by immutable PR 56 checks before merge
```

```text
BEFORE: H.1 stores source media as exact S0, then decodes, resamples, and accumulates the audio again as a large 0x0102 PCM16 S1 record before AAC-encoding it during MP4 export.
AFTER: New H.1 writes store the selected stream's unchanged compressed packet payloads and codec/timeline metadata as bounded 0x0103 S1, validate audio in streaming decode without accumulating PCM, and remux compatible packets during export; old 0x0102 archives retain their existing reader/export path.
```

```yaml
proof:
  regression_test: new encoded-state schema/reader tests plus direct/HLS ingest and MP4 export tests prove 0x0103 writes, no new 0x0102 writes, and audiovisual output
  exactness_test: packet payloads, codec parameters/extradata, timestamps, durations, flags, and presentation window round-trip byte-for-byte through encode/decode and verified ingest/query
  compatibility_test: existing 0x0102 reader/export tests remain green; old/no-audio archives remain exportable; standalone Audio Profile and generic CODA APIs are unchanged
  failure_path_test: malformed packet bundles, excess packet/payload limits, unsupported codec/remux, decode-validation failure, timestamp discontinuity, and provenance corruption fail closed without muting known audio
  security_test: direct/HLS source authorization and memory-only FFmpeg protocol policy remain unchanged; export performs no source re-fetch
  benchmark: fixture operation evidence proves no PCM16 state write and no AAC encoder on compatible passthrough; storage math is reported as workload-specific, not a universal throughput claim
```

Invariant decisions:

- [x] S0 source/container and HLS-resource records remain byte-exact and unchanged; 0x0103 is an additional deterministic profile state, not a replacement for S0.
- [x] 0x0102 is a compatibility tombstone for new writes but remains supported by the existing verified reader and PCM16-to-AAC legacy export path.
- [x] 0x0103 is profile-local, versioned, bounded, and includes exact packet bytes plus sufficient codec/timing metadata for deterministic verification and container export.
- [x] Audio validation remains fail-closed and streaming; decoded frames are discarded and no libswresample/aggregate PCM allocation is used by new ingest.
- [x] Compatible MP4 audio uses packet remux; unsupported exact trims or codecs fail explicitly, never silently mute.
- [x] Direct-video and HLS video-frame provenance, authorization, CLI syntax, generic archive/C ABI, standalone Audio Profile, transport, distributed, telemetry, and Stage G semantics are unchanged.
- [x] FFmpeg-disabled builds retain the media-library-independent encoded-audio schema/reader and explicit backend-unavailable export behavior.

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

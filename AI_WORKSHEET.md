# CODEC AI Worksheet

Canonical work loop for ChatGPT, Codex, and other agentic contributors.

**Purpose:** complete one bounded unit of work without overstating capability, weakening CODA truth semantics, or coupling CODEC core to one profile.

> Read `README.md` first. Read deeper design docs only when the task touches their subject. Do not reread large historical plans unless they are directly relevant.

## Active work record — CODEC v0.4.0 release preparation

```yaml
task: Promote the merged Stage H.1 compressed-media implementation to CODEC v0.4.0 with synchronized CMake/package/CLI version reporting and release-grade user documentation.
base_ref: main
base_head_sha: 50bede31c21f5e1972d3e697ff2dd4580701f3c0
work_branch: codex/release-0.4.0-prep
current_version: 0.3.0
target_version: 0.4.0
active_roadmap_stage: H.1 is merged and release-ready; this bounded task packages that proven state as v0.4.0 without advancing to H.2 or Stage G.
continuity_evidence:
  - git_head: main is 50bede31c21f5e1972d3e697ff2dd4580701f3c0, the verified merge of PR 59 leading-trim MP4 passthrough
  - open_prs: none at release-prep branch creation
  - prior_exact_head_ci: PR 59 head bd7600912c2ff3bb0752226ec3568d7cba23ab02 passed CI run 33466004590 across GCC, Clang, sanitizers, FFmpeg-disabled, CLI integration, install, and installed-package consumers before merge
  - release_workflow: release/v* requires branch head to equal main, CMake/README/changelog version consistency, and publishes GitHub tag/release v<version>
  - roadmap_issue: issue 10 is the unique exact-title CODEC v1.0 roadmap execution log; current code/tests/build/docs remain authoritative
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: [docs, cli-version, package-version]
touched_truth_classes: []
current_behavior_verified_from: [main HEAD, CMakeLists.txt, src/cli/main.cpp, tests/cli_integration.sh, README.md, CHANGELOG.md, release workflow]
new_capability_claim: none; v0.4.0 packages already-merged H.1 behavior and synchronizes release/version documentation.
change_class: documentation_only
verification:
  release_configure: pending exact-head CI
  release_build: pending exact-head CI
  tests: pending exact-head CI
  sanitizer_build: pending exact-head CI
  sanitizer_tests: pending exact-head CI
  ffmpeg_disabled: pending exact-head CI
  install_and_package_consumer: pending exact-head CI
  cli_version: require `codec --version` = `codec 0.4.0`
  cli_help: require help banner begins with `CODEC 0.4.0`
  cli_capabilities: require JSON `"version":"0.4.0"`
  release_metadata: require CMake project version, README machine-readable manifest, changelog section, CLI integration expected version, and package version to agree on 0.4.0
  exact_head_ci: pending after final release-prep commit
review:
  diff_scope: version metadata, CLI version assertions, README, CHANGELOG, release documentation, and worksheet only
  compatibility: no archive/API/CLI command semantics change; versioned shared-library/package metadata advances to major SOVERSION 0 as already derived from CMake major version
  performance_claim: no universal storage or throughput ratio; documentation describes structural removal of VFR1/PCM16 duplication and gives workload-specific examples only
```

```text
BEFORE: Merged main contains the H.1 EVP1/EAP1 compressed-media ingest/export implementation, but project/package/CLI metadata and release-facing documentation still identify the tree as 0.3.0 or unreleased H.1.
AFTER: The same runtime behavior is packaged and documented consistently as CODEC 0.4.0; CMake/package metadata, `codec --help`, `codec --version`, `codec capabilities`, README, changelog, and release documentation agree on 0.4.0.
```

```yaml
proof:
  regression_test: CLI integration must verify `--version`, help banner, and capabilities all use the CMake-provided 0.4.0 version
  exactness_test: n/a; no S0/S1/D encoding change
  compatibility_test: full existing unit/CLI/install/package-consumer matrix remains green, including legacy VFR1/PCM16 compatibility
  failure_path_test: release workflow metadata validation remains able to reject inconsistent branch/version/changelog metadata
  security_test: n/a; no capture/network authorization change
  benchmark: n/a; no new performance claim
```

Release documentation requirements:

- [ ] Present CODEC with a concise, accurate program description suitable for the README/release page.
- [ ] Explain preservation-first S0/S1/D semantics without implying inference or identity guarantees.
- [ ] Document the v0.4.0 H.1 storage model: exact source/container S0 plus compressed H.264 EVP1 (`0x0104`) and AAC EAP1 (`0x0103`).
- [ ] State that new H.1 ingest decodes video/audio only for validation and does not persist decoded VFR1 pixels or Video Profile PCM16.
- [ ] Document direct MP4 and same-origin unencrypted HLS capture boundaries.
- [ ] Document MP4 packet remux, Annex-B `extract_extradata`, ADTS `aac_adtstoasc`, and representable AAC leading trim via MP4 edit lists.
- [ ] Document fail-closed cases and legacy VFR1 (`0x0101`) / Video PCM16 (`0x0102`) compatibility.
- [ ] Synchronize all user-visible 0.3.0 version references that describe the current release to 0.4.0 while retaining historical 0.3.0 changelog/release references.
- [ ] Add durable `docs/releases/0.4.0.md` release notes/migration guidance.
- [ ] Verify the existing `release/v0.4.0` workflow contract before publication.

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
# CODEC AI Worksheet

Canonical work loop for ChatGPT, Codex, and other agentic contributors.

**Purpose:** complete one bounded unit of work without overstating capability, weakening CODA truth semantics, or coupling CODEC core to one profile.

> Read `README.md` first. Read deeper design docs only when the task touches their subject. Do not reread large historical plans unless they are directly relevant.

## Active work record — Stage F.5

```yaml
task: Add bounded deterministic multi-partition scheduling that composes F.1 partitions through F.4 location resolution, F.3 exact retrieval, and F.2 worker execution with per-partition outcomes.
base_ref: origin/main
base_head_sha: 1e48a16b11378897b0311f4c198c443d1a1bb976
work_branch: automation/stage-f5-bounded-orchestration
current_version: 0.2.0
active_roadmap_stage: F — F.1 partitioning, F.2 one-partition execution, F.3 exact retrieval, and F.4 bounded location indexing are merged; bounded multi-partition scheduling/orchestration is the next unmet dependency.
continuity_evidence:
  - git_head: main at 1e48a16b11378897b0311f4c198c443d1a1bb976
  - open_prs: stale unrelated draft PR 26 is preserved; F.5 uses its own branch/PR 37
  - exact_head_ci: F.4 merge evidence is recorded in roadmap issue 10; F.5 RED head 3aa5ccf612be995d3f57f0702c094b2f9400b144 failed only on the intentionally absent scheduler API
  - roadmap_issue: issue 10 records F.4 complete and requires a fresh Stage F dependency selection
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: distributed
touched_truth_classes: []
current_behavior_verified_from: [code, tests, cli, changelog]
new_capability_claim: A caller can schedule a bounded ordered batch of exact F.1 partitions across an ordered worker pool, deterministically select canonical F.4 placements, materialize through F.3, execute through F.2, and receive one explicit outcome per partition.
change_class: generic_stream_abstraction
```

```text
BEFORE: F.1-F.4 expose exact partitioning, one-partition worker execution, exact retrieval, and bounded canonical placement lookup, but callers must coordinate more than one partition themselves.
AFTER: schedule_partitions preflights a bounded ordered batch, assigns workers by stable input-position round robin, selects the first canonical F.4 candidate for each exact member, composes F.3 retrieval into F.2 execution, and returns one ordered success/failure outcome per input partition.
```

```yaml
proof:
  regression_test: tests/test_distributed_scheduler.cpp plus unchanged F.1/F.2/F.3/F.4 and all existing tests
  exactness_test: each F.1 CDP1 identity and stream membership is revalidated before side effects; F.4 canonical candidate order is preserved; F.3 still verifies exact length and SHA-256 before F.2 receives materialized records
  compatibility_test: installed-package consumer exercises F.5 from installed <codec/distributed.hpp>; F.1-F.4 APIs, CODA, S0/S1/D, provenance, Stage E, C ABI, and CLI remain compatible
  failure_path_test: invalid worker/batch/aggregate bounds and tampered/duplicate partitions fail before backend or worker side effects; missing locations, retrieval failures, and execution failures become ordered per-partition outcomes while later partitions continue
  security_test: scheduling consumes caller-supplied workers, backend, and F.4 index only; it adds no credential, authorization, authentication, attestation, discovery, health, retry, failover, lease, or exactly-once decision
  benchmark: n/a — synchronous deterministic orchestration adds no throughput, latency, concurrency, availability, durability, fault-tolerance, or scale claim
```

Invariant decisions:

- [x] S0/S1/D semantics remain unchanged; F.5 coordinates existing exact/derived boundaries only.
- [x] F.1 `DistributedPartition` and CDP1 bytes remain unchanged and are revalidated before scheduling side effects.
- [x] The complete batch is structurally and aggregately preflighted before the first backend read or worker invocation.
- [x] Worker assignment is deterministic from input position (`partition_index % workers.size()`) and is not perturbed by prior partition failures.
- [x] F.5 selects only the first canonical F.4 candidate for each exact member; it invents no health, locality, cost, retry, or failover ranking.
- [x] F.4/F.3/F.2 failures after preflight are captured as explicit per-partition outcomes and do not reorder or suppress later partitions.
- [x] Retryable nested errors are preserved as evidence but F.5 performs no retry.
- [x] F.5 is synchronous and sequential; it adds no threads, RPC/network execution, worker/backend discovery, persistent/global catalogs, leases, heartbeats, exactly-once semantics, deployment integration, or scale claim.

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

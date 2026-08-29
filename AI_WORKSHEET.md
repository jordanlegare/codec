# CODEC AI Worksheet

Canonical work loop for ChatGPT, Codex, and other agentic contributors.

**Purpose:** complete one bounded unit of work without overstating capability, weakening CODA truth semantics, or coupling CODEC core to one profile.

> Read `README.md` first. Read deeper design docs only when the task touches their subject. Do not reread large historical plans unless they are directly relevant.

## Active work record — Stage F.6

```yaml
task: Add a bounded provider-neutral remote-worker transport boundary that implements the existing DistributedWorker interface without changing F.5 scheduling semantics.
base_ref: origin/main
base_head_sha: f98575cb88ac2115b7960c4fb4d417e9b8a381d0
work_branch: automation/stage-f6-remote-worker-transport
current_version: 0.2.0
active_roadmap_stage: F — F.1 partitioning, F.2 one-partition execution, F.3 exact retrieval, F.4 location indexing, and F.5 bounded multi-partition scheduling are merged; a generic remote-worker transport seam is the next unmet dependency.
continuity_evidence:
  - git_head: main at f98575cb88ac2115b7960c4fb4d417e9b8a381d0
  - open_prs: stale unrelated draft PR 26 is preserved; F.6 uses its own branch/PR 39
  - exact_head_ci: F.5 final head 1c1e01baf7c2d44255e75c9318319cb04c37c2b4 passed CI 265 before merge; F.6 RED head a264a10dec099e08d02ca4f0f9ceb31a37ae8aae failed only on the intentionally absent remote-worker API
  - roadmap_issue: issue 10 records F.5 complete and identifies the bounded remote-worker transport seam as the smallest next Stage F dependency
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: distributed
touched_truth_classes: []
current_behavior_verified_from: [code, tests, changelog]
new_capability_claim: A caller-supplied DistributedWorkerTransport can back RemoteDistributedWorker, dispatch one bounded exact materialized input batch once to configured worker/processor labels, return bounded outputs or one explicit error, and compose unchanged through F.2 and F.5.
change_class: generic_stream_abstraction
```

```text
BEFORE: F.5 accepts any DistributedWorker, but CODEC supplies only the in-process LocalProcessorWorker and no bounded generic remote transport adapter.
AFTER: RemoteDistributedWorker validates bounded request/label metadata, delegates exactly once through a caller-supplied DistributedWorkerTransport, requires descriptive response identity to match, bounds returned outputs, preserves transport errors without retry, and returns outputs to F.2 for authoritative semantic validation; F.5 remains unchanged.
```

```yaml
proof:
  regression_test: tests/test_distributed_remote_worker.cpp plus unchanged F.1-F.5 and all existing tests
  exactness_test: F.2 still verifies CDP1 identity, exact ordered links, payload sizes and SHA-256 before invoking the remote worker; F.6 does not redefine exactness or truth semantics
  compatibility_test: installed-package remote consumer implements DistributedWorkerTransport from installed <codec/distributed.hpp> and executes through RemoteDistributedWorker/F.2; CODA, F.1-F.5, Stage E, C ABI, and CLI remain compatible
  failure_path_test: invalid limits/labels/input sizes fail before dispatch; transport errors propagate exactly once; response identity mismatch and output exhaustion fail closed; invalid ProcessorOutput semantics are rejected by F.2 after one dispatch
  security_test: worker/processor/transport names remain descriptive routing evidence only; F.6 adds no endpoint policy, credential, authentication, authorization, attestation, discovery, retry/failover, lease, or exactly-once decision
  benchmark: n/a — no network availability, throughput, latency, concurrency, fault-tolerance, durability, or scale claim
```

Invariant decisions:

- [x] S0/S1/D, CODA, CDP1, provenance, Stage E, C ABI, and CLI semantics remain unchanged.
- [x] F.2 remains authoritative for partition identity, ordered membership, stream, SHA-256, and ProcessorOutput semantic validation.
- [x] F.5 public API and scheduler implementation remain unchanged; RemoteDistributedWorker is an ordinary DistributedWorker.
- [x] Adapter-owned limits, label shape, input count/bytes, and input payload-size metadata are validated before transport dispatch.
- [x] One successful preflight causes exactly one caller-supplied transport dispatch; retryable provider errors are preserved but never retried by F.6.
- [x] Successful responses must echo configured worker/processor labels and remain inside caller output count/byte bounds before returning to F.2.
- [x] Matching labels are not authentication, authorization, attestation, or proof that a specific machine executed the work.
- [x] F.6 defines no CODEC RPC wire format or concrete socket/HTTP/gRPC transport, discovery/health, concurrency, retry/failover, leases/heartbeats/cancellation/exactly-once semantics, persistence, deployment, or scale claim.

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

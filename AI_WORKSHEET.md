# CODEC AI Worksheet

Canonical work loop for ChatGPT, Codex, and other agentic contributors.

**Purpose:** complete one bounded unit of work without overstating capability, weakening CODA truth semantics, or coupling CODEC core to one profile.

> Read `README.md` first. Read deeper design docs only when the task touches their subject. Do not reread large historical plans unless they are directly relevant.

## Active work record — Stage F.7

```yaml
task: Add bounded deterministic request/reply envelope serialization for the existing F.6 remote-worker structures without adding a concrete network transport.
base_ref: origin/main
base_head_sha: 5892437b150ab8e4e7b16ece95870c70130a553c
work_branch: automation/stage-f7-remote-envelope-codec
current_version: 0.2.0
active_roadmap_stage: F — F.1-F.6 are merged; deterministic bounded remote-worker serialization is the current milestone before any concrete HTTP/gRPC/socket provider.
continuity_evidence:
  - git_head: main at 5892437b150ab8e4e7b16ece95870c70130a553c before F.7; post-F.6 main push CI run 277 passed
  - open_prs: no open PRs existed before F.7; F.7 uses branch automation/stage-f7-remote-envelope-codec and PR 40
  - exact_head_ci: F.7 RED head e21c04ef39bdee573f918737248ff85fcb4d1bc1 failed only because the public wire header was intentionally absent; declaration-only head f9e1f87ea689480384727471166f2bf5a61ddb36 compiled and failed at the intentionally unresolved encoder symbol; implementation/package/strict proof heads are recorded in PR 40
  - roadmap_issue: issue 10 records F.6 complete and identifies deterministic remote request/reply serialization as the smallest next Stage F dependency
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: distributed
touched_truth_classes: []
current_behavior_verified_from: [code, tests, changelog]
new_capability_claim: CODEC can deterministically encode and strictly decode bounded DRQ1 remote execution requests plus DRS1 success/error replies that preserve existing F.6 structured data exactly enough for a later provider to transport them without inventing a private serialization format.
change_class: generic_stream_abstraction
```

```text
BEFORE: F.6 exposes a structured provider-neutral DistributedWorkerTransport seam, but concrete providers would have to invent their own request/reply serialization.
AFTER: <codec/distributed_wire.hpp> exposes deterministic bounded DRQ1/DRS1 codecs for F.6 request records, ProcessorOutput/ProvenanceProcess success replies, and explicit Error replies; the codec adds no network I/O and F.2 remains authoritative for execution/truth/provenance semantics.
```

```yaml
proof:
  regression_test: tests/test_distributed_wire.cpp and tests/test_distributed_wire_strict.cpp plus unchanged F.1-F.6 and all existing tests
  exactness_test: request round trips preserve complete RecordInfo, ordered payload bytes, hashes, file offsets, and unknown 16-bit record type codes; F.2 still performs CDP1/input SHA-256 and ProcessorOutput semantic validation after decode
  compatibility_test: installed-package remote-wire consumer uses only installed <codec/distributed_wire.hpp> and codec::codec to round-trip a request, success reply, and retryable Error under GCC and Clang
  failure_path_test: zero/oversized limits, malformed lengths, magic/version/flags, truncation/trailing bytes, digest mismatch, reserved fields, unknown outcome/truth/error codes, invalid retryable byte, and process/input/output/error bounds fail closed; strict tests recompute a valid digest after deep-field mutation to prove parser canonicality independently of checksum failure
  security_test: DRQ1/DRS1 SHA-256 is corruption evidence only; it is not authentication, authorization, active-tamper protection, confidentiality, replay protection, attestation, or proof of remote execution
  benchmark: n/a — no network availability, throughput, latency, concurrency, fault-tolerance, durability, or scale claim
```

Invariant decisions:

- [x] S0/S1/D, CODA, CDP1, provenance persistence, Stage E, C ABI, and CLI semantics remain unchanged.
- [x] F.7 is isolated in <codec/distributed_wire.hpp> and src/distributed/wire.cpp; F.6 worker, remote-worker, and scheduler behavior is unchanged.
- [x] DRQ1 preserves full existing ExtractedRecord/RecordInfo metadata and payload order, including unknown 16-bit type codes, without claiming partition exactness on its own.
- [x] DRS1 preserves structurally defined TruthClass values, ProcessorOutput payloads, full ProvenanceProcess metadata/optional hashes/details, and one explicit Error alternative.
- [x] Stable explicit wire numbers map every current non-ok ErrorCode; ErrorCode::ok is never encoded as an error result.
- [x] Decode validates fixed headers, declared lengths, reserved fields, enum domains, aggregate/resource limits, exact body consumption, and SHA-256 before accepting an envelope.
- [x] F.2 remains authoritative for CDP1 identity, stream/member SHA-256, S1/D-only processor outputs, interval/type rules, and provenance/process semantic validity.
- [x] F.7 adds no socket/HTTP/TLS/QUIC/gRPC provider, endpoint/DNS/SSRF policy, credential/authentication/authorization/attestation, signing/encryption/replay protection, discovery/health, retry/failover, leases/heartbeats/cancellation/idempotency/exactly-once, concurrency/server loop, persistence, deployment, or scale claim.

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

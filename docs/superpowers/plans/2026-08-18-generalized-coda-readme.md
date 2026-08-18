# Generalized CODA README Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Update `README.md` so future ChatGPT/Codex work evolves CODEC toward a vendor-neutral generalized authenticated temporal-stream substrate while preserving the truthful v0.1.0 audio-first implementation boundary.

**Architecture:** Keep the current README as the normative product specification, but add a concise platform-direction layer above later implementation phases. Generalize CODA conceptually without changing current binary-format claims, define future recovery/distributed/trust profiles as optional extensions, and give future agents explicit implementation rules that prioritize preservation invariants and honest status reporting.

**Tech Stack:** Markdown documentation for a C++20/CMake project; no source-code or archive-format changes in this tranche.

**Spec:** `docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md`

## Global Constraints

- Preserve all current v0.1.0 claims and implementation boundaries exactly.
- Audio remains the first specialization while CODA evolves toward a general authenticated temporal container.
- Kubernetes, AWS, Azure, GCP, PAR2, Reed-Solomon, OFDM, QAM, AC-3, justice, surveillance, military, and other deployment-specific technologies must not become CODA-core dependencies.
- S0/S1 source truth must remain distinguishable from D-class derived inference.
- Recovery, distributed/cloud, and selective-disclosure capabilities must be explicitly labeled planned/future until implemented and tested.
- No source-code behavior or public archive-format change is part of this README update.

---

### Task 1: Add generalized CODA platform direction

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: current v0.1.0 status, fidelity classes, architecture, and delivery roadmap already documented in `README.md`.
- Produces: normative documentation for generalized logical streams, heterogeneous payload types, and stable provenance semantics.

- [ ] **Step 1: Read the current README around Status, Capability boundary, Architecture, archive format, and delivery phases**

Verify the existing implementation-status language is retained and identify insertion points that do not overwrite current guarantees.

- [ ] **Step 2: Add a `Generalized CODA platform direction` section**

Include these normative points:

```text
Audio remains the first implemented specialization.
CODA is intended to evolve into a general authenticated temporal container.
Logical stream identity is independent of transport port, process, carrier frequency, or worker assignment.
Future stream types may include audio, video, imagery, telemetry, sensor observations, navigation data, documents/text, model outputs, recovery shards, and audit/authorization records.
Unknown stream types must remain preservable even when the current engine cannot interpret them.
Current v0.1.0 implementation remains audio-oriented and does not yet implement the generalized record model.
```

- [ ] **Step 3: Add conceptual record-envelope guidance without freezing a binary layout**

Document the conceptual fields:

```text
stream_id
stream_type
source_id
time/clock fields
sequence/epochs
fidelity_class
payload_type
payload
payload_hash
provenance
policy_tags
```

State explicitly that the exact binary representation remains versioned implementation work and must preserve compatibility rules.

- [ ] **Step 4: Review the new section against the existing S0/S1/D definitions**

Confirm no sentence implies that derived inference, translated material, reconstructed material, neural stems, or recovery outputs become source truth.

- [ ] **Step 5: Commit the documentation increment**

```bash
git add README.md
git commit -m "docs: generalize CODA platform direction"
```

### Task 2: Add future profile boundaries and scale semantics

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: generalized stream semantics from Task 1.
- Produces: profile boundaries for recovery, multiplexed transport, distributed/cloud processing, and confidential/selective disclosure.

- [ ] **Step 1: Add a `Future extension profiles` subsection**

Describe four optional profiles:

```text
Transport/recovery profile
Distributed/cloud profile
Trust/selective-disclosure profile
Vertical/domain profiles
```

Each profile must say `planned` or `future` and must not alter CODA source-truth semantics.

- [ ] **Step 2: Define multiplexing/scale semantics**

Document that one endpoint can carry many logical streams, one logical stream can migrate between workers/endpoints while retaining identity, and scale is bounded by bandwidth/compute/memory/storage rather than port-number or identifier-space limits.

- [ ] **Step 3: Define recovery semantics**

State that future recovery records may represent parity/FEC shards, coding groups, reconstruction hashes, loss observations, and exactness/correction residuals. Explicitly distinguish exact recovery of an encoded bitstream from exact reconstruction of a pre-lossy source.

- [ ] **Step 4: Define distributed/cloud semantics**

Describe a vendor-neutral flow such as:

```text
global/regional ingress -> stream-ID partitioning -> preservation workers -> optional recovery/inference workers -> CODA/object archive -> query/index services
```

State that Kubernetes and specific cloud providers are deployment choices, not core format requirements.

- [ ] **Step 5: Define selective-disclosure semantics**

State that future secure profiles may record encryption envelopes, scoped authorization/policy records, access/audit events, and selective exports, while CODEC itself does not create legal authority.

- [ ] **Step 6: Commit the profile documentation increment**

```bash
git add README.md
git commit -m "docs: define CODA extension profiles"
```

### Task 3: Add ChatGPT/Codex build guidance and roadmap integration

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: generalized platform and profile requirements from Tasks 1-2.
- Produces: explicit development instructions and staged delivery ordering for future agentic work.

- [ ] **Step 1: Add `ChatGPT/Codex implementation guidance`**

Require future agents to:

```text
inspect repository state/tests before edits;
never claim unimplemented capability is complete;
keep status tables synchronized with code;
preserve deterministic/probabilistic boundaries;
prefer narrow testable primitives;
keep core abstractions vendor-neutral;
implement archive/transport invariants before hyperscale deployment;
add compatibility/corruption tests for archive changes;
ensure inference failure cannot break preservation;
record provenance for all D-class artifacts;
benchmark before scale claims;
treat security/privacy/authorization/retention as first-class interfaces when those profiles are implemented.
```

- [ ] **Step 2: Integrate the generalized direction into the delivery roadmap**

Order later work as:

```text
A. preserve v0.1 foundation
B. generalized record semantics
C. complete audio 1.0 path
D. transport/recovery profile
E. distributed processing profile
F. trust/selective-disclosure profile
G. vertical profiles
```

Do not erase existing dated milestones; explain how the new stages constrain/extend them.

- [ ] **Step 3: Add explicit implementation-status disclaimer**

State that the generalized record model, recovery profile, distributed/cloud processing, confidential-compute/selective-disclosure profile, and vertical profiles are architectural requirements/plans, not current v0.1.0 capabilities.

- [ ] **Step 4: Verify README consistency**

Check that searches for `v0.1.0`, `model_incompatible`, `S0`, `S1`, `D-class`, `W0`, `W1`, `W2`, and current media-layer limitations still produce the original truthful claims.

- [ ] **Step 5: Verify documentation-only scope**

Confirm the branch diff changes only:

```text
README.md
docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md
docs/superpowers/plans/2026-08-18-generalized-coda-readme.md
```

- [ ] **Step 6: Commit the final README guidance increment**

```bash
git add README.md
git commit -m "docs: guide agentic development toward generalized CODA"
```

### Task 4: Final verification and review handoff

**Files:**
- Review: `README.md`
- Review: `docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md`
- Review: `docs/superpowers/plans/2026-08-18-generalized-coda-readme.md`

**Interfaces:**
- Consumes: completed documentation changes.
- Produces: verified branch ready for pull request/review.

- [ ] **Step 1: Compare branch against `main`**

Expected changed files are the three documentation files listed in Task 3.

- [ ] **Step 2: Inspect the complete README diff**

Reject the change if any existing v0.1.0 implemented capability is silently upgraded, downgraded, or removed.

- [ ] **Step 3: Check for placeholders and ambiguous status language**

The new sections must contain no `TBD`, no `TODO`, and no wording that presents future profiles as implemented.

- [ ] **Step 4: Check design coverage**

Confirm README guidance covers generalized stream identity/types, fidelity boundaries, provenance, multiplexing, recovery, cloud/distributed processing, selective disclosure, agent guidance, staged delivery, and testing/benchmark discipline.

- [ ] **Step 5: Prepare the branch for review**

Report the final commit SHA and summarize the documentation-only scope. Do not merge without an explicit merge request or repository policy authorizing automatic merge.
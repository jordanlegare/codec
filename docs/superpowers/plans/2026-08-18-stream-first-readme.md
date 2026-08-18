# Stream-First README Correction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite CODEC's README so the normative product and architecture are heterogeneous-stream-first, CODA is type-agnostic, and audio is documented as the first implemented reference profile rather than CODEC core.

**Architecture:** Preserve truthful v0.1.0 behavior while changing the README's semantic center of gravity. Generic Stream/CODA invariants govern the opening definition, truth model, processing pipeline, timing, identity, query/extraction, acceptance criteria, and roadmap; existing PCM/WAV/W0/W1/W2/separation material remains as Audio Stream Profile documentation.

**Tech Stack:** Markdown documentation, C++20 project terminology, existing GitHub CI.

**Spec:** `docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md`

## Global Constraints

- CODEC core is heterogeneous-stream-first and CODA is payload-type agnostic.
- Audio remains the first implemented reference profile; do not claim non-audio profiles are implemented.
- S0 is source-exact; S1 is state-exact; D is derived.
- Existing audio functionality and truthful v0.1.0 implementation status must not be weakened or overstated.
- Generic core requirements govern conflicts with profile-specific requirements.
- No cloud vendor, transport, recovery algorithm, or vertical-domain technology becomes a core dependency.
- This change modifies documentation only; no source/API/archive-format behavior changes.

---

### Task 1: Rewrite the README's normative core

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: corrected stream-first design spec.
- Produces: a README whose opening definition, goals, invariants, conceptual architecture, record model, truth classes, and agent guidance define CODEC/CODA generically.

- [ ] **Step 1: Capture the current README and identify normative audio-first statements**

Search the opening/product definition, goals, fidelity model, architecture, APIs, ingest, timing, identity, query/extraction, acceptance criteria, and roadmap for language that makes audio/PCM/speakers/stems fundamental rather than profile-specific.

- [ ] **Step 2: Rewrite the product definition and architectural hierarchy**

State that CODEC aggregates authorized heterogeneous logical streams and that CODA is the authenticated temporal archive. Introduce generic `StreamId`, `StreamType`, `StreamSpec`, `StreamClock`, `StreamRecord`, `StreamProvenance`, `StreamAdapter`, `StreamProcessor`, `StreamInference`, and `StreamExtraction` as the direction for core concepts.

- [ ] **Step 3: Rewrite fidelity/truth semantics**

Define S0 as source-exact, S1 as deterministic state-exact under registered profile canonicalization, and D as derived. Explicitly state that sample-exact integer PCM is the Audio Stream Profile specialization of S1.

- [ ] **Step 4: Make the generic processing pipeline primary**

Document `source -> StreamAdapter -> S0 -> typed StreamRecord/CODA -> optional S1/profile processing/inference/identity/recovery`, with preservation priority and unknown-type preservation.

- [ ] **Step 5: Reclassify audio-specific functionality**

Keep existing WAV/PCM, FLAC roadmap, W0/W1/W2, diarization, embeddings, neural separation/stems, speech/music, HLS/Icecast/audio transport, and audio benchmarking material, but label/scope it under Audio Stream Profile and current implementation status.

- [ ] **Step 6: Generalize cross-cutting sections**

Update ingest, timing, identity, inference, query/extraction, multiplexing, testing, acceptance, and roadmap wording so generic stream invariants are primary and audio examples are profile-specific.

- [ ] **Step 7: Add an explicit implementation-status boundary**

State that v0.1.0 remains primarily an Audio Stream Profile implementation and that generalized non-audio profile semantics are architectural direction/planned work until implemented and tested.

- [ ] **Step 8: Commit the README rewrite**

Commit message: `docs: make CODEC stream-first and audio a profile`

### Task 2: Residual audio-core audit

**Files:**
- Modify if necessary: `README.md`

**Interfaces:**
- Consumes: Task 1 README.
- Produces: corrected README with no accidental audio-only core assumptions.

- [ ] **Step 1: Search for residual core-defining audio vocabulary**

Audit occurrences of `audio`, `PCM`, `sample-exact`, `speaker`, `speech`, `music`, `stem`, `watermark`, `FeedSpec`, and `feed` and classify each occurrence as either legitimate Audio Profile/current-implementation text or an accidental core assumption.

- [ ] **Step 2: Correct accidental core assumptions**

Replace core-level audio assumptions with generic stream semantics while retaining exact API names where they describe existing compatibility surfaces.

- [ ] **Step 3: Verify success criteria manually**

Confirm a new reader cannot reasonably infer that CODEC core means audio processing; S1 is not intrinsically PCM; unknown non-audio streams are preservable conceptually; and existing audio functionality remains documented truthfully.

- [ ] **Step 4: Commit audit corrections if any**

Commit message: `docs: remove residual audio assumptions from core`

### Task 3: Verify repository and CI

**Files:**
- Verify: `README.md`
- Verify: `docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md`
- Verify: `docs/superpowers/plans/2026-08-18-stream-first-readme.md`

**Interfaces:**
- Consumes: final documentation tree.
- Produces: reviewable branch/PR with evidence that the documentation correction did not alter source behavior.

- [ ] **Step 1: Compare branch to `main`**

Confirm only intended documentation files changed and inspect the README diff for accidental deletion of existing implementation details.

- [ ] **Step 2: Open a pull request**

Describe the semantic correction, current implementation boundary, S1 state-exact change, and Audio Profile classification.

- [ ] **Step 3: Run/inspect existing CI**

Require the repository's existing build/test matrix to complete successfully on the final branch head before claiming completion.

- [ ] **Step 4: Final verification**

Verify PR head SHA, changed files, mergeability, and successful checks. Do not merge unless explicitly directed by the user.

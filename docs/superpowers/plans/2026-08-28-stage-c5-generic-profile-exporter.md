# Stage C.5 Generic Profile Exporter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded caller-supplied generic exporter for exact extracted records that returns provider-owned typed bytes plus exact ordered supporting record links, without archive or filesystem writes.

**Architecture:** Extend the existing generic processing boundary in `processing.hpp/.cpp`. `StreamExporter` receives a caller-owned span of exact `ExtractedRecord` inputs and returns an `ExporterOutput`; `invoke_exporter()` validates input count/bytes/record ownership and caller limits before provider invocation, validates output metadata/bytes afterward, and derives immutable `ProvenanceRecordLink` support links from the supplied authenticated `RecordInfo` values in input order. No archive format, CLI, C ABI, audio-profile, registry, scheduler, or persistence behavior changes.

**Tech Stack:** C++20, CMake >= 3.20, existing `codec::Result`, CODA record metadata/provenance types, repository unit/CI test harness.

**Spec:** `docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md` plus the Stage C.5 planned-work contract in roadmap issue #10.

## Global Constraints

- Preserve S0 before optional interpretation; never silently conflate S0, S1, and D.
- Logical `StreamId` remains independent of transport/archive placement.
- Unknown compatible payload types remain preservable/extractable.
- Export is caller-paced and performs no archive, filesystem, network, or registry writes.
- Provider errors propagate unchanged.
- No new archive format, C ABI, CLI, audio, deployment, scale, or frozen-CODA-v1 claim.

---

### Task 1: Add RED exporter contract tests

**Files:**
- Modify: `tests/test_processing.cpp`

**Interfaces:**
- Consumes: existing `ExtractedRecord`, `ProvenanceRecordLink`, `Result`, `ErrorCode`.
- Produces expected API: `ExporterOutput`, `ExportResult`, `ExporterRunLimits`, `StreamExporter`, `invoke_exporter()`.

- [ ] **Step 1: Add a telemetry CSV exporter proof.** Create two exact `ExtractedRecord` inputs with distinct stream/type/sequence/hash values. The exporter verifies the payloads it receives and returns `payload_type = "text/csv"` and exact CSV bytes.
- [ ] **Step 2: Assert the success contract.** `invoke_exporter()` must return byte-identical provider output and `supporting_records` equal to each input's stream/type/sequence/hash in the original input order.
- [ ] **Step 3: Add deterministic validation tests.** Cover empty inputs, excessive inputs, mismatched payload size, cumulative input bytes over limit, zero input/output/payload-type limits, empty payload type, overlong payload type, oversized output, and unchanged provider error propagation. Pre-invocation failures must leave a counting exporter uncalled.
- [ ] **Step 4: Push the tests only and verify RED CI.** Expected failure: compilation because the exporter API does not yet exist; unrelated targets should still compile as far as possible.

### Task 2: Implement the minimal generic exporter boundary

**Files:**
- Modify: `include/codec/processing.hpp`
- Modify: `src/core/processing.cpp`

**Interfaces:**
- Produces:
  - `struct ExporterOutput { std::string payload_type; std::vector<std::byte> payload; };`
  - `struct ExportResult { std::string payload_type; std::vector<std::byte> payload; std::vector<ProvenanceRecordLink> supporting_records; };`
  - `struct ExporterRunLimits { std::size_t maximum_inputs{4096}; std::uint64_t maximum_input_bytes{64ULL * 1024ULL * 1024ULL}; std::uint64_t maximum_output_bytes{64ULL * 1024ULL * 1024ULL}; std::size_t maximum_payload_type_bytes{256}; };`
  - `class StreamExporter` with `name()` and `export_records(std::span<const ExtractedRecord>)`.
  - `Result<ExportResult> invoke_exporter(StreamExporter&, std::span<const ExtractedRecord>, ExporterRunLimits = {});`

- [ ] **Step 1: Add the public API declarations only after RED is observed.** Keep the exporter beside adapter/processor contracts.
- [ ] **Step 2: Validate before provider invocation.** Reject zero limits, empty inputs, input count above `maximum_inputs`, inputs whose payload bytes do not match `RecordInfo::payload_size`, and cumulative input bytes above `maximum_input_bytes`. Use `invalid_argument` for invalid contracts and `resource_exhausted` for exceeded configured resource bounds.
- [ ] **Step 3: Invoke the provider once and preserve any provider `Error` unchanged.** Do not catch/rewrite provider error code, message, or retryable flag.
- [ ] **Step 4: Validate provider output.** Reject an empty payload type, payload type longer than `maximum_payload_type_bytes`, and payload bytes larger than `maximum_output_bytes`; use deterministic `invalid_argument`/`resource_exhausted` codes.
- [ ] **Step 5: Derive support links.** For every input in order, copy `record.stream`, `record.type_code()`, `record.sequence`, and `record.hash` into `ProvenanceRecordLink`. Return those links with provider bytes and payload type unchanged.
- [ ] **Step 6: Run exact-head CI and keep the implementation minimal until all tests pass.** No archive/filesystem writes and no new dependencies.

### Task 3: Synchronize capability documentation and merge gates

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: proven Stage C.5 behavior.
- Produces: truthful current-status statements only.

- [ ] **Step 1: Update README implemented generic capabilities.** Add the bounded generic profile-exporter boundary and retain explicit non-claims for generalized CLI/C ABI/profile migration.
- [ ] **Step 2: Add an Unreleased changelog entry.** State caller-supplied typed export with exact ordered support links and no archive write/format change.
- [ ] **Step 3: Audit the full PR diff for scope and AI-contract reference preservation.** No unrelated rewrites or dropped references.
- [ ] **Step 4: Require exact-head GCC, Clang, sanitizer, unit, C ABI, CLI, AI-contract, and install/package checks to be green before merge.
- [ ] **Step 5: Squash-merge only the verified exact head, then verify the resulting `main` CI and append the Stage C.5 completion evidence to roadmap issue #10.

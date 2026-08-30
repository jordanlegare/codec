# Stage H.2 Telemetry Stream Profile Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an installed, dependency-free Telemetry Stream Profile that deterministically encodes bounded canonical metric descriptors and scalar snapshot S1 state and retrieves only snapshots with exact verified same-stream S0 provenance.

**Architecture:** Keep all telemetry-only types, record codes, validation, encoding, parsing, and verified-read logic under `codec::profiles::telemetry`. Store TPD1/TSS1 through existing raw record APIs and validate S1 lineage through the existing provenance/query substrate; generic record envelopes and stream timing remain authoritative, while H.3 retains physical sensor/calibration semantics.

**Tech Stack:** C++20, CMake 3.20+, existing `codec::Result`/CODA archive/provenance APIs, existing lightweight test harness, GCC/Clang, ASan/UBSan. No new library dependency.

**Spec:** `docs/superpowers/specs/2026-08-30-stage-h2-telemetry-profile-design.md`

## Global Constraints

- Stage G remains deferred; H.1 is complete and H.2 is the active Stage H dependency.
- Add no telemetry fields to generic core structures and no telemetry enumerators to generic `RecordType`.
- Reuse existing `StreamType::telemetry`; generic CODA record envelope time/clock/epoch/sequence remain temporal authority.
- Use profile-owned record codes `0x0110` for TPD1 and `0x0111` for TSS1; keep `0x0112`-`0x011f` unassigned.
- Metric names are 1-128 printable-ASCII bytes, units are 0-64 printable-ASCII bytes, and metric names must be strictly increasing by exact byte order.
- Scalar types are signed 64-bit, unsigned 64-bit, exact float64 raw bits, and boolean. Float bits are preserved without arithmetic canonicalization; booleans must be exactly 0 or 1.
- TPD1/TSS1 use big-endian integers, exact version/magic, zero reserved fields, checked lengths, and reject trailing bytes.
- Keep accepted source bytes S0. Return TSS1 as S1 only under the exact `codec.telemetry.sample.canonicalize` provenance contract with direct exact same-stream S0 input.
- Add no telemetry transport/parser/vendor integration, CLI, C ABI, model, aggregation, unit conversion, physical sensor calibration, benchmark, performance, quality, or scale claim.
- The explicit `CODEC_ENABLE_FFMPEG_VIDEO=OFF` CI/package path must continue to pass, proving H.2 has no FFmpeg dependency.

---

### Task 1: Deterministic telemetry descriptor and sample-state codec

**Files:**
- Create: `include/codec/profiles/telemetry.hpp`
- Create: `src/telemetry/sample_state.cpp`
- Create: `tests/test_telemetry_profile.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `codec::RecordTypeCode`, `codec::Result<T>`, `codec::ErrorCode`, `codec::RecordTimeRange`.
- Produces:
  - `telemetry_profile_descriptor_record_type == 0x0110`
  - `telemetry_sample_state_record_type == 0x0111`
  - `TelemetryScalarType`, `TelemetryValue`, `TelemetryMetricDescriptor`, `TelemetryProfileDescriptor`, `TelemetrySampleState`, `TelemetryDecodeLimits`
  - `encode_telemetry_profile_descriptor()`, `decode_telemetry_profile_descriptor()`
  - `encode_telemetry_sample_state()`, `decode_telemetry_sample_state()`

- [ ] **Step 1: Add the public declarations and failing tests**

Create the public header with this exact core surface:

```cpp
#pragma once

#include <codec/archive.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace codec::profiles::telemetry {

inline constexpr RecordTypeCode telemetry_profile_descriptor_record_type = 0x0110;
inline constexpr RecordTypeCode telemetry_sample_state_record_type = 0x0111;

enum class TelemetryScalarType : std::uint8_t {
  signed_integer = 1,
  unsigned_integer = 2,
  float64_bits = 3,
  boolean = 4,
};

struct TelemetryValue {
  TelemetryScalarType scalar_type{TelemetryScalarType::unsigned_integer};
  std::uint64_t raw_bits{};

  static TelemetryValue from_signed(std::int64_t value) noexcept;
  static TelemetryValue from_unsigned(std::uint64_t value) noexcept;
  static TelemetryValue from_float64_bits(std::uint64_t bits) noexcept;
  static TelemetryValue from_boolean(bool value) noexcept;
  bool operator==(const TelemetryValue&) const = default;
};

struct TelemetryMetricDescriptor {
  std::string name;
  std::string unit;
  TelemetryScalarType scalar_type{TelemetryScalarType::unsigned_integer};
  auto operator<=>(const TelemetryMetricDescriptor&) const = default;
};

struct TelemetryProfileDescriptor {
  std::vector<TelemetryMetricDescriptor> metrics;
  bool operator==(const TelemetryProfileDescriptor&) const = default;
};

struct TelemetrySampleState {
  TelemetryProfileDescriptor descriptor;
  std::vector<TelemetryValue> values;
  bool operator==(const TelemetrySampleState&) const = default;
};

struct TelemetryDecodeLimits {
  std::uint32_t maximum_metrics{1024};
  std::uint32_t maximum_metric_name_bytes{128};
  std::uint32_t maximum_unit_bytes{64};
  std::uint64_t maximum_descriptor_bytes{1024ULL * 1024ULL};
  std::uint64_t maximum_state_bytes{16ULL * 1024ULL * 1024ULL};
};

Result<std::vector<std::byte>> encode_telemetry_profile_descriptor(
    const TelemetryProfileDescriptor& descriptor);
Result<TelemetryProfileDescriptor> decode_telemetry_profile_descriptor(
    std::span<const std::byte> payload, TelemetryDecodeLimits limits = {});
Result<std::vector<std::byte>> encode_telemetry_sample_state(
    const TelemetrySampleState& state);
Result<TelemetrySampleState> decode_telemetry_sample_state(
    std::span<const std::byte> payload, TelemetryDecodeLimits limits = {});

}  // namespace codec::profiles::telemetry
```

Create `tests/test_telemetry_profile.cpp` with these tests:

```cpp
TEST(telemetry_profile_record_codes_are_profile_owned_and_stable)
TEST(telemetry_descriptor_encoding_matches_tpd1_golden_bytes)
TEST(telemetry_sample_encoding_matches_tss1_golden_bytes)
TEST(telemetry_sample_round_trips_all_scalar_types_exactly)
TEST(telemetry_profile_encoding_rejects_noncanonical_descriptors)
TEST(telemetry_profile_decoding_rejects_malformed_and_over_limit_payloads)
```

Use this canonical four-metric descriptor, already strictly sorted:

```cpp
TelemetryProfileDescriptor descriptor{{
    {.name = "cpu.busy", .unit = "percent", .scalar_type = TelemetryScalarType::float64_bits},
    {.name = "queue.depth", .unit = "items", .scalar_type = TelemetryScalarType::unsigned_integer},
    {.name = "service.ready", .unit = "", .scalar_type = TelemetryScalarType::boolean},
    {.name = "temperature.delta", .unit = "mK", .scalar_type = TelemetryScalarType::signed_integer},
}};
```

The TPD1 golden payload begins with:

```cpp
{
  std::byte{'T'}, std::byte{'P'}, std::byte{'D'}, std::byte{'1'},
  std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
  std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x04},
}
```

Each metric entry header is:

```text
scalar_type:u8, flags:u8=0, reserved:u16=0, name_length:u16-be, unit_length:u16-be
```

The TSS1 golden state uses values:

```cpp
{
  TelemetryValue::from_float64_bits(0x3ff8000000000000ULL), // exact 1.5 bits
  TelemetryValue::from_unsigned(42),
  TelemetryValue::from_boolean(true),
  TelemetryValue::from_signed(-7),
}
```

and begins with `TSS1`, version `1`, zero reserved bytes, the exact encoded TPD1 length, value count `4`, and value-byte length `32`.

- [ ] **Step 2: Wire RED into CMake and run it**

Add `src/telemetry/sample_state.cpp` to `codec_core` and `tests/test_telemetry_profile.cpp` to `codec_tests`. Initially create `sample_state.cpp` with only the include and namespace so the declared functions are unresolved.

Run through CI on the test-only/declaration head. Expected: compile/link failure only because telemetry codec functions are absent; existing tests should still compile until the missing telemetry symbols are linked.

- [ ] **Step 3: Implement validation and scalar constructors**

Private helpers in `sample_state.cpp` must include checked big-endian u16/u32/u64 put/get operations and:

```cpp
bool supported_scalar_type(TelemetryScalarType value) noexcept;
bool printable_ascii(std::string_view value, bool allow_empty) noexcept;
Result<void> validate_limits(const TelemetryDecodeLimits& limits);
Result<void> validate_descriptor(const TelemetryProfileDescriptor& descriptor,
                                 const TelemetryDecodeLimits& limits,
                                 ErrorCode malformed_code);
Result<void> validate_state(const TelemetrySampleState& state,
                            const TelemetryDecodeLimits& limits,
                            ErrorCode malformed_code);
```

`from_signed(value)` stores `static_cast<std::uint64_t>(value)`, which is the defined modulo-2^64 representation. Boolean stores exactly 0 or 1. Float constructor stores caller bits unchanged.

Descriptor validation must reject empty metric vectors, configured count excess, bad scalar types, non-printable/over-limit strings, duplicate names, and any `metrics[i-1].name >= metrics[i].name` under ordinary bytewise `std::string` comparison. State validation requires `values.size() == metrics.size()`, exact per-slot scalar-type equality, and canonical booleans.

- [ ] **Step 4: Implement exact TPD1/TSS1 encode/decode**

Constants:

```cpp
constexpr std::size_t kTpd1HeaderSize = 12;
constexpr std::size_t kMetricHeaderSize = 8;
constexpr std::size_t kTss1HeaderSize = 20;
constexpr std::size_t kScalarBytes = 8;
```

Encoding returns `invalid_argument` for caller semantic errors and `resource_exhausted` for checked size/limit excess. TPD1 computes total size before allocation, writes exact headers and strings, and never NUL-terminates strings.

TSS1 first obtains canonical TPD1 bytes, checks descriptor length fits u32, checks `value_count * 8` without overflow and fits u32, checks total state bytes against `maximum_state_bytes`, then writes each `raw_bits` as u64-be.

Decoding returns `decode` for malformed direct payloads and `resource_exhausted` for configured bounds. It must reject bad magic/version/reserved/flags, zero count, unsupported types, impossible lengths, noncanonical metric ordering, string violations, truncation, count mismatch, noncanonical boolean raw values, and trailing bytes before unsafe allocation/copy.

- [ ] **Step 5: Prove GREEN for the focused codec**

Run CI or local equivalent:

```bash
./build/codec_tests --include-prefix telemetry_profile_
```

Expected: all telemetry profile codec tests pass, including encode-decode-encode equality and exact NaN-payload raw bits.

- [ ] **Step 6: Commit the self-contained codec task**

Commit only the public header, codec source, focused tests, and CMake wiring with a message equivalent to `Add deterministic telemetry sample state`.

---

### Task 2: Provenance-verified telemetry S1 reader

**Files:**
- Modify: `include/codec/profiles/telemetry.hpp`
- Create: `src/telemetry/sample_state_reader.cpp`
- Create: `tests/test_telemetry_state_reader.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 TSS1 decoder plus existing `CodaArchive::query_records`, `extract_records`, `query_stream_provenance`, exact `RecordInfo` links, `TruthClass::state_exact`.
- Produces:

```cpp
struct TelemetrySampleQuery {
  std::optional<StreamId> stream;
  std::optional<RecordTimeRange> time;
  std::size_t maximum_results{4096};
  std::uint64_t maximum_encoded_bytes{64ULL * 1024ULL * 1024ULL};
  TelemetryDecodeLimits decode_limits{};
};

struct VerifiedTelemetrySample {
  TelemetrySampleState state;
  RecordInfo state_record;
  std::vector<RecordInfo> source_records;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedTelemetrySample>> query_verified_telemetry_samples(
    const CodaArchive& archive,
    const TelemetrySampleQuery& query = {});
```

- [ ] **Step 1: Add reader declarations and RED tests**

Create tests named:

```cpp
TEST(telemetry_state_reader_returns_exact_verified_s1)
TEST(telemetry_state_reader_ignores_unprovenanced_tss1)
TEST(telemetry_state_reader_ignores_non_s1_tss1)
TEST(telemetry_state_reader_rejects_wrong_process_contract)
TEST(telemetry_state_reader_rejects_wrong_process_details_version)
TEST(telemetry_state_reader_rejects_cross_stream_or_non_source_inputs)
TEST(telemetry_state_reader_accepts_multiple_exact_s0_inputs)
TEST(telemetry_state_reader_rejects_malformed_tss1_as_archive_corrupt)
TEST(telemetry_state_reader_enforces_result_byte_and_decode_limits)
TEST(telemetry_state_reader_preserves_unknown_future_profile_records)
```

Use the exact canonical process fixture:

```cpp
ProvenanceProcess{
  .operation = "codec.telemetry.sample.canonicalize",
  .implementation_id = "codec.telemetry",
  .implementation_version = "1",
  .implementation_hash = std::nullopt,
  .configuration_hash = std::nullopt,
  .created_utc_ns = 0,
  .details_type = "application/vnd.codec.telemetry.canonicalization.v1",
  .details = {std::byte{0x01}},
};
```

The archive fixture must append a `StreamType::telemetry` descriptor, at least one `RecordType::source_bytes` S0 record, one raw TSS1 record at `0x0111`, and matching provenance. Unknown future code fixture uses `0x011f`, verifies the archive, raw-extracts exact bytes, repairs a deliberately damaged tail into a new archive, and rechecks exact future-code bytes.

- [ ] **Step 2: Wire the reader test/source and prove RED**

Add `src/telemetry/sample_state_reader.cpp` and `tests/test_telemetry_state_reader.cpp` to CMake, leaving the query function unresolved. CI should fail only on the absent reader implementation.

- [ ] **Step 3: Implement query validation and candidate extraction**

Follow the existing H.1 reader structure without moving helpers into core. Validate non-zero result/byte/decode limits before scanning. Query exactly raw type `0x0111`, optional stream, optional time. Enforce `maximum_results` and aggregate encoded TSS1 bytes without silent truncation.

For every selected TSS1 record, exact-extract its payload and decode it. Convert a direct decode `decode` error on committed selected TSS1 into non-retryable `archive_corrupt`; preserve `resource_exhausted` for configured limit failures.

- [ ] **Step 4: Implement exact provenance gate**

For each canonical candidate:

1. query provenance whose subject is exactly the state record;
2. ignore the candidate when no provenance exists;
3. ignore provenance entries whose truth class is not `state_exact` only when there is no contradictory S1 claim; multiple/contradictory S1 sidecars fail closed;
4. require exactly one matching `state_exact` sidecar;
5. require exact operation/implementation/version/details-type/details-byte contract;
6. resolve every subject/input link to an exact committed record, comparing all `RecordInfo` identity fields used by the existing provenance implementation;
7. require at least one input `RecordType::source_bytes`, same stream as the state, and not the state itself;
8. reject dangling, self, cross-stream, or non-S0-only input sets as `archive_corrupt`;
9. return all exact S0 `RecordInfo` inputs in provenance order.

- [ ] **Step 5: Prove reader GREEN**

Run:

```bash
./build/codec_tests --include-prefix telemetry_state_reader_
```

Expected: all ten reader tests pass. Run existing `video_state_reader_` tests in the same build to prove the profile-local reader did not alter H.1 behavior.

- [ ] **Step 6: Commit the verified-reader task**

Commit only the reader API/source/tests/CMake delta with a message equivalent to `Add verified telemetry state reader`.

---

### Task 3: Installed-package integration and documentation contract

**Files:**
- Modify: `tests/package_consumer/main.cpp`
- Modify: `tests/ai_contract.cmake`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `AI_WORKSHEET.md`

**Interfaces:**
- Consumes: Tasks 1-2 installed public telemetry API.
- Produces: external-package proof and repository cold-start documentation for H.2.

- [ ] **Step 1: Add package-consumer telemetry proof**

In the installed consumer, include:

```cpp
#include <codec/profiles/telemetry.hpp>
```

Construct a two-metric descriptor with `queue.depth` unsigned and `service.ready` boolean, construct corresponding values, encode TSS1, create a temporary CODA archive, append a telemetry stream descriptor, exact source S0, raw TSS1, and this exact process:

```cpp
ProvenanceProcess{
  .operation = "codec.telemetry.sample.canonicalize",
  .implementation_id = "codec.telemetry",
  .implementation_version = "1",
  .created_utc_ns = 0,
  .details_type = "application/vnd.codec.telemetry.canonicalization.v1",
  .details = {std::byte{0x01}},
};
```

Finalize/reopen/query through `query_verified_telemetry_samples()` and fail the consumer executable unless exactly one state with exact descriptor/value equality is returned.

- [ ] **Step 2: Extend AI contract without weakening existing profile checks**

Have `tests/ai_contract.cmake` read `include/codec/profiles/telemetry.hpp` and require these stable strings across header/README:

```text
telemetry_profile_descriptor_record_type = 0x0110
telemetry_sample_state_record_type = 0x0111
query_verified_telemetry_samples
Telemetry Stream Profile — Stage H.2
Stage G trust/selective-disclosure work is explicitly deferred
```

Build a telemetry-foundation source set from the telemetry header/source files and reject FFmpeg/GStreamer symbols exactly as H.1 does. Do not remove or relax H.1 checks.

- [ ] **Step 3: Synchronize README/CHANGELOG/worksheet**

README must describe H.2 only as:

- dependency-free metric descriptor/sample-state schema;
- exact signed/unsigned/float-bit/boolean scalar slots;
- generic envelope time as temporal authority;
- provenance-verified TSS1 S1;
- no telemetry transport, labels/histograms, aggregation, sensor calibration, CLI, model, or performance claim.

CHANGELOG gets one Unreleased H.2 foundation bullet. `AI_WORKSHEET.md` records base `f68515068...`, active H.2 stage, touched `[S0, S1]`, exact record codes/process contract, and next dependency H.3.

- [ ] **Step 4: Run package/AI/compatibility gate**

Required results:

```text
codec-unit: pass
codec-c-api: pass
codec-cli-integration: pass
codec-ai-contract: pass
installed package consumer: pass
ffmpeg-disabled installed package consumer: pass
```

- [ ] **Step 5: Commit packaging/docs task**

Use a focused commit equivalent to `Document Stage H.2 telemetry profile`.

---

### Task 4: Final verification, review, roadmap evidence, and guarded merge

**Files:**
- No production files unless verification finds a defect.
- Update roadmap issue #10 with planned/completed evidence as repository metadata.

**Interfaces:**
- Consumes: complete H.2 branch.
- Produces: reviewed, merged, main-CI-verified H.2 milestone.

- [ ] **Step 1: Open focused PR from `codex/stage-h2-telemetry-profile` to `main`**

PR body must list exact base/head, TPD1/TSS1 record codes, S0/S1 effects, process contract, package proof, explicit H.3/sensor non-goals, and TDD RED/GREEN run IDs.

- [ ] **Step 2: Run exact-head matrix**

Require completed/success for:

```text
build (gcc): configure, build, full tests, install, package consumer
build (clang): configure, build, full tests, install, package consumer
sanitizers: configure, build, full tests
ffmpeg-disabled: configure OFF, build, full tests, install, package consumer
```

- [ ] **Step 3: Review changed-file patches**

Audit especially:

- every multiplication/addition before allocation/slicing;
- strict metric ordering and duplicate rejection;
- exact float-bit and boolean behavior;
- decode error-code conversion at the verified-reader boundary;
- provenance same-stream/S0/direct-input checks;
- unknown `0x011f` preservation;
- no telemetry dependency added to core/video/CLI/C ABI;
- no accidental FFmpeg dependency in telemetry/package paths.

Fix every Critical or Important finding and rerun exact-head CI.

- [ ] **Step 4: Merge with an expected-head guard**

Only after exact-head CI and review are green, merge using the exact verified head SHA. Abort if the PR head moved or mergeability changed.

- [ ] **Step 5: Verify merged `main`**

Confirm `main` points at the merge commit and its push-triggered CI passes all four jobs. Add a completion entry to roadmap issue #10 recording branch, PR, final feature head, merge SHA, CI run IDs, touched truth classes, non-claims, and next dependency:

```text
H.3 — Sensor Stream Profile foundation
```

No H.3 implementation begins until H.2 merge/main evidence is complete.

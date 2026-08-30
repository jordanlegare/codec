# Stage H.2 Telemetry Stream Profile Foundation Design

Date: 2026-08-30  
Repository: `jordanlegare/codec`  
Status: Approved by delegated roadmap direction  
Base: `main` at `f68515068a022ef4f16eefdc1df0512b94bcec77`

## Decision

Stage G remains deferred, not completed or deleted. Stage H.1 is merged and verified. H.2 is the next bounded Stage H slice:

> Add a dependency-free Telemetry Stream Profile foundation with deterministic metric-set metadata, exact scalar snapshot S1 encoding, strict bounded decoding, exact provenance verification, and installed-package integration on the existing generic CODEC/CODA substrate.

H.2 proves a second non-audio/non-video vertical without changing generic truth, identity, archive, query, time, provenance, transport, recovery, distributed, CLI, or C ABI semantics.

## Goals

1. Represent named machine/application telemetry metrics without introducing telemetry fields into generic core structures.
2. Preserve accepted source bytes as S0 through existing generic archive APIs.
3. Define a canonical deterministic S1 snapshot containing a complete metric descriptor and one exact scalar value per metric.
4. Keep telemetry temporal authority in the generic record envelope and generic clock/epoch records rather than adding a second timestamp system inside the profile payload.
5. Verify S1 snapshots only when exact `state_exact` provenance resolves to direct same-stream S0 inputs under an exact versioned process contract.
6. Preserve unknown/newer telemetry profile records through generic raw-code verification, extraction, and repair without interpretation.
7. Install and test the profile with no new third-party dependency.

## Boundary from H.3 Sensor Profile

H.2 is for machine/application telemetry such as process counters, runtime gauges, queue depths, service status, or device-reported operational metrics where the source already defines a scalar metric identity and value.

H.2 deliberately does **not** define physical-sensor semantics such as calibration curves, coordinate frames, sensor axes, spatial pose, accuracy models, physical uncertainty, sampling geometry, ADC characteristics, or hardware-device calibration. Those belong to H.3 Sensor Stream Profile.

A telemetry metric may carry an opaque printable-ASCII unit string, but H.2 does not standardize unit ontologies or claim physical metrology semantics.

## Non-goals

H.2 does not add:

- a telemetry transport, scraper, exporter, OpenTelemetry/Prometheus/StatsD parser, SNMP client, or vendor integration;
- labels/tags/dimensions, histograms, summaries, exemplars, traces, logs, or event documents;
- counter-reset/rate semantics or aggregation rules;
- missing-value interpolation, resampling, alignment, smoothing, normalization, or unit conversion;
- physical sensor calibration or uncertainty semantics;
- a CLI or C ABI surface;
- a model, model bundle, inference path, anomaly detector, or quality claim;
- changes to CODA headers/envelopes, `RecordType`, `StreamType`, S0/S1/D definitions, generic query/provenance APIs, or generic time semantics;
- completion of H.3-H.6 or Stage G.

## Architectural boundary

The implementation lives under `codec::profiles::telemetry` and uses the stable substrate unchanged:

- `StreamDescriptor` identifies the logical stream with existing `StreamType::telemetry`.
- `CodaWriter::append_raw` stores telemetry profile records under profile-owned raw type codes.
- `RecordQuery` and `CodaArchive::extract_records` retrieve them without core interpretation.
- `CodaWriter::append_stream_provenance` binds S1 sample records to exact supporting records.
- generic archive verification, repair, raw-code extraction, stream identity, record envelope time, continuity, clock, sequence, and format epochs remain authoritative.

No telemetry-only field is added to a generic CODEC structure.

## Profile-owned record types

H.1 uses `0x0100`/`0x0101`. H.2 reserves a separate small block so later video additions do not collide with telemetry:

```cpp
inline constexpr RecordTypeCode telemetry_profile_descriptor_record_type = 0x0110;
inline constexpr RecordTypeCode telemetry_sample_state_record_type = 0x0111;
```

These are profile constants, not additions to `RecordType`. Codes `0x0112`-`0x011f` remain unassigned for later telemetry-profile evolution. Existing registered codes and compatibility tombstones remain unchanged.

## Public API

The umbrella header is `<codec/profiles/telemetry.hpp>`.

### Scalar vocabulary

```cpp
namespace codec::profiles::telemetry {

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

}
```

`raw_bits` is an exact canonical scalar representation rather than an arithmetic claim:

- `signed_integer`: mathematical 64-bit two's-complement representation; `from_signed` uses modulo-2^64 conversion.
- `unsigned_integer`: ordinary unsigned 64-bit representation.
- `float64_bits`: caller-supplied IEEE-754 binary64 bit pattern stored exactly. H.2 accepts every 64-bit pattern, including infinities and NaN payloads, and does not canonicalize or evaluate them.
- `boolean`: only `raw_bits == 0` or `raw_bits == 1` is canonical.

Using explicit float bits avoids silently changing NaN payloads or depending on host floating-point formatting/arithmetic during archive canonicalization.

### Metric descriptor

```cpp
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
```

Canonical descriptor rules:

- at least one metric;
- metric names are 1-128 bytes of printable ASCII (`0x20`-`0x7e`);
- unit strings are 0-64 bytes of printable ASCII;
- metrics are strictly increasing by exact unsigned-byte lexicographic `name` order;
- duplicate names and caller-supplied non-canonical ordering are rejected rather than silently reordered;
- every scalar type must be one of the four v1 values.

The strict ordering gives one canonical descriptor byte representation for one metric set and makes value-slot identity deterministic.

### Sample S1 state

```cpp
struct TelemetrySampleState {
  TelemetryProfileDescriptor descriptor;
  std::vector<TelemetryValue> values;
  bool operator==(const TelemetrySampleState&) const = default;
};

Result<std::vector<std::byte>> encode_telemetry_profile_descriptor(
    const TelemetryProfileDescriptor& descriptor);
Result<TelemetryProfileDescriptor> decode_telemetry_profile_descriptor(
    std::span<const std::byte> payload,
    TelemetryDecodeLimits limits = {});
Result<std::vector<std::byte>> encode_telemetry_sample_state(
    const TelemetrySampleState& state);
Result<TelemetrySampleState> decode_telemetry_sample_state(
    std::span<const std::byte> payload,
    TelemetryDecodeLimits limits = {});
```

A sample contains exactly one value per descriptor metric in descriptor order. The value's `scalar_type` must exactly equal the corresponding metric's type. No value timestamp exists inside TSS1; the CODA record envelope interval and generic stream timing metadata remain temporal authority. If source observations are not one exact snapshot, callers must archive separate states/intervals rather than invent profile-local timing.

### Decode limits

```cpp
struct TelemetryDecodeLimits {
  std::uint32_t maximum_metrics{1024};
  std::uint32_t maximum_metric_name_bytes{128};
  std::uint32_t maximum_unit_bytes{64};
  std::uint64_t maximum_descriptor_bytes{1024ULL * 1024ULL};
  std::uint64_t maximum_state_bytes{16ULL * 1024ULL * 1024ULL};
};
```

Every limit must be non-zero except that a unit string itself may be empty. All length additions and count multiplications are checked before slicing or allocation.

## Binary schemas

All multibyte integers are big-endian. Exact v1 lengths are required; reserved bytes must be zero; trailing bytes are rejected.

### TPD1 — Telemetry Profile Descriptor v1

Header, 12 bytes:

- magic: four bytes `TPD1`;
- schema version: one byte `1`;
- three reserved zero bytes;
- metric count: unsigned 32-bit.

Then exactly `metric_count` metric entries. Each entry begins with an 8-byte header:

- scalar type: one byte;
- flags: one byte, required `0` in v1;
- two reserved bytes, required `0`;
- name length: unsigned 16-bit;
- unit length: unsigned 16-bit;

The header is followed immediately by exact name bytes and exact unit bytes. Names/units are not NUL terminated. The complete descriptor must fit configured descriptor bounds. The decoder validates canonical strict name ordering while parsing.

### TSS1 — Telemetry Sample State v1

Header, 20 bytes:

- magic: four bytes `TSS1`;
- schema version: one byte `1`;
- three reserved zero bytes;
- embedded descriptor length: unsigned 32-bit;
- value count: unsigned 32-bit;
- value byte length: unsigned 32-bit.

Then:

1. exactly one canonical TPD1 payload;
2. exactly `value_count` 8-byte raw values in descriptor order.

Canonical rules:

- embedded descriptor length must equal the actual complete TPD1 size and fit configured descriptor bounds;
- `value_count` must equal the descriptor metric count;
- `value_byte_length` must equal `value_count * 8` exactly;
- each value is one unsigned 64-bit big-endian raw bit pattern;
- boolean slots must contain only 0 or 1;
- the complete TSS1 payload must fit configured state bounds;
- no trailing bytes are accepted.

The descriptor supplies each slot's scalar type, so no redundant per-value type byte appears in the wire payload. The decoded `TelemetryValue.scalar_type` is reconstructed from the corresponding descriptor entry.

## Truth and provenance

- Accepted telemetry wire/file/vendor/source bytes remain S0.
- TPD1 is profile metadata and does not replace S0.
- TSS1 is S1 only when archived with `TruthClass::state_exact` provenance satisfying the H.2 process contract.
- A canonical TSS1 raw record without valid S1 provenance is not upgraded by the verified reader.
- Unit conversion, rate calculation, aggregation, interpolation, anomaly scoring, inference, summarization, or cross-stream joining is D unless a later approved exact-state contract proves otherwise.

Canonicalization process contract:

```text
operation: codec.telemetry.sample.canonicalize
implementation_id: codec.telemetry
implementation_version: 1
details_type: application/vnd.codec.telemetry.canonicalization.v1
details: exactly one byte 0x01
```

The verified reader requires at least one direct S0 input from the same stream. Multiple exact S0 inputs are allowed. Every provenance link must resolve to the exact committed record. The state record may not be its own source.

## Verified reader

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

The reader returns a sample only when:

1. the selected record type is exactly `telemetry_sample_state_record_type`;
2. the TSS1 payload is canonical and within configured bounds;
3. exactly one matching S1 provenance sidecar resolves to the state record;
4. every provenance subject/input link resolves to an exact committed record;
5. at least one direct input is a same-stream S0 record and is not the state record itself;
6. truth class is exactly `state_exact`;
7. process operation, implementation ID/version, details type, and exact one-byte `0x01` details match the H.2 contract;
8. per-query result and aggregate encoded-byte bounds are not exceeded.

As with H.1, a raw profile record that lacks provenance is ignored as unclassified rather than treated as S1. Contradictory or malformed provenance for a selected candidate fails closed as archive corruption.

## Error behavior

- Encoding invalid caller-owned descriptors/states returns `invalid_argument`.
- Decode/query of malformed committed telemetry profile payloads returns `archive_corrupt`.
- Configured bounds exceeded before or during parsing/allocation return `resource_exhausted`.
- Missing provenance for an otherwise canonical raw TSS1 record means it is unclassified and is omitted.
- Duplicate/contradictory provenance, dangling links, wrong process contracts, cross-stream S0 inputs, or malformed selected state bytes fail closed as `archive_corrupt`.

No profile error mutates the archive or weakens generic verification, repair, or raw extraction.

## File/build layout

New production files:

- `include/codec/profiles/telemetry.hpp` — public profile types, limits, codec, and verified-reader API.
- `src/telemetry/sample_state.cpp` — TPD1/TSS1 validation, deterministic encode/decode, scalar helpers.
- `src/telemetry/sample_state_reader.cpp` — verified S1 query/provenance boundary.

New tests:

- `tests/test_telemetry_profile.cpp` — golden bytes, scalar/layout round trips, canonical validation, malformed/bounded decode.
- `tests/test_telemetry_state_reader.cpp` — exact provenance filtering/fail-closed behavior and unknown-code preservation.

Existing files modified only as required:

- `CMakeLists.txt` — compile sources/tests.
- `tests/package_consumer/main.cpp` — prove installed-header encode/archive/provenance/query use.
- `README.md`, `CHANGELOG.md`, `AI_WORKSHEET.md`, `tests/ai_contract.cmake` — synchronize proven profile scope and cold-start contract.

No CLI, C ABI, generic archive/core/query/provenance, transport, recovery, distributed, audio, or video production source changes are planned.

## Test contract

### Determinism/exactness

- fixed TPD1 golden bytes with at least two strictly ordered metrics;
- fixed TSS1 golden bytes covering signed, unsigned, float64 raw bits, and boolean values;
- encode-decode-encode equality;
- exact preservation of all 64 float-bit patterns used by tests, including a NaN payload fixture;
- signed negative value represented by defined modulo-2^64 raw bits;
- repeated encoding produces identical bytes.

### Validation/failure paths

- zero/over-limit metric count;
- empty/over-limit/non-printable metric names;
- over-limit/non-printable units;
- duplicate or non-canonical metric name ordering;
- unknown scalar type;
- non-zero flags/reserved bytes;
- wrong magic/version/count/declared lengths;
- descriptor/state truncation and trailing bytes;
- value count/type mismatch;
- non-canonical boolean value;
- multiplication/addition and configured resource-limit exhaustion.

### Provenance reader

- exact verified S1 sample is returned;
- unprovenanced canonical TSS1 is ignored;
- wrong truth class is ignored or rejected consistently with the existing H.1 reader contract;
- wrong operation/implementation/version/details are rejected;
- dangling, self, cross-stream, or non-S0-only inputs are rejected;
- multiple exact same-stream S0 inputs are accepted;
- malformed selected TSS1 is `archive_corrupt`;
- query result/byte/decode limits are enforced.

### Preservation/compatibility

- telemetry profile decode failure does not prevent generic archive verification;
- TPD1/TSS1 remain exact through raw extraction;
- an unknown future `0x011f` profile record survives verify and non-mutating repair byte-exactly;
- existing H.1 video, audio, CLI, C ABI, transport, distributed, and archive tests remain unchanged/green.

### Packaging

An installed external consumer must include `<codec/profiles/telemetry.hpp>`, construct and encode a descriptor/sample, append source S0 and raw TSS1, attach the exact H.2 `state_exact` provenance contract, reopen the archive, and retrieve the sample through `query_verified_telemetry_samples()`.

## CI and exit criteria

H.2 is complete only when the exact PR head passes:

1. GCC warnings-as-errors build, full CTest, install, and package consumer;
2. Clang warnings-as-errors build, full CTest, install, and package consumer;
3. ASan/UBSan build and tests;
4. the explicit FFmpeg-disabled build and package consumer, proving H.2 has no FFmpeg dependency;
5. repository AI contract, CLI integration, and C ABI tests;
6. focused review with no unresolved Critical/Important findings;
7. guarded merge and green `main` push CI.

No benchmark is required because H.2 makes no throughput, latency, cardinality-scale, compression, quality, or deployment claim.

## Deferred follow-on milestones

After H.2:

- H.3 — Sensor Stream Profile foundation;
- H.4 — Document/Event Stream Profile foundation;
- H.5 — Network/System Stream Profile foundation;
- H.6 — domain-specific schema/model-bundle contracts proven by at least two verticals;
- H.7+ — concrete telemetry/video/device/vendor/transport/model/deployment integrations selected from demonstrated use cases.

No later profile may fork or weaken generic core truth, identity, time, query, provenance, archive, transport, or distributed semantics.

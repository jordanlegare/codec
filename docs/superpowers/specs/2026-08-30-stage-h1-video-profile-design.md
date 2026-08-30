# Stage H.1 Video Stream Profile Foundation Design

Date: 2026-08-30  
Repository: `jordanlegare/codec`  
Status: Approved  
Base: `main` at `2fa8da9fab514d77aa525be0cc6ed940e6569d67`

## Decision

Stage G is deferred, not completed or deleted. Stage H becomes the active roadmap stage.

H.1 is the first bounded Stage H slice:

> Add a dependency-free Video Stream Profile foundation with versioned profile metadata, deterministic raw-frame S1 encoding, exact provenance verification, and installed-package integration on the existing generic CODEC/CODA substrate.

Later Stage H milestones will address telemetry, sensor, document/event, network/system, domain-specific schemas, model bundles, and concrete integrations. H.1 does not bundle those independent verticals into one change.

## Goals

1. Prove that a non-audio vertical can add typed semantics without changing generic stream, archive, query, provenance, transport, or distributed semantics.
2. Preserve accepted encoded/container/source bytes as S0 through existing generic archive APIs.
3. Define a bounded, deterministic raw-frame S1 representation for explicitly supported pixel layouts.
4. Make every verified S1 frame traceable to exact supporting S0 records and the exact video profile descriptor used to interpret it.
5. Keep unknown or newer profile records raw-preservable and raw-extractable.
6. Install and test the new public API without introducing FFmpeg or another media dependency.

## Non-goals

H.1 does not add:

- container demultiplexing or encoded-video parsing;
- H.264, H.265, AV1, VP9, MPEG, or another codec implementation;
- FFmpeg, GStreamer, libavcodec, or a playback integration;
- a new CLI command;
- video export or transcoding;
- streaming video inference;
- a production model, model bundle, or model-quality claim;
- automatic proof that caller-supplied decoded pixels are semantically equivalent to an encoded source;
- a generic plugin registry;
- changes to the CODA header, record envelope, S0/S1/D meanings, or generic `RecordType` enumeration.

## Architectural boundary

The implementation lives under `codec::profiles::video`. It uses the stable substrate as follows:

- `StreamDescriptor` identifies the logical stream with `StreamType::video`.
- `CodaWriter::append_raw` stores profile records under profile-owned type codes.
- `RecordQuery` and `CodaArchive::extract_records` retrieve those records without core interpretation.
- `CodaWriter::append_stream_provenance` binds S1 frame records to exact inputs.
- existing archive verification, unknown-type preservation, repair, and raw extraction remain authoritative.
- generic envelope time, sequence, stream ID, and continuity/epoch records remain the temporal authority.

No profile-only field is added to a generic CODEC structure.

## Profile-owned record types

The Video Profile owns two development-profile type codes:

```cpp
inline constexpr RecordTypeCode video_profile_descriptor_record_type = 0x0100;
inline constexpr RecordTypeCode raw_video_frame_state_record_type = 0x0101;
```

They are constants in the video profile header, not additions to `RecordType`. This deliberately exercises the existing raw-code compatibility boundary and prevents video semantics from expanding core archive vocabulary.

Codes 20 and 21 remain retired compatibility tombstones and are not reused. Existing type codes remain unchanged.

## Public API

The umbrella header is `<codec/profiles/video.hpp>`.

### Descriptor types

```cpp
namespace codec::profiles::video {

enum class PixelLayout : std::uint8_t {
  gray8 = 1,
  rgb24 = 2,
  rgba32 = 3,
  yuv420p8 = 4,
};

enum class ColorRange : std::uint8_t {
  unspecified = 0,
  limited = 1,
  full = 2,
};

enum class ColorPrimaries : std::uint8_t {
  unspecified = 0,
  bt709 = 1,
  bt2020 = 2,
};

enum class TransferCharacteristics : std::uint8_t {
  unspecified = 0,
  linear = 1,
  srgb = 2,
  bt709 = 3,
  pq = 4,
  hlg = 5,
};

enum class MatrixCoefficients : std::uint8_t {
  unspecified = 0,
  identity = 1,
  bt709 = 2,
  bt2020_ncl = 3,
};

struct VideoProfileDescriptor {
  std::uint32_t coded_width{};
  std::uint32_t coded_height{};
  PixelLayout pixel_layout{PixelLayout::gray8};
  std::uint32_t sample_aspect_ratio_numerator{1};
  std::uint32_t sample_aspect_ratio_denominator{1};
  std::uint32_t nominal_frame_rate_numerator{};
  std::uint32_t nominal_frame_rate_denominator{1};
  ColorRange color_range{ColorRange::unspecified};
  ColorPrimaries color_primaries{ColorPrimaries::unspecified};
  TransferCharacteristics transfer{TransferCharacteristics::unspecified};
  MatrixCoefficients matrix{MatrixCoefficients::unspecified};
};

Result<std::vector<std::byte>> encode_video_profile_descriptor(
    const VideoProfileDescriptor& descriptor);
Result<VideoProfileDescriptor> decode_video_profile_descriptor(
    std::span<const std::byte> payload,
    VideoDecodeLimits limits = {});

}
```

A zero nominal frame-rate numerator means variable or unspecified frame rate. Denominators must be non-zero. YUV420P8 requires even coded dimensions. All dimensions and derived sizes are checked before allocation or multiplication.

### Raw-frame state

```cpp
struct RawVideoFrameState {
  VideoProfileDescriptor descriptor;
  std::vector<std::byte> pixels;
};

Result<std::vector<std::byte>> encode_raw_video_frame_state(
    const RawVideoFrameState& frame);
Result<RawVideoFrameState> decode_raw_video_frame_state(
    std::span<const std::byte> payload,
    VideoDecodeLimits limits = {});
```

The canonical payload has no row padding or caller stride:

| Layout | Canonical planes |
|---|---|
| Gray8 | one plane, `width * height` bytes |
| RGB24 | one interleaved plane, `width * height * 3` bytes |
| RGBA32 | one interleaved plane, `width * height * 4` bytes |
| YUV420P8 | Y plane then U then V; `width * height + 2 * (width/2) * (height/2)` bytes |

The encoded frame repeats its complete descriptor so one state record is independently interpretable and format changes do not depend on ambient mutable state. The generic record envelope carries stream identity and time. Generic continuity records carry format epochs.

The default decode limits are conservative and caller-overridable:

```cpp
struct VideoDecodeLimits {
  std::uint32_t maximum_width{16384};
  std::uint32_t maximum_height{16384};
  std::uint64_t maximum_pixels{268435456};
  std::uint64_t maximum_payload_bytes{1024ULL * 1024ULL * 1024ULL};
};
```

### Verified reader

```cpp
struct VideoFrameQuery {
  std::optional<StreamId> stream;
  std::optional<RecordTimeRange> time;
  std::size_t maximum_results{1024};
  std::uint64_t maximum_encoded_bytes{1024ULL * 1024ULL * 1024ULL};
  VideoDecodeLimits decode_limits{};
};

struct VerifiedRawVideoFrame {
  RawVideoFrameState state;
  RecordInfo state_record;
  std::vector<RecordInfo> source_records;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedRawVideoFrame>> query_verified_raw_video_frames(
    const CodaArchive& archive, const VideoFrameQuery& query = {});
```

The reader returns a frame only when all of these conditions hold:

1. the selected record type is exactly `raw_video_frame_state_record_type`;
2. its VFR1 payload is canonical and within bounds;
3. exactly one matching S1 provenance sidecar resolves to that state record;
4. every provenance link resolves to an exact committed record;
5. at least one direct input is an S0 record that is not the state record itself;
6. the process operation and details identify the Video Profile canonicalization contract;
7. result and aggregate encoded-byte limits are not exceeded.

Multiple exact S0 inputs are allowed for frames that depend on more than one source fragment. The reader does not claim that a decoder was deterministic or correct; it verifies the exact archived S1 state, its canonical form, and its provenance links.

## Binary schemas

### VPD1

`VPD1` is a fixed-size 36-byte, big-endian descriptor payload:

- four-byte magic `VPD1`;
- one-byte schema version, initially 1;
- one-byte pixel layout;
- one-byte color range;
- one-byte color primaries;
- one-byte transfer characteristics;
- one-byte matrix coefficients;
- two reserved bytes required to be zero;
- coded width and height;
- sample-aspect numerator and denominator;
- nominal-frame-rate numerator and denominator.

All integer fields are unsigned and encoded big-endian. The decoder requires the exact v1 length and rejects non-zero reserved bytes or trailing data.

### VFR1

`VFR1` is:

- four-byte magic `VFR1`;
- one-byte schema version, initially 1;
- three reserved bytes required to be zero;
- one unsigned 32-bit big-endian descriptor length;
- one unsigned 64-bit big-endian pixel length;
- exactly one canonical VPD1 descriptor;
- exactly the canonical pixel bytes.

The decoder checks all additions and multiplications before slicing or allocating, requires the embedded descriptor length to equal the v1 VPD1 size, requires the pixel length to match the layout formula exactly, and rejects trailing bytes.

Unknown magic, version, enum values, non-zero reserved fields, impossible geometry, zero denominators, length mismatch, overflow, truncation, or trailing bytes are errors. The physical archive record remains verifiable and raw-extractable even when profile decoding fails.

## Truth and provenance

- Encoded/container/source bytes accepted by the caller remain S0.
- A canonical VPD1 descriptor is profile metadata and does not replace S0 media bytes.
- VFR1 is S1 only when archived with `TruthClass::state_exact` provenance.
- A VFR1 record without valid S1 provenance remains a raw profile record; the verified reader does not upgrade it.
- Any interpolation, scaling, colorspace conversion, denoising, enhancement, recognition, detection, embedding, summarization, or generation is D unless a later approved profile contract proves a different exact-state rule.
- D artifacts never replace S0 or VFR1 S1 records.

The canonicalization operation identifier is versioned and stable, for example:

```text
operation: codec.video.raw-frame.canonicalize
implementation_id: codec.video
implementation_version: 1
details_type: application/vnd.codec.video.canonicalization.v1
```

## Error behavior

Encoding caller-owned invalid values returns `invalid_argument`. Decode/query errors from malformed committed profile payloads return `archive_corrupt`. Bounds exceeded before or during allocation return `resource_exhausted`. Missing or inconsistent provenance returns `archive_corrupt`.

Every parser is fail-closed, bounded, and exact-length. No profile error mutates the archive or prevents generic verification, repair, or raw extraction of already committed records.

## File and build layout

New production files:

- `include/codec/profiles/video.hpp`
- `include/codec/profiles/video_state_reader.hpp`
- `src/video/frame_state.cpp`
- `src/video/frame_state_reader.cpp`

New tests:

- `tests/test_video_profile.cpp`
- `tests/test_video_state_reader.cpp`

Existing files updated only as required:

- `CMakeLists.txt`
- `tests/package_consumer/CMakeLists.txt`
- `tests/package_consumer/main.cpp`
- `README.md`
- `CHANGELOG.md`
- `AI_WORKSHEET.md`

No CLI source, archive envelope implementation, transport, recovery, distributed, audio, or C ABI source file is changed.

## Test contract

### Determinism and exactness

- fixed golden VPD1 and VFR1 byte fixtures;
- encode-decode-encode equality;
- exact pixel round trips for Gray8, RGB24, RGBA32, and YUV420P8;
- repeated encoding produces identical bytes;
- format changes remain independently decodable and align with generic format epochs.

### Validation and failure paths

- zero or excessive dimensions;
- zero rational denominators;
- odd YUV420P8 dimensions;
- arithmetic overflow and configured resource limits;
- unknown enum values and versions;
- non-zero reserved bytes;
- wrong declared lengths;
- truncation and trailing bytes;
- absent, duplicate, wrong-truth, dangling, or self-referential provenance;
- aggregate query limit exhaustion.

### Preservation and compatibility

- profile decode failure does not prevent archive verification;
- VPD1/VFR1 records remain selectable and byte-exact through raw queries;
- unknown future profile code/version records survive verification and non-mutating repair;
- existing registered record codes and numeric tombstones remain unchanged;
- existing C ABI and CLI tests remain green.

### Packaging

An installed external consumer must include `<codec/profiles/video.hpp>`, encode/decode a frame, write it with `append_raw`, attach S1 provenance, reopen the archive, and retrieve it through the verified reader.

### CI

The exact PR head must pass:

- GCC warnings-as-errors build, full CTest, install, and package consumer;
- Clang warnings-as-errors build, full CTest, install, and package consumer;
- ASan/UBSan build and tests;
- repository AI contract;
- unchanged CLI integration and C ABI tests.

No benchmark is required because H.1 makes no throughput, latency, compression, scale, or quality claim.

## Documentation and roadmap state

The roadmap execution log receives a planned-work entry stating:

- Stage G is deferred by explicit user direction, with no trust capability claimed complete;
- Stage H is active;
- H.1 is the Video Stream Profile foundation described here;
- H.2 and later verticals remain unimplemented.

README and CHANGELOG describe only code proven on the final exact CI head. The README must distinguish dependency-free raw-frame support from arbitrary video ingest/playback.

## Exit criteria

H.1 is complete only when:

1. the profile public API is installed and consumable;
2. VPD1 and VFR1 encodings are deterministic and bounded;
3. all four initial pixel layouts round-trip exactly;
4. verified reads require exact S1 provenance to committed S0 inputs;
5. malformed or newer profile data never weakens generic preservation;
6. all compatibility, packaging, GCC, Clang, sanitizer, C ABI, CLI, and AI-contract gates pass on the exact PR head;
7. a focused PR is reviewed and merged without claiming decoders, playback, inference, model quality, performance, or completion of other Stage H profiles.

## Deferred follow-on milestones

The Stage H order after H.1 is:

- H.2 — Telemetry Stream Profile foundation;
- H.3 — Sensor Stream Profile foundation;
- H.4 — Document/Event Stream Profile foundation;
- H.5 — Network/System Stream Profile foundation;
- H.6 — domain-specific schema and model-bundle contracts proven by at least two verticals;
- H.7+ — concrete codec, device, vendor, transport, model, and deployment integrations selected from demonstrated use cases.

This ordering may be refined by later approved designs, but no later profile may fork or weaken generic core truth, identity, time, query, provenance, or archive semantics.

# CODEC v0.4.0

**Channel-Oriented Decomposition, Extraction, and Capture**

CODEC is a C++20 **preservation-first stream capture and archival engine** for exact, inspectable temporal data. It records authorized source bytes into append-only CODA archives, verifies and repairs archive integrity, preserves explicit provenance between source and profile-defined state, and can extract exact source bytes again. In v0.4.0, the Stage H.1 Video Stream Profile also preserves compatible H.264/AAC media as **compressed packet state** that can be remuxed to MP4 without persisting decoded video frames or aggregate PCM audio.

The `codec` CLI supports concurrent local/stdin/HTTP(S) capture, archive verification and inspection, source-exact extraction, verified-prefix follow extraction while an archive is still growing, non-mutating repair, explicit FFmpeg-backed video ingest, grouped multi-video ingest, and verified MP4 export. The installed C++ library additionally exposes generic stream/provenance APIs, Audio Stream Profile processing, bounded multiplex/recovery primitives, and Stage F.1-F.7 distributed-processing interfaces.

> **Scope:** CODEC is not a general-purpose media transcoder, playback framework, remote-worker service, or identity/authentication system. It preserves accepted representations and validates explicit profile contracts; unsupported or contradictory state fails closed instead of being silently transformed.

## Core preservation model

CODEC distinguishes three truth classes:

| Class | Meaning |
|---|---|
| **S0** | Exact accepted source representation. This is the preservation layer. |
| **S1** | Exact deterministic state defined by a registered profile and linked to supporting source records. |
| **D** | Derived, transformed, inferred, or analytical output carrying provenance to its support. |

The architectural rule is simple: **preserve S0 first; interpretation must not erase or replace the accepted source representation.**

For generic CLI capture, `codec record` writes S0 source bytes and `codec extract --feed ...` reconstructs them source-exactly.

For compatible H.1 video ingest in v0.4.0:

```text
accepted MP4/TS/HLS bytes          -> S0
selected H.264 compressed packets  -> EVP1 / 0x0104 / S1
selected AAC compressed packets    -> EAP1 / 0x0103 / S1
```

Video and audio are decoded incrementally during ingest only for validation and bounded resource checks. New compatible H.1 ingest then discards decoded pixel/audio buffers instead of archiving them.

## What changed in v0.4.0

v0.4.0 packages the Stage H.1 compressed-media work as a released capability.

- New compatible FFmpeg video ingest writes one compressed H.264 `EVP1` state (`0x0104`) instead of one raw `VFR1` pixel state per decoded frame.
- Compatible AAC is preserved as compressed `EAP1` packet state (`0x0103`) instead of new Video Profile PCM16 state.
- Compatible H.264/AAC MP4 export is compressed-domain packet remux: no H.264 encoder and no AAC encoder are used on that path.
- Annex-B H.264 can recover in-band SPS/PPS through FFmpeg `extract_extradata`.
- ADTS AAC without global decoder configuration can recover MPEG-4 AudioSpecificConfig through `aac_adtstoasc`.
- Representable AAC leading trim/preroll is preserved with negative packet timing plus an MP4 edit list instead of being rejected.
- Existing verified `VFR1` (`0x0101`) and Video Profile PCM16 (`0x0102`) archives remain readable/exportable through compatibility paths.
- Same-origin unencrypted HTTP/HTTPS HLS remains bounded by CODEC authorization and exact S0 resource capture.

For the complete migration, compatibility, storage, and failure semantics, see [`docs/releases/0.4.0.md`](docs/releases/0.4.0.md).

## CLI overview

| Task | Command |
|---|---|
| Show version | `codec --version` |
| Show runtime capability flags | `codec capabilities` |
| Show help | `codec --help` |
| Record one or more generic feeds | `codec record` |
| Explicitly preserve validated H.264/AAC media | `codec video ingest` |
| Export one verified video stream to MP4 | `codec video export ... --stream ...` |
| Export every video stream to MP4 | `codec video export ... --all` |
| Verify a CODA archive | `codec verify` |
| Verify and inspect basic archive information | `codec inspect` |
| List feed descriptors | `codec list feeds` |
| Extract exact source bytes | `codec extract` |
| Follow exact source bytes while an archive grows | `codec extract --follow` |
| Rebuild the valid committed prefix into a new archive | `codec repair` |

Successful operations normally write JSON or JSON Lines to stdout. Human-readable failures go to stderr.

---

# Build

## Validated environment

Project CI builds and tests v0.4.0 on Ubuntu Linux with GCC and Clang. The current implementation is POSIX-oriented; native Windows is not a validated v0.4.0 target.

Default requirements:

- CMake 3.20+
- C++20 compiler
- OpenSSL 3.0+ Crypto
- libcurl
- pkg-config
- libFLAC development headers/library
- FFmpeg development libraries: `libavformat`, `libavcodec`, `libavutil`, `libswscale`, `libswresample`
- Ninja or Make

Ubuntu/Debian example:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config curl ca-certificates \
  libssl-dev libcurl4-openssl-dev libflac-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
```

Build:

```bash
git clone https://github.com/jordanlegare/codec.git
cd codec
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Version checks:

```bash
./build/codec --version
./build/codec --help
./build/codec capabilities
```

Expected release identity:

```text
codec 0.4.0
```

The help banner begins with:

```text
CODEC 0.4.0 - preservation-first multi-stream capture, CODA archival, and media preservation
```

`codec capabilities` reports the same project version:

```json
{
  "version":"0.4.0",
  "coda_archive":true,
  "file_capture":true,
  "http_capture":true,
  "pcm16_wav":true,
  "neural_separation":false,
  "gpu_inference":false
}
```

The version in `--version`, the help banner, capabilities, installed package metadata, and library version is derived from CMake `PROJECT_VERSION`.

## FFmpeg-disabled foundation build

The media-library-independent H.1 schemas and verified readers remain buildable without FFmpeg:

```bash
cmake -S . -B build-no-ffmpeg -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCODEC_ENABLE_FFMPEG_VIDEO=OFF
cmake --build build-no-ffmpeg --parallel
ctest --test-dir build-no-ffmpeg --output-on-failure
```

With FFmpeg disabled, `ffmpeg_video_ingest_available()` returns false and FFmpeg-backed ingest/export operations fail explicitly with `model_incompatible` rather than fabricating output.

## CMake options

| Switch | Default | Meaning |
|---|---:|---|
| `CODEC_BUILD_TESTS` | `ON` | Build/register tests. |
| `CODEC_BUILD_EXAMPLES` | `ON` | Build examples. |
| `CODEC_WARNINGS_AS_ERRORS` | `OFF` | Promote warnings to errors. CI enables this. |
| `CODEC_ENABLE_SANITIZERS` | `OFF` | Enable ASan/UBSan for GCC/Clang. |
| `CODEC_ENABLE_FFMPEG_VIDEO` | `ON` | Enable FFmpeg H.1 ingest/export bridge. |
| `CODEC_ONNXRUNTIME_ROOT` | empty | Optional extracted ONNX Runtime root for the Audio CPU backend. |

Strict release build:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Sanitizer build:

```bash
cmake -S . -B build-san -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON \
  -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel
ctest --test-dir build-san --output-on-failure
```

## Install and consume

```bash
cmake --install build --prefix "$HOME/.local"
```

Installed artifacts include the CLI, library, public headers, and CMake package files.

Consumer example:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_codec_app LANGUAGES CXX)

find_package(codec CONFIG REQUIRED)
add_executable(my_codec_app main.cpp)
target_compile_features(my_codec_app PRIVATE cxx_std_20)
target_link_libraries(my_codec_app PRIVATE codec::codec)
```

A default installation exposes the FFmpeg pkg-config dependencies to consumers. An installation built with `CODEC_ENABLE_FFMPEG_VIDEO=OFF` does not require those FFmpeg package targets.

---

# CLI reference

## Exit status

| Code | Meaning |
|---:|---|
| `0` | Command completed successfully. |
| `1` | General runtime/library/profile/I/O failure. |
| `2` | Invalid or missing command-line arguments. |
| `3` | `verify`/`inspect` completed but archive verification was not OK. |

Errors use:

```text
codec: <error_code>: <message>
```

Common error-code names include `invalid_argument`, `unauthorized_source`, `network`, `protocol`, `decode`, `archive_io`, `archive_corrupt`, `model_incompatible`, `inference`, `resource_exhausted`, and `internal`.

## `codec capabilities`

```bash
codec capabilities
```

The capability JSON is intentionally small. `neural_separation:false` means CODEC does not bundle a production neural model/default neural separation path. `gpu_inference:false` means no GPU inference backend is provided. H.1 FFmpeg availability is queried through the C++ Video Profile API rather than a new CLI field.

## `codec record`

Capture one or more generic sources as exact S0:

```bash
codec record --archive FILE --feed LABEL=URI [--feed LABEL=URI ...]
```

Supported URI/source forms include:

```text
/path/to/file
file:///path/to/file
-
http://example.com/source
https://example.com/source
```

A URL containing query-string `=` characters must still be supplied as the URI portion of `LABEL=URI`:

```bash
codec record \
  --archive camera.coda \
  --feed 'camera3800=https://www.quebec511.info/Carte/Fenetres/camera.ashx?id=3800&format=mp4'
```

`codec record` remains generic S0 capture. It does **not** automatically invoke FFmpeg or create EVP1/EAP1 media state.

Default CLI capture policy is conservative:

- local sources are opened read-only without following symlinks;
- private/local HTTP destinations are denied by default;
- environment proxies are disabled while private-network denial is active;
- redirects are refused instead of being followed without re-authorization;
- only HTTP/HTTPS are accepted for network capture;
- HTTP responses must be successful 2xx responses.

The default engine uses 256 KiB capture chunks, a 16 GiB maximum per feed, eight pending chunks per stream, and a 64 MiB aggregate pending-capture bound. These values are configurable through the C++ `EngineConfig`; the v0.4.0 CLI does not expose switches for them.

## `codec video ingest`

### Grouped multi-video form

The preferred form for one or more video streams in one archive is:

```bash
codec video ingest --archive FILE \
  --video --source URI --label LABEL --start-ns NS --end-ns NS [VIDEO OPTIONS] \
  [--video --source URI --label LABEL --start-ns NS --end-ns NS [VIDEO OPTIONS] ...]
```

Example:

```bash
codec video ingest --archive cameras.coda \
  --video --source ./left.mp4  --label left  --start-ns 0 --end-ns 10000000000 \
  --video --source ./right.mp4 --label right --start-ns 0 --end-ns 10000000000
```

### Legacy single-video form

Still supported:

```bash
codec video ingest \
  --source URI \
  --archive FILE \
  --label LABEL \
  --start-ns NS \
  --end-ns NS \
  [VIDEO OPTIONS]
```

Video Options:

```text
--layout gray8|rgb24|rgba32|yuv420p8
--maximum-source-bytes N
--maximum-decoded-bytes N
--maximum-decoded-audio-bytes N
--maximum-frames N
--maximum-hls-resources N
--maximum-hls-resource-bytes N
--maximum-hls-total-bytes N
```

`--layout` remains accepted for compatibility and defaults to `yuv420p8`, but new EVP1 packet storage is layout-independent and does not persist decoded pixel layouts.

On successful compatible H.264 ingest, CODEC writes one provenance-verified `EVP1` encoded-video state (`0x0104`) and no new per-frame `VFR1` state (`0x0101`). If compatible AAC exists, CODEC writes one `EAP1` encoded-audio state (`0x0103`) and no new Video Profile PCM16 state (`0x0102`).

If exact S0 capture succeeds but media validation later fails, CODEC finalizes a source-preserving archive and reports the profile error rather than discarding accepted source bytes.

### Direct-media boundary

For direct file/HTTP/HTTPS media, CODEC captures the accepted source first, commits exact S0, and demuxes/decodes those already captured bytes through CODEC-owned I/O. FFmpeg does not receive independent network authority for direct media.

### HLS boundary

The H.1 bridge supports bounded **same-origin unencrypted HTTP/HTTPS HLS**. CODEC captures the primary manifest and every accepted child object as exact S0 before FFmpeg reads those bytes. Encrypted HLS, key retrieval, cross-origin children, private-network-denied children, malformed/non-HTTP(S) resources, DASH, and arbitrary native FFmpeg protocol fallback are not supported.

For live HLS, `end_ns - start_ns` bounds decoded media timeline rather than promising an exact wall-clock recording duration. Resource-count and byte ceilings remain independent bounds.

## `codec video export`

Export one verified video stream:

```bash
codec video export ARCHIVE \
  --stream UUID \
  --output FILE \
  [--maximum-frames N] \
  [--maximum-input-bytes N] \
  [--maximum-output-bytes N]
```

Export all video streams:

```bash
codec video export ARCHIVE \
  --all \
  --output-dir DIR \
  [--maximum-frames N] \
  [--maximum-input-bytes N] \
  [--maximum-output-bytes N]
```

For compatible new EVP1/EAP1 state, export remuxes compressed H.264/AAC packets into MP4 without decoding/re-encoding the media.

H.264 handling:

- length-prefixed AVC requires valid decoder configuration;
- Annex-B can recover in-band SPS/PPS through `extract_extradata`;
- unrecoverable configuration returns `model_incompatible` rather than silently transcoding.

AAC handling:

- stored AudioSpecificConfig is used directly;
- empty-config ADTS AAC can use `aac_adtstoasc` to derive AudioSpecificConfig and strip ADTS framing;
- representable leading trim/preroll keeps negative packet timing and is expressed through an MP4 edit list;
- a trim claim with no compressed preroll evidence remains `model_incompatible`.

Legacy verified VFR1 video and Video PCM16 audio remain exportable through compatibility paths. Contradictory verified forms for the same stream fail closed.

## `codec verify` and `codec inspect`

```bash
codec verify ARCHIVE [--level full]
codec inspect ARCHIVE
```

Verification reports include:

- `ok`
- `finalized`
- `committed_records`
- `verified_payload_bytes`
- `valid_prefix_bytes`
- `file_bytes`
- `message`

`inspect` additionally reports feed count when verification succeeds.

`valid_prefix_bytes < file_bytes` indicates bytes exist beyond the verified committed prefix.

## `codec list feeds`

```bash
codec list feeds ARCHIVE
```

Prints one JSON object per feed with label, stable stream ID, recorded/redacted URI descriptor, and `"fidelity":"S0"`.

## `codec extract`

```bash
codec extract ARCHIVE \
  --feed LABEL \
  [--fidelity source-exact] \
  [--follow] \
  --output FILE
```

`source-exact` is the only CLI extraction fidelity in v0.4.0 and is the default when omitted.

With `--follow`, CODEC reads only the verified committed prefix, writes newly committed S0 bytes for the selected feed, and finishes when a committed final index becomes visible.

## `codec repair`

```bash
codec repair ARCHIVE --output REPAIRED.coda
```

Repair is non-mutating. It rebuilds the valid committed prefix into a separate archive and reports recovered record/payload counts plus discarded tail bytes.

---

# v0.4.0 H.1 storage and compatibility

## Record types

| Record | Code | Current role |
|---|---:|---|
| `VPD1` | `0x0100` | Video profile descriptor. |
| `VFR1` | `0x0101` | Legacy raw-video-frame S1; retained for verified reads/export. |
| Video PCM16 | `0x0102` | Legacy Video Profile audio state; retained for verified reads/export. |
| `EAP1` | `0x0103` | Current compressed AAC packet S1 state. |
| `EVP1` | `0x0104` | Current compressed H.264 packet S1 state. |

The H.1 foundation is media-library independent. `query_verified_raw_video_frames()` and `query_verified_video_encoded_video()` enforce canonical state/provenance contracts without requiring FFmpeg.

The profile-local process contracts include existing legacy `codec.video.raw-frame.canonicalize.hls` state verification and the new `codec.video.encoded-video.preserve` packet-preservation path.

## Storage implications

The main 0.4.0 storage correction is structural: new compatible H.1 ingest no longer duplicates each decoded video frame as Gray/RGB/RGBA/YUV pixel bytes and no longer duplicates compatible AAC as aggregate Video Profile PCM16.

Archive growth now reflects exact S0 plus compressed EVP1/EAP1 packet copies and archive/provenance/index overhead. The actual reduction depends on the source bitrate, dimensions, frame rate, audio bitrate, HLS segmentation, and container overhead, so CODEC does not publish a universal reduction factor.

For one concrete audio comparison, 48 kHz stereo PCM16 is 691.2 MB/hour, while 128 kbit/s compressed audio is 57.6 MB/hour before packet-table/archive overhead—about twelve times smaller for that specific workload.

## Compatibility

Existing CODA archives are not rewritten in place. Verified `0x0101` and `0x0102` state remains supported. New compatible H.1 ingest prefers `0x0104`/`0x0103`.

The current CODA executable format and profile encodings remain development-profile formats in the 0.x line, not a frozen normative CODA v1 interoperability standard.

---

# C++ library capabilities beyond the CLI

## Generic stream/archive APIs

The installed library includes:

- stable generic `StreamId`, `StreamDescriptor`, clocks, epochs, timing, and gap metadata;
- exact physical-record queries and extraction;
- S1/D provenance sidecars and direct-provenance queries;
- generic `StreamAdapter`, `StreamProcessor`, and `StreamExporter` interfaces;
- source-exact recording/extraction and non-mutating repair;
- preservation of unknown compatible development-profile record type codes.

## Audio Stream Profile

The C++ Audio Stream Profile includes deterministic PCM16 `APS1`, preservation-first PCM16 WAV/FLAC ingest, verified WAV/FLAC export, bounded offline separation orchestration, deterministic `AMB1` model bundles, and an optional caller-activated ONNX Runtime CPU backend. No production neural model is bundled.

## Video Stream Profile — Stage H.1

The media-library-independent Video Stream Profile defines deterministic bounded `VPD1`, legacy `VFR1`, encoded-audio `EAP1`, and encoded-video `EVP1` state plus strict verified readers and profile-local provenance contracts.

Default builds add the FFmpeg ingest/export bridge described above. Direct media uses already captured bytes. HLS children must pass CODEC's same-origin/capture policy. Compatible H.264/AAC uses EVP1/EAP1 packet preservation and MP4 remux; legacy VFR1/PCM16 remains compatibility-only for new audiovisual ingest.

**Stage G trust/selective-disclosure work is explicitly deferred** and is not claimed complete. Stage H is active at H.1; H.2+ telemetry and later domain/profile stages remain planned work.

H.1 does not provide general-purpose transcoding, playback, GStreamer integration, encrypted/cross-origin HLS, DASH, arbitrary FFmpeg protocol access, GPU decode, streaming video inference, or a video model.

## Stage E transport/recovery

The library includes deterministic `CMX1` multiplex framing/demultiplexing, sequence-loss observation, bounded recovery groups, `XRF1` XOR repair symbols, and `StreamingRepairSession` single-erasure orchestration. This is not a socket/network/retransmission service.

## Stage F.1-F.7 distributed primitives

The library includes deterministic partitioning, bounded worker execution, provider-neutral object-range retrieval, deterministic location indexing, synchronous scheduling, a provider-neutral remote-worker transport seam, and bounded `DRQ1`/`DRS1` serialization.

It does **not** provide a built-in HTTP/gRPC/QUIC worker service, endpoint discovery, credentials/authentication, encrypted worker sessions, retry/failover/exactly-once semantics, concrete cloud object-store clients, or a distributed-processing CLI.

---

# Security and operational boundaries

- Archive/frame/envelope SHA-256 provides corruption evidence under the defined structure; it is not external identity authentication.
- Generic HTTP capture blocks private destinations by default and refuses redirects under the current CLI policy.
- The FFmpeg bridge does not receive unrestricted source/network authority: direct media uses captured bytes; HLS children are independently authorized and captured by CODEC before use.
- Media parsing still processes potentially hostile bytes through linked FFmpeg libraries; CODEC does not claim a decoder sandbox.
- Distributed worker labels and wire envelopes do not authenticate workers.
- No production-scale throughput, availability, durability, or storage-ratio guarantee is claimed.

Use CODEC only with sources and systems you are authorized to access and preserve.

---

# Release scope: v0.4.0

v0.4.0 releases the merged Stage H.1 compressed H.264/AAC preservation and MP4 remux workflow, including same-origin unencrypted HLS, EVP1/EAP1 state, configuration recovery, and representable AAC leading-trim edit-list handling. It also retains the Stage E/F and Audio Profile capabilities accumulated in earlier releases.

Detailed release and migration guide: [`docs/releases/0.4.0.md`](docs/releases/0.4.0.md).

Change history: [`CHANGELOG.md`](CHANGELOG.md).

The release workflow publishes tag `v0.4.0` and GitHub release `CODEC v0.4.0` from the exact release commit.

---

# Developer and architecture documentation

- [`CONTRIBUTING.md`](CONTRIBUTING.md) — contributor workflow and architectural contribution rules.
- [`AGENTS.md`](AGENTS.md) — machine-readable repository instructions.
- [`AI_WORKSHEET.md`](AI_WORKSHEET.md) — active implementation/verification record.
- [`docs/releases/0.4.0.md`](docs/releases/0.4.0.md) — v0.4.0 release guide.
- [`docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md`](docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md) — stream-first architecture rationale.
- [`docs/superpowers/specs/2026-08-30-stage-h1-video-profile-design.md`](docs/superpowers/specs/2026-08-30-stage-h1-video-profile-design.md) — Stage H.1 Video Profile design.
- [`docs/superpowers/plans/2026-08-30-stage-h1-video-profile.md`](docs/superpowers/plans/2026-08-30-stage-h1-video-profile.md) — Stage H.1 implementation plan.
- [`docs/superpowers/specs/2026-08-30-video-ffmpeg-ingest-design.md`](docs/superpowers/specs/2026-08-30-video-ffmpeg-ingest-design.md) — preservation-first FFmpeg integration boundary.
- [`docs/superpowers/plans/2026-08-30-video-ffmpeg-ingest.md`](docs/superpowers/plans/2026-08-30-video-ffmpeg-ingest.md) — FFmpeg ingest implementation plan.
- [`docs/superpowers/specs/2026-08-30-video-hls-ingest-design.md`](docs/superpowers/specs/2026-08-30-video-hls-ingest-design.md) — HLS authorization/source-frontier design.
- [`docs/superpowers/plans/2026-08-30-video-hls-ingest.md`](docs/superpowers/plans/2026-08-30-video-hls-ingest.md) — HLS implementation plan.
- [`docs/superpowers/specs/2026-08-31-video-encoded-audio-design.md`](docs/superpowers/specs/2026-08-31-video-encoded-audio-design.md) — EAP1 encoded-audio design.
- [`docs/superpowers/plans/2026-08-31-video-encoded-audio.md`](docs/superpowers/plans/2026-08-31-video-encoded-audio.md) — EAP1 implementation plan.
- [`docs/superpowers/specs/2026-08-31-h1-encoded-video-preservation-design.md`](docs/superpowers/specs/2026-08-31-h1-encoded-video-preservation-design.md) — EVP1 compressed-video design.
- [`docs/superpowers/plans/2026-08-31-h1-encoded-video-preservation.md`](docs/superpowers/plans/2026-08-31-h1-encoded-video-preservation.md) — EVP1 implementation plan.
- [`docs/superpowers/specs/2026-09-01-aac-leading-trim-export-design.md`](docs/superpowers/specs/2026-09-01-aac-leading-trim-export-design.md) — AAC preroll/edit-list export design.
- [`docs/superpowers/plans/2026-09-01-aac-leading-trim-export.md`](docs/superpowers/plans/2026-09-01-aac-leading-trim-export.md) — AAC leading-trim implementation plan.

A small machine-readable project block is retained because CI verifies documentation/version continuity:

```yaml
project:
  name: CODEC
  version: 0.4.0
  language: C++20
  build: CMake >= 3.20
  archive: CODA (.coda)
  architecture: stream-first, payload-type agnostic
```

# CODEC v0.3.0

**Channel-Oriented Decomposition, Extraction, and Capture**

CODEC is a C++20 preservation-first capture and archive engine for temporal data streams. The `codec` command-line program can record one or more local, standard-input, HTTP, or HTTPS feeds into a CODA archive, verify or inspect archive integrity, list feeds, extract exact source bytes by feed, and repair a damaged trailing archive segment into a new file.

The current development tree, based on **v0.3.0**, contains substantially more functionality in the installed C++ library than is exposed through the CLI, including generic stream/provenance APIs, transport multiplexing and bounded XOR recovery, audio-profile processing, the unreleased Stage H.1 Video Stream Profile foundation with an optional FFmpeg ingest bridge, and the Stage F.1-F.7 distributed-processing primitives.

> **Important scope:** the distributed Stage F.1-F.7 implementation and the development-tree optional FFmpeg Video Profile ingest bridge are C++ library/package capabilities. There is no `codec distributed ...` CLI command, no built-in remote HTTP/gRPC worker service, and no CLI mode that automatically decodes recorded video into S1 frames.

## What CODEC is for

CODEC is designed around a simple rule: **preserve the accepted source representation before optional interpretation or derivation**.

It distinguishes three truth classes:

| Class | Meaning |
|---|---|
| **S0** | The exact accepted source representation. This is the preservation layer used by the CLI capture/extract workflow. |
| **S1** | An exact canonical state defined by a registered profile, such as deterministic PCM16 state in the Audio Stream Profile. |
| **D** | A derived, inferred, transformed, or analytical result that carries provenance back to supporting records. |

For a normal CLI user, the most important behavior is that `codec record` writes S0 source bytes to a `.coda` archive and `codec extract --feed ...` reconstructs those source bytes with source-exact fidelity by default.

## What you can do from the CLI in v0.3.0

| Task | Command |
|---|---|
| Show version | `codec --version` |
| Show runtime capability flags | `codec capabilities` |
| Record one or more feeds | `codec record` |
| Verify a CODA archive | `codec verify` |
| Verify and show basic archive information | `codec inspect` |
| List legacy feed descriptors | `codec list feeds` |
| Extract exact source bytes | `codec extract` |
| Follow exact source bytes while an archive is still growing | `codec extract --follow` |
| Rebuild the valid committed prefix of a damaged archive into a new archive | `codec repair` |

The CLI writes machine-readable JSON or JSON Lines to standard output for successful operations. Human-readable errors go to standard error.

---

# Build environment

## Supported/validated build environment

The project CI for v0.3.0 builds and tests on **Ubuntu Linux** with both **GCC** and **Clang**. The implementation is currently POSIX-oriented; native Windows is not a validated v0.3.0 target.

Minimum project requirements:

- CMake **3.20+**
- C++20 compiler
- OpenSSL **3.0+** Crypto
- libcurl
- pkg-config
- libFLAC development headers/library
- a build tool such as Ninja or Make

FFmpeg development libraries are optional and are required only when `CODEC_ENABLE_FFMPEG_VIDEO=ON`.

### Ubuntu/Debian setup

A practical development environment for the default build is:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  curl \
  ca-certificates \
  libssl-dev \
  libcurl4-openssl-dev \
  libflac-dev
```

To enable the optional FFmpeg Video Profile ingest backend, also install:

```bash
sudo apt-get install -y \
  libavformat-dev \
  libavcodec-dev \
  libavutil-dev \
  libswscale-dev
```

To build with Clang as well:

```bash
sudo apt-get install -y clang
```

CI currently validates the optional ONNX Runtime CPU integration using ONNX Runtime **1.29.0** on Linux x86-64, but ONNX Runtime is **not required** to build or use the CLI functionality described in this README.

## Clone and build

```bash
git clone https://github.com/jordanlegare/codec.git
cd codec

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The executable is then:

```bash
./build/codec --version
./build/codec capabilities
```

Expected version output:

```text
codec 0.3.0
```

## Run the test suite

Tests are enabled by default:

```bash
ctest --test-dir build --output-on-failure
```

The normal test configuration includes the C++ unit suite, C API test, real CLI integration test, and repository contract check.

## CMake switches

These are the CODEC-specific CMake configuration switches in the current development tree:

| Switch | Default | Meaning |
|---|---:|---|
| `CODEC_BUILD_TESTS` | `ON` | Build and register the test suite. |
| `CODEC_BUILD_EXAMPLES` | `ON` | Build the example programs, including `codec_capture_example`. |
| `CODEC_WARNINGS_AS_ERRORS` | `OFF` | Promote compiler warnings to errors (`-Werror` or `/WX`). CI enables this. |
| `CODEC_ENABLE_SANITIZERS` | `OFF` | For GCC/Clang, compile/link with AddressSanitizer and UndefinedBehaviorSanitizer. |
| `CODEC_ENABLE_FFMPEG_VIDEO` | `OFF` | Enable the optional FFmpeg-backed Video Profile ingest bridge; requires `libavformat`, `libavcodec`, `libavutil`, and `libswscale` development packages discoverable through pkg-config. |
| `CODEC_ONNXRUNTIME_ROOT` | empty | Optional path to an extracted ONNX Runtime distribution used by the Audio CPU separation backend. |

Useful standard CMake switches include:

| Switch | Example | Purpose |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release`, `Debug` | Select optimization/debug configuration for single-config generators such as Ninja. |
| `CMAKE_INSTALL_PREFIX` | `/opt/codec` | Default installation prefix used by `cmake --install`. |
| `CMAKE_PREFIX_PATH` | `/opt/codec` | Helps another CMake project find an installed CODEC package. |
| `CMAKE_C_COMPILER` | `clang` | Select a C compiler explicitly. |
| `CMAKE_CXX_COMPILER` | `clang++` | Select a C++ compiler explicitly. |

Example strict release build:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Sanitizer build

```bash
cmake -S . -B build-san -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON \
  -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel
ctest --test-dir build-san --output-on-failure
```

`CODEC_ENABLE_SANITIZERS` has an effect only for GCC/Clang builds.

## Optional ONNX Runtime CPU backend

A normal CLI build does not need ONNX Runtime. To compile the optional C++ Audio separation backend, point `CODEC_ONNXRUNTIME_ROOT` at an extracted runtime distribution containing:

```text
<root>/include/onnxruntime_c_api.h
<root>/lib/libonnxruntime.so        # Linux
```

Then configure with:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCODEC_ONNXRUNTIME_ROOT=/path/to/onnxruntime
```

If `CODEC_ONNXRUNTIME_ROOT` is supplied but the expected header/shared library cannot be found, configuration fails instead of silently disabling the backend.

Even when this optional backend is compiled, `codec capabilities` still reports `"neural_separation":false` in v0.3.0. That flag means CODEC does not bundle a production model or default neural-separation runtime path for the CLI; the optional backend is activated through the C++ API with caller-supplied model/runtime material.

## Optional FFmpeg Video Profile ingest backend

The H.1 Video Stream Profile schema and verified reader remain usable without FFmpeg. To compile the optional preservation-first encoded-video ingest bridge, install the four FFmpeg development modules listed above and configure with:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCODEC_ENABLE_FFMPEG_VIDEO=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

When enabled, the installed C++ API exposes `codec::profiles::video::ingest_video_ffmpeg()`. The bridge:

- captures the accepted file/HTTP/HTTPS source through CODEC's existing bounded capture policy;
- commits the exact encoded/container representation as S0 before media interpretation;
- decodes only those already captured in-memory bytes through FFmpeg custom AVIO;
- blocks libavformat secondary protocol I/O with a reject-all secondary-open callback plus an inherited whitelist containing no real FFmpeg URL protocol, instead of allowing nested file or network retrieval;
- canonicalizes decoded frames to an explicitly requested H.1 Gray8, RGB24, RGBA32, or planar YUV420P8 layout;
- writes each successful `VFR1` frame as S1 with direct exact same-stream S0 provenance;
- finalizes a valid source-only archive with `profile_error` populated when media demux/decode/canonicalization fails after S0 preservation.

The backend does not change `codec record`: the CLI remains generic S0 capture and still requires `--feed LABEL=URI`. There is no automatic CLI MP4-to-VFR1 decoding mode.

When the library is built with `CODEC_ENABLE_FFMPEG_VIDEO=OFF`, `ffmpeg_video_ingest_available()` reports `false` and a valid `ingest_video_ffmpeg()` request fails explicitly with `model_incompatible` before creating an archive.

## Install CODEC

```bash
cmake --install build --prefix "$HOME/.local"
```

This installs:

- the `codec` executable;
- the CODEC library;
- public headers under `include/codec`;
- CMake package files for `find_package(codec CONFIG ...)`.

If `$HOME/.local/bin` is on your `PATH`:

```bash
codec --version
```

## Use CODEC from another CMake project

After installation, a C++20 consumer can use:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_codec_app LANGUAGES CXX)

find_package(codec CONFIG REQUIRED)

add_executable(my_codec_app main.cpp)
target_compile_features(my_codec_app PRIVATE cxx_std_20)
target_link_libraries(my_codec_app PRIVATE codec::codec)
```

If CODEC was installed into a non-system prefix:

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/path/to/codec/install
```

The consumer must always be able to resolve CODEC's public OpenSSL 3 Crypto, CURL, pkg-config, and FLAC dependencies. If the installed CODEC library was built with `CODEC_ENABLE_FFMPEG_VIDEO=ON`, its package configuration also recreates the required FFmpeg pkg-config targets so static consumers resolve `libavformat`, `libavcodec`, `libavutil`, and `libswscale` correctly.

---

# CLI reference

## General behavior

```text
codec --help
codec help
codec --version
```

`--help`/`help` prints the command synopsis. `--version` prints the configured CMake project version.

### Exit status

The principal CLI exit statuses in v0.3.0 are:

| Code | Meaning |
|---:|---|
| `0` | Command completed successfully. |
| `1` | General runtime/library error such as I/O, protocol, archive, key, or processing failure. |
| `2` | Invalid or missing command-line arguments, unknown command, or malformed identifier. |
| `3` | `verify`/`inspect` completed verification but the archive verification report is not OK. |

Library errors are printed as:

```text
codec: <error_code>: <message>
```

Examples of error-code names include `invalid_argument`, `unauthorized_source`, `network`, `protocol`, `decode`, `archive_io`, `archive_corrupt`, `model_incompatible`, `inference`, `resource_exhausted`, and `internal`.

---

## `codec capabilities`

```bash
codec capabilities
```

Example shape:

```json
{
  "version":"0.3.0",
  "coda_archive":true,
  "file_capture":true,
  "http_capture":true,
  "pcm16_wav":true,
  "neural_separation":false,
  "gpu_inference":false
}
```

Fields:

| Field | Meaning in v0.3.0 |
|---|---|
| `version` | CLI/project version. |
| `coda_archive` | CODA development-profile archive read/write/verify support is present. |
| `file_capture` | Local file/stdin capture is present. |
| `http_capture` | HTTP/HTTPS source capture is present. |
| `pcm16_wav` | PCM16 RIFF/WAVE support is present. |
| `neural_separation` | `false`: no production neural separation model/default CLI runtime is bundled. |
| `gpu_inference` | `false`: no GPU inference backend is provided. |

These are capability declarations, not performance measurements. The optional FFmpeg C++ ingest backend is queried through `ffmpeg_video_ingest_available()` rather than a new CLI capability field.

---

## `codec record`

Capture one or more feeds into a CODA archive:

```bash
codec record --archive FILE --feed LABEL=URI [--feed LABEL=URI ...]
```

### Switches

| Argument | Required | Meaning |
|---|---:|---|
| `--archive FILE` | yes | Output CODA archive path. |
| `--feed LABEL=URI` | yes, repeatable | Declares one named input source. Multiple feeds are captured concurrently into one archive. |

At least one feed is required.

### Feed labels

A feed label must:

- be unique within the command;
- contain 1-128 printable ASCII characters;
- not contain `=`.

### Supported URI/source forms

`URI` can be:

```text
/path/to/file
file:///path/to/file
-
http://example.com/source
https://example.com/source
```

Meaning:

- a plain path reads a local file/device/FIFO path;
- `file://...` reads a local path;
- `-` duplicates and reads standard input;
- `http://` and `https://` perform a bounded HTTP capture.

Other URI schemes are rejected.

### Capture security behavior

The CLI uses the default `EngineConfig`, which deliberately applies conservative source rules:

- local paths are opened read-only with no symbolic-link following;
- private/local HTTP destinations are denied by default;
- resolved HTTP socket addresses must be globally routable;
- environment proxies are disabled while private-network denial is active;
- HTTP redirects are refused in v0.3.0 rather than being followed without re-authorizing every hop;
- only HTTP/HTTPS protocols are permitted;
- HTTP responses must be successful 2xx responses;
- HTTP connection timeout is 15 seconds.

There are currently **no CLI switches** to disable these protections.

### CLI capture limits

The CLI currently uses these fixed default engine limits:

| Limit | v0.3.0 default |
|---|---:|
| Capture chunk size | 256 KiB |
| Maximum bytes per feed | 16 GiB |
| Maximum pending chunks per stream | 8 |
| Maximum aggregate pending capture bytes | 64 MiB |

The C++ `EngineConfig` can customize these values, but the v0.3.0 CLI does not expose them as switches.

When capturing multiple feeds, producer reads may happen concurrently. A single archive writer serializes committed records. CODEC preserves each stream's own S0 byte order; it does **not** promise deterministic physical interleaving between independent feeds.

### Output metrics

Successful recording prints one JSON object:

```json
{
  "archive":"session.coda",
  "feeds_recorded":2,
  "source_bytes":123456,
  "source_records":7
}
```

| Field | Meaning |
|---|---|
| `archive` | Archive path reported by the engine. |
| `feeds_recorded` | Number of successfully recorded feed descriptors/sources. |
| `source_bytes` | Total exact source payload bytes committed across the feeds. |
| `source_records` | Number of committed S0 source-byte records/chunks, not the number of input files. |

### Examples

One local file:

```bash
codec record \
  --archive session.coda \
  --feed news=./input.bin
```

Two concurrent local feeds:

```bash
codec record \
  --archive session.coda \
  --feed left=./left.raw \
  --feed right=./right.raw
```

Standard input:

```bash
cat input.bin | codec record \
  --archive stdin.coda \
  --feed stdin=-
```

Public HTTPS source:

```bash
codec record \
  --archive remote.coda \
  --feed source=https://example.com/media.bin
```

A URL containing query-string `=` characters must still be supplied as the URI portion of `LABEL=URI`, for example:

```bash
codec record \
  --archive camera.coda \
  --feed 'camera3800=https://www.quebec511.info/Carte/Fenetres/camera.ashx?id=3800&format=mp4'
```

That command preserves the remote response as S0. It does not invoke the optional C++ FFmpeg profile bridge automatically.

---

## `codec verify`

```bash
codec verify ARCHIVE [--level full]
```

The v0.3.0 CLI has one archive verification behavior: it runs `CodaArchive::verify()` over the archive integrity structure. `--level full` is accepted as the documented spelling but does not select between multiple verification levels in this release.

Example output:

```json
{
  "ok":true,
  "finalized":true,
  "committed_records":12,
  "verified_payload_bytes":123456,
  "valid_prefix_bytes":125632,
  "file_bytes":125632,
  "message":"ok"
}
```

### Verification metrics

| Field | Meaning |
|---|---|
| `ok` | Whether archive verification succeeded. |
| `finalized` | Whether a committed final index is present. An actively growing archive may be valid but not finalized. |
| `committed_records` | Number of records accepted in the verified committed chain/prefix. |
| `verified_payload_bytes` | Sum of record payload bytes covered by successful verification. |
| `valid_prefix_bytes` | File offset/length through the valid committed archive prefix. This is especially useful when the tail is damaged or incomplete. |
| `file_bytes` | Physical file size at verification time. |
| `message` | Human-readable verification summary. |

`valid_prefix_bytes < file_bytes` indicates bytes exist after the valid committed prefix.

`verify` exits with status `3` when the verification report is not OK.

---

## `codec inspect`

```bash
codec inspect ARCHIVE
```

`inspect` runs the same archive verification and prints the same metrics as `verify`. If verification is OK, it also adds:

```json
"feeds":2
```

`feeds` is the number of legacy feed descriptors visible in the archive. It is not a count of generic records or derived artifacts.

---

## `codec list feeds`

```bash
codec list feeds ARCHIVE
```

Outputs one JSON object per feed, one object per line:

```json
{"label":"news","stream_id":"...","uri":"./input.bin","fidelity":"S0"}
```

Fields:

| Field | Meaning |
|---|---|
| `label` | Feed label supplied during recording. |
| `stream_id` | Stable logical stream identifier stored/projected for the feed. |
| `uri` | Recorded/redacted source URI descriptor. URI user-info and query/fragment material are removed before descriptor persistence where applicable. |
| `fidelity` | `S0`, indicating source-preservation truth. |

---

## `codec extract`

Extract exact source bytes by feed label:

```bash
codec extract ARCHIVE --feed LABEL [--fidelity source-exact] [--follow] --output FILE
```

### Switches

| Argument | Required | Meaning |
|---|---:|---|
| `ARCHIVE` | yes | Source CODA archive. |
| `--feed LABEL` | yes | Select by feed label. |
| `--fidelity source-exact` | no | `source-exact` is the only supported extraction fidelity in the v0.3.0 CLI. It is the default when omitted; the explicit spelling remains accepted for existing scripts. |
| `--output FILE` | yes | Destination file. |
| `--follow` | no | Follow the verified committed prefix while the archive is still being written. |

### Normal extraction output

```json
{"feed":"news","fidelity":"source_exact","bytes":123456}
```

`bytes` is the number of exact S0 payload bytes written to the output file.

### Live follow extraction

```bash
codec extract live.coda \
  --feed LABEL1 \
  --follow \
  --output live.bin
```

Follow mode:

1. reads only the archive's verified committed prefix;
2. writes newly committed source bytes for the selected stream incrementally;
3. does not emit another stream's bytes;
4. stays attached while the archive remains open/growing;
5. finishes when a committed final index becomes visible.

Final follow output adds:

```json
"follow":true
```

The `bytes` count is the total selected source bytes written during that follow session.

---

## `codec repair`

```bash
codec repair ARCHIVE --output REPAIRED.coda
```

Repair is **non-mutating**: it reads the source archive and writes a separate output archive from the recoverable committed prefix.

### Switches

| Argument | Required | Meaning |
|---|---:|---|
| `ARCHIVE` | yes | Damaged/incomplete source archive. |
| `--output FILE` | yes | New repaired archive path. |

Example:

```bash
codec repair damaged.coda --output repaired.coda
codec verify repaired.coda --level full
```

### Repair metrics

```json
{
  "recovered_records":11,
  "recovered_payload_bytes":120000,
  "discarded_tail_bytes":20
}
```

| Field | Meaning |
|---|---|
| `recovered_records` | Number of records copied/rebuilt into the repaired archive from the valid source prefix. |
| `recovered_payload_bytes` | Payload bytes preserved in those recovered records. |
| `discarded_tail_bytes` | Physical source-file tail bytes that could not be retained as valid committed archive data. |

These are recovery counters, not an estimate of semantic data quality.

---

# Practical workflows

## Capture, verify, list, and extract

```bash
codec record \
  --archive session.coda \
  --feed news=./input.bin

codec verify session.coda --level full
codec list feeds session.coda

codec extract session.coda --feed news --output recovered.bin

cmp input.bin recovered.bin
```

## Follow a live recording

Terminal 1:

```bash
codec record \
  --archive live.coda \
  --feed live=/path/to/a/growing-source-or-fifo
```

Terminal 2:

```bash
codec extract live.coda --feed live --follow --output live-copy.bin
```

The follower reads only verified committed source records and exits after the recorder commits the final archive index.

## Recover from a damaged tail

```bash
codec verify damaged.coda --level full
codec repair damaged.coda --output repaired.coda
codec verify repaired.coda --level full
```

Keep the original damaged archive if it is evidentiary material; `repair` intentionally produces a separate file.

# Understanding CODEC metrics

CODEC's CLI outputs mostly **integrity, byte-count, and record-count statistics**. They are not throughput/latency benchmarks.

Examples:

- `source_bytes` measures source payload volume preserved during recording.
- `source_records` measures committed source chunks/records.
- `verified_payload_bytes` measures payload volume covered by successful archive verification.
- `valid_prefix_bytes` measures the verified committed file prefix.
- `recovered_records` and `discarded_tail_bytes` describe archive repair results.

v0.3.0 does **not** publish or claim measured:

- capture throughput;
- end-to-end latency;
- distributed worker throughput/latency;
- network availability/durability;
- recovery percentage under real packet loss;
- cloud cost/scale limits;
- GPU performance.

If you need those operational metrics, benchmark the exact workload, machine, storage, network, model, and build you intend to deploy.

---

# C++ library capabilities beyond the CLI

The current development-tree installed `codec::codec` library exposes significantly more than the v0.3.0 command-line program. These APIs are useful to application developers but should not be mistaken for CLI functionality; released versus unreleased scope is called out below.

## Generic archive and stream APIs

Public APIs include:

- CODA writer/archive verification and non-mutating repair;
- generic `StreamId`, stream descriptor, clock, epoch, timing, and gap metadata;
- exact physical record queries by stream/type/sequence/time;
- exact per-record payload extraction;
- S1/D provenance sidecars and direct-provenance queries;
- `StreamAdapter`, `StreamProcessor`, and `StreamExporter` boundaries;
- generic stream recording with caller-owned stable stream IDs.

Unknown compatible 16-bit development-profile record type codes can be preserved/extracted without interpreting their payload.

## Audio Stream Profile

The C++ Audio profile provides, among other APIs:

- deterministic PCM16 `APS1` S1 canonical state;
- preservation-first WAV and native PCM16 FLAC ingest;
- verified PCM16 WAV/FLAC export;
- caller-supplied offline separation orchestration with provenance/reconstruction metrics;
- deterministic `AMB1` separation-model bundles;
- optional ONNX Runtime CPU separation backend when explicitly built and supplied a compatible runtime/model.

No production neural model is bundled.

## Video Stream Profile — Stage H.1

The current C++ tree includes a media-library-independent Video Stream Profile foundation in `<codec/profiles/video.hpp>`:

- deterministic, bounded `VPD1` video descriptors;
- deterministic `VFR1` raw-frame S1 state for Gray8, RGB24, RGBA32, and planar YUV420P8;
- exact profile-local record codes used through CODA's existing raw-code archive boundary;
- a verified reader that returns VFR1 as S1 only when its canonical bytes and exact provenance to committed same-stream S0 records validate;
- raw preservation, extraction, and repair of unknown future profile codes without interpretation.

On top of that foundation, builds configured with `CODEC_ENABLE_FFMPEG_VIDEO=ON` provide the optional bounded FFmpeg ingest bridge described above. It accepts one CODEC-authorized source snapshot, preserves the exact encoded/container bytes as S0, demuxes/decodes only those captured bytes, converts frames to an existing H.1 canonical layout, and emits each successful frame with exact `state_exact` provenance to the same-stream S0 source. The verified reader therefore consumes the bridge output through the same H.1 contract rather than a second FFmpeg-specific state model.

Stage G trust/selective-disclosure work is explicitly deferred and is not claimed complete. Stage H is active at H.1; telemetry, sensor, document/event, network/system, domain schemas, and model-bundle work remain later milestones.

H.1 still does **not** provide GStreamer integration, playback, transcoding/export, a CLI video command or automatic `codec record` decoding, HLS/DASH secondary retrieval, arbitrary FFmpeg protocol access, GPU decode, streaming inference, or a video model. It makes no general codec-compatibility, model-quality, throughput, latency, or scale claim; actual encoded-media support depends on the linked FFmpeg build.

## Stage E transport and recovery

The C++ library includes the bounded Stage E profile:

- deterministic `CMX1` multiplex framing/demultiplexing;
- per-stream sequence-loss observation and explicit recovery groups;
- deterministic `XRF1` XOR repair symbols;
- exact reconstruction of one known missing member when commitments verify;
- `StreamingRepairSession` orchestration that can accept source frames and repair symbols in either order.

This is bounded in-memory transport/recovery logic, not a socket server, retransmission service, or measured network product.

## Stage F.1-F.7 distributed processing

v0.3.0 includes these C++ distributed primitives:

| Stage | Library capability |
|---|---|
| **F.1** | Deterministic partitioning of exact extracted records into bounded one-stream work partitions with stable `CDP1` membership identity. |
| **F.2** | Bounded one-partition/one-worker execution and local `StreamProcessor` worker adapter with exact input and S1/D output validation. |
| **F.3** | Provider-neutral exact record retrieval from caller-described object-store byte ranges. |
| **F.4** | Deterministic in-memory location index and bounded canonical placement candidate resolution. |
| **F.5** | Deterministic synchronous multi-partition scheduling over an ordered worker pool. |
| **F.6** | Provider-neutral `DistributedWorkerTransport` seam and `RemoteDistributedWorker` adapter. |
| **F.7** | Bounded deterministic `DRQ1` request / `DRS1` reply serialization in `<codec/distributed_wire.hpp>`. |

F.7 preserves full request record metadata/payloads and successful `ProcessorOutput`/`ProvenanceProcess` data, with strict bounded canonical decoding. Envelope SHA-256 provides corruption evidence; it is not a signature, MAC, authentication mechanism, or encryption mechanism.

### What Stage F does not yet provide

v0.3.0 does not include:

- a concrete socket, HTTP, HTTPS, QUIC, or gRPC remote-worker implementation;
- a remote worker server loop;
- endpoint/DNS/SSRF policy for worker RPC;
- credentials, authentication, authorization, attestation, or encrypted/replay-resistant worker sessions;
- worker discovery/health or capability negotiation;
- retry/failover, leases, heartbeats, cancellation, idempotency, or exactly-once semantics;
- concurrent/thread-pool distributed scheduling;
- a persistent/global location catalog;
- concrete S3/GCS/Azure object-store clients;
- a distributed-processing CLI.

Applications may supply their own implementations behind the library interfaces.

---

# File-format and compatibility status

The current executable uses a **CODA development profile**, not a frozen normative CODA v1 binary standard. The code deliberately identifies the executable profile separately so that the project can evolve without pretending that this development layout is a permanently frozen interoperability contract.

The same caution applies to development-profile `VPD1`, `VFR1`, `CMX1`, `XRF1`, `CDP1`, `DRQ1`, and `DRS1` structures: they have deterministic tested encodings in this implementation, but the current 0.x line should not be treated as a promise that all future major versions will preserve every development-profile byte layout indefinitely.

Within a given v0.3.0 workflow, integrity checks and exact-source extraction are the relevant guarantees to test.

---

# Security and operational boundaries

CODEC includes integrity and preservation mechanisms, but integrity evidence is not the same as identity, authorization, confidentiality, or operational reliability.

Keep these distinctions in mind:

- SHA-256 archive/frame/envelope hashes detect corruption under their defined structures; an active attacker who can rewrite data can generally recompute an unkeyed hash.
- The CLI blocks private HTTP capture targets by default and refuses redirects, but this does not make arbitrary remote content trustworthy.
- The optional FFmpeg bridge decodes only already captured bytes and blocks libavformat secondary protocol I/O with both a reject-all direct `io_open` hook and an inherited whitelist containing no real FFmpeg URL protocol; that limits authorization expansion but does not make hostile media trustworthy or provide a decoder sandbox.
- Distributed F.1-F.7 labels and wire envelopes do not authenticate workers.
- There is no encrypted remote-worker protocol in v0.3.0.
- There are no published production-scale throughput, availability, or durability guarantees.

Use CODEC only with sources and systems you are authorized to access and preserve.

---

# Release scope: v0.3.0

The v0.3.0 release includes the previously accumulated Stage E.4/E.5 and Stage F.1-F.7 work, plus synchronized CLI/package version reporting. Unreleased Stage H.1 work described in this branch is not part of the already-tagged `v0.3.0` release until it is released under a subsequent version/tag.

For the detailed change history, see [`CHANGELOG.md`](CHANGELOG.md).

The GitHub release is tagged `v0.3.0`.

---

# Developer and repository documentation

This README is the user-facing guide. Repository-maintenance and architecture material lives separately:

- [`CONTRIBUTING.md`](CONTRIBUTING.md) — contributor workflow and architectural contribution rules.
- [`AGENTS.md`](AGENTS.md) — machine-readable repository instructions for repository-aware agents.
- [`AI_WORKSHEET.md`](AI_WORKSHEET.md) — current implementation/verification work record.
- [`docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md`](docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md) — detailed stream-first architecture rationale.
- [Approved reduced-CLI removal design](docs/superpowers/specs/2026-08-30-water%6dark-and-stream-cli-removal-design.md) — explicit superseding removal decision.
- [Stage H.1 Video Profile design](docs/superpowers/specs/2026-08-30-stage-h1-video-profile-design.md) — approved profile boundary and exact-state contract.
- [Stage H.1 implementation plan](docs/superpowers/plans/2026-08-30-stage-h1-video-profile.md) — test-first implementation and verification steps.
- [Optional FFmpeg Video ingest design](docs/superpowers/specs/2026-08-30-video-ffmpeg-ingest-design.md) — preservation-first optional media-integration boundary.
- [Optional FFmpeg Video ingest implementation plan](docs/superpowers/plans/2026-08-30-video-ffmpeg-ingest.md) — TDD and verification sequence for the integration bridge.

A small machine-readable project block is retained here because repository CI verifies version/documentation continuity:

```yaml
project:
  name: CODEC
  version: 0.3.0
  language: C++20
  build: CMake >= 3.20
  archive: CODA (.coda)
  architecture: stream-first, payload-type agnostic
```

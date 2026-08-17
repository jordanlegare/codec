# CODEC

## Channel-Oriented Decomposition, Extraction, and Capture

CODEC is a proposed C++20 engine and API that:

- aggregates multiple authorized internet audio feeds;
- preserves captured source bytes and decoded audio in one self-contained lossless archive;
- separates mixed audio into the greatest defensible number of source tracks;
- tracks anonymous sources over time;
- matches tracks to enrolled identities with calibrated confidence;
- extracts an identity, source, time range, or neural stem on request; and
- evaluates CPU, GPU, latency, fidelity, energy, and neural-quality trade-offs.

The single-file archive is **CODA**, the Channel-Oriented Data Archive, using the **.coda** extension.

This README is the normative implementation specification. It defines guarantees, architecture, archive format, neural and identity policy, C++ and C APIs, CLI usage, validation, and delivery phases.

## Status

| Item | Decision |
|---|---|
| Specification | Implementation-ready draft |
| Implementation | Not yet built |
| Language | C++20 |
| Build | CMake |
| Initial platform | Linux |
| Media layer | FFmpeg libraries behind private adapters |
| Inference | ONNX Runtime |
| Archive mode | Dual preservation by default |
| Neural mode | Offline first; bounded live inference later |

## Capability boundary

CODEC must not turn probabilistic inference into a false identity guarantee.

| Capability | Guarantee | Meaning |
|---|---|---|
| Captured-byte preservation | Deterministic | Stored source bytes hash-identically to bytes accepted at ingest |
| Decoded PCM preservation | Deterministic | Archived PCM reproduces the integer samples accepted by the writer |
| Archive integrity | Deterministic | Hashes, chain, checkpoints, and final index verify or locate damage |
| Feed reconstruction | Deterministic when records are complete | Source packets or lossless PCM can be read without neural inference |
| Source separation | Probabilistic | Estimated stems include residual, quality scores, and model provenance |
| Anonymous continuity | Probabilistic | Stable clusters depend on acoustic and temporal evidence |
| Named identity | Probabilistic and enrollment-dependent | A name requires an authorized identity profile |
| Wideband identity evidence | Experimental | Usable only when the capture actually contains validated bands |
| Recovery of absent information | Impossible | Lossy encoding, filtering, gaps, and missing bands cannot be reversed exactly |

### Fidelity classes

- **S0 — source exact:** accepted encoded bytes, manifests, headers, sequence information, and timing observations.
- **S1 — sample exact:** losslessly compressed integer PCM reproducing decoded samples exactly.
- **D — derived:** analysis audio, resampling, neural stems, embeddings, inferred labels, and enhancements.

Calling a CODA archive lossless means S0 and/or S1 records are preserved without mutation. A lossy internet feed does not become equivalent to its pre-encoding master.

## Requirements language

**MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY** are normative.

## Goals

1. Produce one portable archive containing media, provenance, indexes, models, identities, and audit history needed for offline queries.
2. Preserve known feed boundaries; apply neural separation only to genuinely mixed audio.
3. Support an unknown active-source count up to the declared model and hardware limit.
4. Trace every identity result to intervals, evidence, model hashes, calibration, and confidence.
5. Preserve capture under overload even if inference must defer or stop.
6. Keep GPU inference optional and benchmark it against an equivalent CPU path.
7. Provide stable C++20, C ABI, and CLI surfaces.

## Non-goals

- Circumventing DRM, encryption, access controls, paywalls, or provider restrictions.
- Recording without required authorization, legal basis, or consent.
- Recovering frequencies or independent sources absent from the captured signal.
- Treating a voiceprint as legal proof of identity.
- Hiding uncertainty, residual energy, corruption, or failed checks.
- Kernel-space codecs, neural models, identity logic, or DSP graphs.
- Requiring GPU peer DMA for the first implementation.
- Mutating neural weights during live capture by default.

## Architecture

~~~mermaid
flowchart TD
    A["Authorized feeds"] --> B["Ingest adapters"]
    B --> C["Exact source capture"]
    B --> D["Decode and timeline"]
    C --> E["CODA append writer"]
    D --> E
    D --> F["Bounded analysis queue"]
    F --> G["Separation and diarization"]
    G --> H["Tracking and identity"]
    G --> E
    H --> E
    E --> I["Self-contained .coda"]
    I --> J["Query and extraction API"]
~~~

The engine has two planes:

- **Preservation:** ingest, exact capture, decode, clocks, lossless PCM, append writes, checkpoints, and verification.
- **Inference:** separation, activity, diarization, embeddings, clustering, identity, and derived tracks.

Preservation has priority. Inference MUST NOT block or corrupt capture.

## End-to-end flow

1. The caller supplies authorized feed specifications.
2. Adapters resolve feeds into manifests, renditions, packets, segments, and metadata.
3. Each feed receives an immutable UUID; redirects, formats, clocks, gaps, and reconnects are recorded.
4. Accepted source bytes are appended as S0 records before optional decode.
5. Decoders produce native-rate integer PCM, framed into S1 FLAC chunks without resampling.
6. Timeline mapping retains source PTS, capture monotonic time, and observed UTC separately.
7. A bounded queue creates D-class float tensors at each model’s declared rate.
8. Neural models estimate sources, residual, activity, embeddings, and quality.
9. Tracking stabilizes output permutations and joins observations into anonymous tracks.
10. Identity resolution compares tracks with enrolled profiles and other authorized evidence.
11. Results, claims, scores, and provenance are appended to the archive.
12. Checkpoint indexes keep a growing file recoverable and queryable.
13. Clean close appends a final index and hash-root footer with an optional signature.
14. A user with only the archive can verify, analyze, query, and extract offline.

## Internet feed aggregation

### Protocol scope

| Input | Phase | Preservation |
|---|---:|---|
| File or pipe | 1 | Exact bytes where ownership and stream semantics permit |
| HTTP/HTTPS media | 1 | Body plus sanitized response metadata |
| HLS | 1 | Playlist snapshots, chosen renditions, sequences, discontinuities, and segments |
| Icecast/Shoutcast | 1 | Audio bytes and metadata intervals |
| MPEG-DASH | 2 | MPD snapshots, adaptation sets, initialization data, and segments |
| RTSP/RTP | 2 | Packets, timestamps, sequence, and loss observations |
| Custom provider | 1 extension point | FeedAdapter implementation |

HLS is an unbounded segmented protocol. Playlist sequence, alternate rendition, timing, and discontinuity semantics MUST remain explicit; see [RFC 8216](https://datatracker.ietf.org/doc/html/rfc8216).

### FeedSpec

| Field | Required | Meaning |
|---|:---:|---|
| uri | Yes | Authorized source URI |
| label | Yes | User-facing feed label |
| expected_content | No | Speech, music, mixed, ambience, or unknown |
| rendition_policy | No | Best audio, fixed selector, or all authorized audio renditions |
| credentials_ref | No | External secret reference; raw credentials are not archived |
| reconnect_policy | No | Retry budget, backoff, and terminal conditions |
| headers | No | Request headers with mandatory provenance redaction |
| preserve_source | Yes | Enable S0 records |
| preserve_pcm | Yes | Enable S1 records |

### Network behavior

- Reconnect uses bounded exponential backoff with jitter.
- Each reconnect begins a new connection epoch.
- Missing packets, segments, and timestamps become GAP_EVENT records.
- Concealment for playback or inference is D class and never silently becomes source truth.
- HLS duplicates are detected by source, sequence, byte range, and payload hash.
- Codec or sample-rate changes begin a new format epoch.
- URI schemes, redirects, DNS targets, response size, and timeouts are policy-limited.
- The engine never bypasses DRM or access controls.

## Timeline

Every observation stores:

- archive monotonic nanoseconds;
- capture-host monotonic time;
- observed UTC and uncertainty;
- original source PTS and rational time base;
- source sequence when available;
- format and connection epochs;
- discontinuity, loss, duplicate, and concealment flags.

S1 audio retains its native sample rate and clock. Resampling is D class. Cross-feed alignment uses source timestamps and optional content correlation, recording method and confidence.

## CODA archive format

### Objectives

The .coda container is append-only, self-describing, recoverable, content-verifiable, indexable without a sidecar, large-file safe, and forward-compatible.

### Modes

| Mode | S0 | S1 | Use |
|---|:---:|:---:|---|
| source_only | Yes | No | Lowest overhead; later decode needs codec support |
| pcm_lossless | Metadata only | Yes | Stable sample archive |
| dual | Yes | Yes | Default and most future-proof |

Derived records never replace S0 or S1.

### Capacity formula

Native PCM bytes per hour:

    sample_rate × channels × bytes_per_sample × 3600

At 48 kHz stereo 24-bit, that is 1,036,800,000 bytes per hour before lossless compression. Multiply by feed count and duration. The engine reports projected and actual growth.

### File header

| Field | Purpose |
|---|---|
| magic | Eight-byte CODA signature |
| major_version | Breaking format version |
| minor_version | Compatible feature version |
| archive_uuid | Archive identity |
| created_utc_ns | Creation observation |
| flags | Encryption, self-contained model, live, finalized |
| alignment | Eight bytes in version 1 |
| hash_algorithm | SHA-256 in version 1 |
| header_hash | Integrity of fixed and extension headers |

Version 1 integers are little-endian.

### Record envelope

| Field | Meaning |
|---|---|
| type | Stable record identifier |
| schema_version | Type payload version |
| flags | Compression, encryption, fidelity, continuation |
| sequence | Strictly increasing record sequence |
| stream_uuid | Feed, track, model, or zero for archive-global |
| archive_time_ns | Timeline start |
| duration_ns | Duration or zero |
| payload_bytes | Stored length |
| logical_bytes | Length after record decompression |
| previous_record_hash | Hash-chain predecessor |
| payload_hash | SHA-256 of stored payload |
| extensions | Codec, time base, key, and type metadata |
| payload | Record body |
| commit_trailer | Length, sequence, and CRC32C |

The trailer is written last. Records without a valid trailer are ignored as torn writes.

### Record types

| Record | Class | Purpose |
|---|---|---|
| ARCHIVE_MANIFEST | Metadata | Policy, build, platform, dependencies, configuration |
| SOURCE_DEF | Metadata | Feed identity, sanitized URI, protocol, rendition |
| CONNECTION_EVENT | Metadata | Connect, redirect, retry, error, stop |
| MANIFEST_SNAPSHOT | S0 | HLS, DASH, or provider manifest bytes |
| SOURCE_PACKET | S0 | Exact accepted segment, packet, or byte range |
| FORMAT_EPOCH | Metadata | Codec and stream parameter change |
| PCM_CHUNK | S1 | Native integer PCM compressed with FLAC |
| GAP_EVENT | Metadata | Missing, duplicate, late, corrupt, or concealed interval |
| MODEL_BUNDLE | Metadata | ONNX bytes, manifest, calibration, license, hashes |
| ANALYSIS_AUDIO | D | Optional normalized model input |
| SEPARATED_CHUNK | D | Estimated source stem |
| RESIDUAL_CHUNK | D | Unassigned mixture residual |
| ACTIVITY_CHUNK | D | Activity probabilities |
| EMBEDDING_CHUNK | D | Speaker, source, content, device, or wideband embedding |
| TRACK_EVENT | D | Cluster birth, death, merge, split, reassignment |
| IDENTITY_PROFILE | Metadata | Authorized enrollment and policy |
| IDENTITY_CLAIM | D | Calibrated match and evidence trace |
| QUALITY_METRIC | D | Separation and reconstruction scores |
| CHECKPOINT_INDEX | Metadata | Partial recoverable index |
| AUDIT_EVENT | Metadata | Enrollment, query, extraction, model, or policy action |
| FINAL_INDEX | Metadata | Complete indexes |
| FOOTER | Metadata | Root hash, final sequence, index offset, signature |

### PCM rules

- PCM_CHUNK preserves decoder integer samples at native rate, depth, channel order, and count.
- FLAC is required for version 1 S1 payloads; see the [Xiph FLAC format](https://xiph.org/flac/format.html).
- Chunks SHOULD be 1–10 seconds.
- Priming, padding, delay, and gapless metadata are retained.
- Floating-point source samples are stored as exact IEEE payload records unless explicit quantization policy is selected.

### Embedded indexes

Indexes map:

- sequence to file offset;
- feed and track UUID to time;
- UTC observation to archive time;
- identity UUID and label to claim intervals;
- anonymous cluster to intervals;
- model hash to inference intervals;
- payload hash to record location;
- gaps, discontinuities, damage, and audit events.

### Recovery

On open, the reader validates the header, finds the last valid footer or checkpoint, scans forward, rejects torn records, verifies hashes at the selected level, rebuilds memory indexes, and reports every damaged interval.

Repair writes a new archive. It MUST NOT rewrite the evidence file in place.

### Self-contained mode

When self_contained is true, the archive embeds:

- portable ONNX models required by recorded tasks;
- semantic input/output manifests;
- normalization and feature parameters;
- provider compatibility metadata;
- calibration and thresholds;
- label maps and identity profiles needed for named queries;
- model and dataset license notices;
- engine configuration and schema descriptors;
- model hashes.

Compiled GPU caches MAY be embedded, but portable model bytes remain authoritative.

## Neural decomposition

### Distinct tasks

1. **Separation:** estimate waveforms from a mixture.
2. **Activity:** estimate when each source is active.
3. **Diarization:** determine which anonymous source is active when.
4. **Tracking:** keep a cluster stable between chunks.
5. **Identification:** compare a track with an enrolled identity.

No single score substitutes for all five.

~~~mermaid
flowchart TD
    A["Analysis PCM"] --> B["Activity and scene analysis"]
    B --> C["K-source separator"]
    C --> D["Residual and consistency"]
    C --> E["Embedding extractors"]
    E --> F["Online clustering"]
    F --> G["Permutation tracker"]
    G --> H["Identity evidence fusion"]
    D --> I["Quality gate"]
    H --> J["Claims and indexes"]
    I --> J
~~~

### ModelBundle contract

| Property | Required value |
|---|---|
| task | separation, activity, diarization, embedding, classification, quality |
| model_format | ONNX for portable version 1 |
| model_hash | SHA-256 |
| input_sample_rate | Exact |
| input_channels | Exact or declared dynamic |
| window_samples | Frame length |
| hop_samples | Hop |
| lookahead_samples | Algorithmic lookahead |
| max_sources | Maximum output slots |
| causal | True or false |
| tensor_layout | Names, shapes, types, dynamic axes |
| normalization | Gain, centering, pre-emphasis, channel policy |
| output_semantics | Waveforms, masks, embeddings, probabilities, residual |
| calibration | Dataset, score transform, operating points |
| license | Redistribution and use terms |
| quality_domain | Speech, music, broadcast, general, or declared subset |

ONNX Runtime supplies a C++ API and hardware execution-provider boundary. CODEC can evaluate CPU, CUDA, TensorRT, OpenVINO, MIGraphX, and other available providers without changing archive semantics. See [ONNX Runtime C++](https://onnxruntime.ai/docs/get-started/with-cpp.html) and its [execution providers](https://onnxruntime.ai/docs/execution-providers/).

### Unknown source count

“As many channels as possible” means bounded evidence-driven decomposition:

1. Estimate active-source count distribution.
2. Run up to max_sources slots.
3. Suppress a slot only when activity and energy tests agree.
4. Optionally recurse on residual structure.
5. Stop at model, compute, latency, confidence, or energy limits.
6. Keep the residual as an explicit output.
7. Store estimated count, configured maximum, stop reason, and confidence.

The engine MUST NOT claim more independent sources than the selected model supports.

### Mixture consistency

For every window:

    mixture ≈ sum(separated_sources) + residual

Record absolute error, relative energy error, clipping, empty-slot rate, and confidence. Failed outputs remain available as low-confidence D data but are excluded from automatic identity claims by default.

### Continuity

Output slots are stabilized using:

- speaker or source embedding similarity;
- overlap-region waveform similarity;
- activity continuity;
- spatial and channel features;
- content fingerprints;
- source-count transitions;
- feed metadata;
- costed split, merge, birth, and death events.

Every reassignment creates TRACK_EVENT; history is never silently rewritten.

### Live “feel”

The requested “feel for the feed” is a stateful, auditable belief model, not intuition. Live state may update cluster centroids, uncertainty, temporal probabilities, feed noise and codec profiles, and clock alignment. Base neural weights do not mutate during capture. Offline fine-tuning produces a new versioned ModelBundle.

## Identity

### Identity types

| Type | Example | Naming rule |
|---|---|---|
| Feed | station-east | May derive from configured metadata |
| Anonymous source | cluster:7f14… | Available after tracking |
| Enrolled person/source | person:alice | Requires authorized reference |
| Content | program:night-news | Requires fingerprint or classifier profile |
| Device/path | device:studio-a | Experimental; insufficient for person identity |

### Enrollment profile

An IDENTITY_PROFILE contains:

- identity UUID, label, and type;
- authorized archive intervals or imported references;
- embeddings with model hashes;
- consent, purpose, retention, and access metadata;
- calibration population and threshold;
- creation, revision, and revocation events;
- optional known feed or content associations.

If no profile or authoritative mapping exists for a requested name, the API returns identity_not_enrolled. It may suggest anonymous clusters but MUST NOT invent a name.

### Evidence fusion

Claims may combine:

- acoustic embedding similarity;
- cluster continuity;
- known-feed association;
- content fingerprint;
- spatial/channel continuity;
- codec, device, or path signature;
- time and schedule evidence;
- user-confirmed labels;
- validated wideband evidence.

Each contribution stores its model hash, calibration, score, interval, frequency band, and exclusion reason. A probability is not legal proof.

### Outside-hearing-range evidence

- A signal contains nothing at or above its Nyquist limit.
- 44.1 kHz input cannot represent 22.05 kHz or above; 48 kHz cannot represent 24 kHz or above.
- Internet codecs and upstream processing often remove high-frequency energy.
- The actual format, spectrum, and path are inspected before wideband features are enabled.
- Missing bands remain missing; generated frequencies cannot support identity.
- Wideband evidence is D class, experimental, and disabled by default.
- It requires cross-device and cross-codec calibration.
- No named claim may rely solely on wideband evidence.
- Every claim states the exact bands used.
- CODEC emits no ultrasonic probe and performs no active covert sensing.

### Default claim levels

| Calibrated probability | Label |
|---:|---|
| Below 0.50 | no_match |
| 0.50–0.75 | candidate |
| 0.75–0.90 | probable_candidate |
| 0.90–0.98 | strong_candidate |
| At least 0.98 | very_strong_candidate |

These are defaults, not universal constants. Uncalibrated models return scores, never probabilities.

### Claim trace

Every claim records identity, anonymous track, intervals, score or probability, threshold, evidence, frequency bands, model and source hashes, separation quality, alternatives, enrollment revision, creation time, and revocation/supersession link.

## C++ components

| Component | Responsibility | Execution |
|---|---|---|
| FeedRegistry | Validate FeedSpec | Control thread |
| FeedAdapter | Protocol acquisition | Network pool |
| SourceRecorder | S0 records | Never waits on inference |
| MediaDecoder | FFmpeg decode | Decoder pool |
| Timeline | Clock and gap model | Lock-bounded |
| PcmArchiver | FLAC S1 chunks | Compression pool |
| CodaWriter | Ordered append and checkpoints | Dedicated writer |
| AnalysisScheduler | Bounded work and degradation | Nonblocking producer |
| ModelRuntime | ONNX sessions/providers | Inference pool |
| Separator | Stems, residual, quality | Offline/bounded live |
| TrackManager | Clusters and permutation | Single owner |
| IdentityResolver | Enrollment and fusion | Inference pool |
| CodaReader | Verify and retrieve | Concurrent readers |
| QueryEngine | Resolve predicates | Read-only |
| Extractor | Exact/derived exports | Export pool |
| BenchmarkRunner | CPU/GPU/fidelity evaluation | Isolated run |

### Dependency rules

- Public headers expose no FFmpeg, ONNX, CUDA, HIP, or vendor types.
- Media and inference libraries live behind private adapters.
- Archive reading builds without a GPU runtime.
- A minimal reader SHOULD not require FFmpeg.
- Resource ownership uses RAII.
- Exceptions never cross the C ABI.

### Repository layout

~~~text
CMakeLists.txt
cmake/
include/codec/
  archive.hpp
  engine.hpp
  error.hpp
  feed.hpp
  identity.hpp
  model.hpp
  query.hpp
  types.hpp
src/
  archive/
  capture/
  cli/
  core/
  decode/
  identity/
  inference/
  query/
  timeline/
models/manifests/
schemas/
tests/
  corpus/
  fuzz/
  integration/
  unit/
benchmarks/
docs/
~~~

### Dependencies

| Dependency | Purpose |
|---|---|
| CMake | Build and package |
| FFmpeg libavformat/libavcodec/libavutil | Protocols, demux, decode |
| FLAC | S1 compression |
| ONNX Runtime | Portable inference |
| OpenSSL or audited equivalent | SHA-256 and signatures |
| fmt-compatible formatting layer | Internal formatting |
| Structured logging sink | Diagnostics |
| GoogleTest or Catch2 | Development tests |

Use FFmpeg public libraries, not parsed CLI subprocess output. See [libavformat](https://ffmpeg.org/doxygen/trunk/group__libavf.html) and [FFmpeg protocols](https://ffmpeg.org/ffmpeg-protocols.html). Pin versions in the build and store them in ARCHIVE_MANIFEST.

## C++ API

### Core types

~~~cpp
namespace codec {

using TimeNs = std::int64_t;

struct StreamId { std::array<std::byte, 16> value; };
struct TrackId { std::array<std::byte, 16> value; };
struct IdentityId { std::array<std::byte, 16> value; };

enum class FidelityClass { source_exact, sample_exact, derived };
enum class ArchiveMode { source_only, pcm_lossless, dual };
enum class InferenceMode { disabled, offline, bounded_live };

struct Confidence {
  double score;
  bool calibrated;
  std::string calibration_id;
};

template<class T>
class Result;

}
~~~

Result is a C++20-compatible value-or-error type owned by CODEC. Its storage implementation is private and may adopt std::expected when the compiler baseline advances.

### Configure

~~~cpp
codec::EngineConfig config;
config.archive.mode = codec::ArchiveMode::dual;
config.archive.self_contained = true;
config.archive.checkpoint_interval = std::chrono::seconds{30};
config.archive.pcm_chunk_duration = std::chrono::seconds{5};

config.inference.mode = codec::InferenceMode::bounded_live;
config.inference.provider_order = {"TensorRT", "CUDA", "CPU"};
config.inference.max_sources = 8;
config.inference.max_queue_audio = std::chrono::seconds{60};
config.inference.on_overload = codec::OverloadPolicy::defer_to_offline;

config.identity.minimum_claim_probability = 0.90;
config.identity.require_calibrated_scores = true;
config.identity.allow_wideband_features = false;
~~~

### Record feeds

~~~cpp
#include <codec/engine.hpp>

int main() {
  auto engine = codec::Engine::create(config);
  if (!engine) return 1;

  std::vector<codec::FeedSpec> feeds{
      {.uri = "https://example.net/live/news.m3u8",
       .label = "news",
       .preserve_source = true,
       .preserve_pcm = true},
      {.uri = "https://radio.example.org/stream",
       .label = "radio",
       .preserve_source = true,
       .preserve_pcm = true}
  };

  auto session = engine->start_recording(feeds, "session.coda");
  if (!session) return 2;
  session->wait();
  return session->finalize() ? 0 : 3;
}
~~~

### Open and verify

~~~cpp
auto archive = codec::Archive::open(
    "session.coda",
    {.verification = codec::VerificationLevel::full});

if (!archive) {
  report(archive.error());
  return;
}

print(archive->verification_report());
~~~

### Enroll from archive intervals

~~~cpp
codec::EnrollmentRequest request;
request.label = "speaker-alice";
request.type = codec::IdentityType::person;
request.references = {
    {.stream = codec::StreamSelector::label("news"),
     .start = codec::seconds(120),
     .end = codec::seconds(180)}
};
request.consent_reference = "local-policy-record-42";

auto identity = archive->enroll(request);
~~~

Enrollment requires writable mode and appends IDENTITY_PROFILE plus AUDIT_EVENT.

### Query and extract

~~~cpp
codec::IdentityQuery query;
query.identity = codec::IdentitySelector::label("speaker-alice");
query.minimum_probability = 0.92;
query.require_mixture_consistency = true;
query.time_range = codec::TimeRange::all();

auto matches = archive->find(query);
if (!matches) {
  handle(matches.error());
  return;
}

codec::ExtractOptions options;
options.format = codec::ExportFormat::flac;
options.layout = codec::ExportLayout::continuous_with_gap_manifest;
options.include_provenance_json = true;

auto result = archive->extract(
    *matches, "speaker-alice.flac", options);
~~~

Neural exports are D class. Exact S1 extracts are labeled sample_exact only when no transform occurred.

### Anonymous sources

~~~cpp
auto clusters = archive->anonymous_tracks({
    .minimum_active_time = codec::seconds(10),
    .minimum_track_confidence = 0.80
});

for (const auto& cluster : *clusters) {
  print(cluster.id, cluster.active_time, cluster.best_candidate);
}
~~~

### Async analysis

~~~cpp
codec::StopSource stop;

auto task = archive->analyze_async(
    {.mode = codec::AnalysisMode::full_offline},
    stop.token(),
    [](const codec::Progress& p) {
      print(p.stage, p.completed, p.total);
    });

auto result = task.get();
~~~

### Errors

Errors contain stable code, category, message, retryability, stream/track/record/model/time context, safe underlying code, and redacted diagnostics.

Categories: invalid_argument, unauthorized_source, network, protocol, decode, archive_io, archive_corrupt, model_incompatible, inference, identity_not_enrolled, identity_uncalibrated, cancelled, resource_exhausted, internal.

## C ABI

~~~c
typedef struct codec_engine codec_engine_t;
typedef struct codec_archive codec_archive_t;
typedef struct codec_error codec_error_t;

int codec_engine_create(
    const codec_engine_config_t* config,
    codec_engine_t** out,
    codec_error_t** error);

int codec_archive_open(
    codec_engine_t* engine,
    const char* path,
    const codec_open_options_t* options,
    codec_archive_t** out,
    codec_error_t** error);

int codec_archive_query_identity(
    codec_archive_t* archive,
    const codec_identity_query_t* query,
    codec_match_list_t** out,
    codec_error_t** error);

int codec_archive_extract(
    codec_archive_t* archive,
    const codec_match_list_t* matches,
    const codec_extract_options_t* options,
    codec_error_t** error);
~~~

Handles have explicit destroy functions. Structs carry size and ABI version fields.

## CLI usage

### Build

~~~bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
~~~

### Record

~~~bash
codec record --archive session.coda --mode dual --self-contained --feed news=https://example.net/live/news.m3u8 --feed radio=https://radio.example.org/stream --model-bundle models/default --inference bounded-live
~~~

### Inspect and verify

~~~bash
codec inspect session.coda
codec verify session.coda --level full
codec list feeds session.coda
codec list tracks session.coda --include-anonymous
codec list gaps session.coda
~~~

### Offline analysis

~~~bash
codec analyze session.coda --mode full-offline --provider CUDA,CPU --max-sources 8 --checkpoint-every 30s
~~~

Analysis appends versioned D records; it never deletes earlier model output.

### Enroll

~~~bash
codec enroll session.coda --label speaker-alice --type person --from-feed news --from 00:02:00 --to 00:03:00 --consent-ref local-policy-record-42
~~~

### Find and extract

~~~bash
codec query session.coda --identity speaker-alice --min-probability 0.92 --require-consistent-mixture --json matches.json

codec extract session.coda --query matches.json --output speaker-alice.flac --provenance speaker-alice.provenance.json
~~~

### Extract a known feed without neural inference

~~~bash
codec extract session.coda --feed news --fidelity sample-exact --output news.flac --provenance news.provenance.json
codec extract session.coda --feed news --fidelity source-exact --output news-source-records.coda --provenance news-source-records.json
~~~

The first command exports exact S1 samples for the named feed. The second creates a smaller valid CODA archive containing the feed’s selected S0 records, manifests, timing, gaps, hashes, and provenance.

### Anonymous extraction

~~~bash
codec extract session.coda --track cluster:7f14c0a2 --output unknown-source.flac --provenance unknown-source.json
~~~

### Repair and benchmark

~~~bash
codec repair damaged.coda --output recovered.coda
codec verify recovered.coda --level full

codec benchmark --archive validation.coda --providers CPU,CUDA,TensorRT --report benchmark.json
~~~

## Configuration

~~~toml
[archive]
mode = "dual"
self_contained = true
checkpoint_seconds = 30
pcm_chunk_seconds = 5
verification = "full"

[capture]
max_feeds = 16
reconnect_attempts = 20
max_redirects = 5
private_network_policy = "deny"

[inference]
mode = "bounded-live"
providers = ["TensorRT", "CUDA", "CPU"]
max_sources = 8
max_queue_audio_seconds = 60
overload = "defer-to-offline"

[identity]
minimum_claim_probability = 0.90
require_calibrated_scores = true
allow_wideband_features = false

[privacy]
archive_request_headers = false
encrypt_identity_profiles = true
audit_queries = true
~~~

CLI values override the file. Secrets are external references and never plaintext archive metadata.

## Threading and backpressure

- One control loop owns lifecycle.
- Network I/O uses a bounded async pool.
- Decode and FLAC use bounded worker pools.
- One writer owns archive ordering.
- Stateful trackers are single-owner.
- Inference has a scheduler per provider and bounded device memory.
- Readers are concurrent and immutable.

| Queue | Drop? | Overload |
|---|:---:|---|
| Accepted bytes → S0 | No | Backpressure if safe; otherwise explicit gap/error |
| PCM → S1 | No in PCM modes | Slow when possible; preserve S0 and report failure |
| PCM → live inference | Yes | Defer to offline |
| Accepted derived result → writer | No | Bound producer or cancel analysis |
| Debug metrics | Yes | Count and report dropped metrics |

Neural work cannot starve committed source capture.

## GPU evaluation

Version 1 path:

    feed → CPU network buffer → S0
         → CPU decode → native PCM/S1
         → pinned staging or runtime tensor
         → GPU inference
         → D records
         → CODA writer

DMA-BUF and peer DMA are optional later experiments and do not affect archive correctness.

### Fair comparison

CPU and GPU runs use identical archive intervals, weights, window/hop/overlap, thresholds, warm-up policy, quality tests, and repetitions. Runtime, provider, driver, and device versions are recorded.

| Category | Metrics |
|---|---|
| Throughput | Real-time factor, streams/device, samples/second |
| Latency | Median, p95, p99, p99.9, maximum |
| Deadlines | Misses, worst margin, queue depth |
| Quality | SI-SDR improvement, residual, reconstruction, identity |
| Resources | CPU, GPU, VRAM, RAM, host/device bytes |
| Energy | Wall energy per processed audio hour |
| Stability | Resets, retries, skipped or corrupted intervals |

GPU advances only with a measured throughput, CPU, latency, or energy benefit and no integrity or quality violation.

## Observability

Structured events include archive/session/feed/track UUIDs, model hash, stage, queue, source/archive time, latency, bytes, samples, gaps, overload, calibration, and redacted error context.

JSON Lines logs are required; OpenTelemetry-compatible metrics are optional. Credentials, keys, reference audio, and sensitive query text are excluded from logs.

## Security, privacy, and responsible use

- Capture and identification require authorization and lawful basis.
- Enrollment requires provenance and policy metadata.
- Identity profiles SHOULD be encrypted.
- Keys are never stored beside ciphertext in plaintext.
- Queries and exports are audited.
- Service deployments SHOULD enforce role and purpose restrictions.
- Revocation appends an event; it never silently rewrites evidence.
- URI handling is hardened against SSRF, redirect abuse, credential leaks, bombs, and malformed media.
- Codecs, parsers, models, record readers, and queries are fuzzed.
- CODEC performs no covert active ultrasonic sensing.
- It must not be marketed as forensic proof without domain validation, uncertainty, and qualified review.

## Validation

### Archive

1. S0 round-trips byte-for-byte.
2. S1 round-trips sample-for-sample.
3. Final archives verify records, chain, index, and root.
4. Truncation at every byte retains only complete records.
5. Checkpoint rebuild equals clean final indexing.
6. Unknown compatible records are skipped safely.
7. Offsets and sequences work beyond 4 GiB and 32-bit limits.
8. Duplicates, gaps, and discontinuities remain explicit.

### Separation

- SI-SDR and improvement;
- interference/artifact ratios where applicable;
- mixture reconstruction error;
- residual energy and structure;
- empty-slot false positives;
- active-source count accuracy;
- permutation continuity errors;
- real-time factor and tail latency;
- results by source count, SNR, codec, sample rate, and domain.

### Diarization and identity

- diarization and Jaccard error rates;
- missed activity and false alarm;
- confusion, fragmentation, and incorrect merges;
- equal-error rate;
- false-match/non-match at thresholds;
- top-k identification;
- retrieval precision/recall;
- calibration error and Brier score;
- open-set rejection;
- subgroup results when people are identified.

### Streaming

- 24-hour multi-feed run;
- reconnect storms;
- slow/malicious endpoints;
- HLS discontinuity/rendition changes;
- codec/rate changes;
- loss and reordering;
- GPU overload/reset;
- disk stall/out-of-space;
- process/host termination;
- concurrent query of a growing archive.

### Fidelity

- digital bit comparison;
- null tests for D resampling;
- clipping and inter-sample peaks;
- frequency/phase response;
- THD+N, SINAD, IMD, noise, crosstalk, jitter for analog tests;
- exact declaration of quantization and resampling.

### Fuzzing

Targets: CODA header/envelope/index/trailer, schemas, identity graph, model manifest, query parser, C ABI versioning, FLAC corruption, and protocol adapters.

## Acceptance criteria

### Archive MVP

- A 24-hour interrupted multi-feed run produces one recoverable file.
- Every accepted S0 payload verifies.
- Every S1 interval round-trips.
- Recovery loses no fully committed record.
- Required queries need no sidecar.

### Neural MVP

- Portable ONNX bundles load from the archive.
- CPU is the reference.
- At least one GPU provider stays within declared numeric tolerance.
- Separation always emits residual and consistency report.
- Unknown-count output records max_sources, confidence, and stop reason.

### Identity MVP

- Anonymous tracks work without enrollment.
- Named queries fail clearly without profiles.
- Enrolled queries return intervals, calibrated confidence, alternatives, and trace.
- No claim relies only on metadata or wideband evidence.
- Revoked profiles are excluded by default.

### API MVP

- Documented record/open/verify/analyze/enroll/query/extract flows compile.
- C ABI ownership/version tests pass under sanitizers.
- Cancellation leaves a valid archive.
- Errors leak no secrets.

### Performance MVP

- Preservation remains independent of inference.
- Live overload defers analysis without losing committed source.
- CPU/GPU reports use equivalent inputs and include quality, tail latency, memory, and energy.

## Delivery plan

### Phase 0 — Archive core

- Freeze CODA v1 schemas.
- Implement writer, scanner, chain, checkpoints, final index, verifier.
- Add property, crash, and fuzz tests.
- Deliver a dependency-light reader.

### Phase 1 — Capture and PCM

- Add file/HTTP, HLS, and Icecast.
- Integrate FFmpeg private wrappers.
- Preserve S0 and S1 FLAC.
- Add timeline, reconnect, gaps, and epochs.
- Pass 24-hour archive test.

### Phase 2 — Inference runtime

- Implement ModelBundle.
- Integrate ONNX Runtime CPU.
- Discover and select GPU providers.
- Add tensors, bounded queues, cancellation, and D records.

### Phase 3 — Separation and tracking

- Add activity, K-source separation, residual, and consistency.
- Add embeddings, clustering, permutation tracking, and anonymous queries.
- Validate offline before bounded live mode.

Candidate families include [Conv-TasNet](https://arxiv.org/abs/1809.07454), [SepFormer](https://arxiv.org/abs/2010.13154), [EEND diarization](https://arxiv.org/abs/1909.06247), and [ECAPA-TDNN embeddings](https://www.isca-archive.org/interspeech_2020/desplanques20_interspeech.html). These are references, not fixed endorsements.

### Phase 4 — Identity and extraction

- Add enrollment and revisions.
- Add calibrated fusion and open-set rejection.
- Add claim trace, revocation, queries, extraction, and provenance.
- Keep wideband evidence disabled until independently validated.

### Phase 5 — Live and GPU

- Add bounded live inference and offline catch-up.
- Reuse pinned memory and tensors.
- Benchmark CPU, CUDA, TensorRT, OpenVINO, and MIGraphX where available.
- Degrade inference without weakening preservation.

### Phase 6 — Optional transport

- Evaluate DMA-BUF and zero-copy.
- Evaluate a custom PCIe audio endpoint.
- Advance only if measured value exceeds complexity, portability, synchronization, and power costs.

## Stop/redesign gates

- One-file recovery cannot survive torn writes.
- Required queries need an external database.
- Sample-exact output cannot be proven.
- Model bundles are not portable or license-compliant.
- Separation fails consistency thresholds.
- Open-set false matches exceed the risk budget.
- Wideband identity fails cross-device/cross-codec validation.
- GPU improves averages but harms tail stability.
- Inference can starve preservation.
- The use case lacks authorization or consent.

## Bounded implementation decisions

| Decision | Default | Alternative |
|---|---|---|
| Schema | Canonical CBOR | FlatBuffers or Protobuf |
| Hash | SHA-256 | Compatible accelerated provider |
| Index | Immutable sorted blocks + bloom filters | B-tree pages |
| Signature | Optional Ed25519 footer | Key-provider adapter |
| Encryption | Optional per-record AEAD | Whole-file envelope rejected for recovery |
| Tests | GoogleTest | Catch2 |
| Dependency lock | vcpkg manifest | Conan lockfile |

Each closes through benchmark, threat review, or compatibility test.

## Principles

- Preserve first; infer second.
- Exact and derived data never share an unlabeled path.
- A model output is evidence, not truth.
- Unknown is valid.
- Residual is mandatory.
- Every identity has provenance and uncertainty.
- No band is used unless it exists in the capture.
- GPU optimization cannot weaken correctness.
- Repair never mutates the source.
- One file means required models and indexes are embedded.

## References

- [FFmpeg libavformat](https://ffmpeg.org/doxygen/trunk/group__libavf.html)
- [FFmpeg protocols](https://ffmpeg.org/ffmpeg-protocols.html)
- [HTTP Live Streaming, RFC 8216](https://datatracker.ietf.org/doc/html/rfc8216)
- [Xiph FLAC format](https://xiph.org/flac/format.html)
- [ONNX Runtime C++](https://onnxruntime.ai/docs/get-started/with-cpp.html)
- [ONNX Runtime execution providers](https://onnxruntime.ai/docs/execution-providers/)
- [Conv-TasNet](https://arxiv.org/abs/1809.07454)
- [SepFormer](https://arxiv.org/abs/2010.13154)
- [End-to-End Neural Speaker Diarization](https://arxiv.org/abs/1909.06247)
- [ECAPA-TDNN](https://www.isca-archive.org/interspeech_2020/desplanques20_interspeech.html)

## License

No software license has been granted. Add LICENSE before accepting contributions or distributing an implementation. Model, dataset, codec, and provider licenses remain independently binding and are stored in each ModelBundle.

# CODEC

## Channel-Oriented Decomposition, Extraction, and Capture

CODEC is a C++20 engine, API, and staged implementation specification that:

- aggregates multiple authorized internet audio feeds;
- preserves captured source bytes and decoded audio in one self-contained lossless archive;
- separates mixed audio into the greatest defensible number of source tracks;
- tracks anonymous sources over time;
- detects signed dual-band feed watermarks and reports verified feed identity live;
- matches tracks to enrolled identities with calibrated confidence;
- extracts an identity, source, time range, or neural stem on request; and
- evaluates CPU, GPU, latency, fidelity, energy, and neural-quality trade-offs.

The single-file archive is **CODA**, the Channel-Oriented Data Archive, using the **.coda** extension.

This README is the normative implementation specification. It defines guarantees, architecture, archive format, neural and identity policy, C++ and C APIs, CLI usage, validation, and delivery phases.

## Status

| Item | Current repository state |
|---|---|
| Specification | Normative staged product specification |
| Implementation | v0.1.0 executable preservation and signed-watermark MVP |
| Language | C++20 |
| Build | CMake 3.20+; GCC and Clang CI |
| Initial platform | Linux, with a portable public API |
| Media layer | File/stdin/HTTP(S) S0 capture and PCM16 WAV; FFmpeg adapters are a later phase |
| Inference | Backend interface shipped; no neural weights or ONNX runtime bundled yet |
| Archive mode | Source-exact S0 development profile implemented; S1 FLAC and full self-contained profile planned |
| Neural mode | Explicitly unavailable until a compatible ModelBundle is installed |
| Watermark mode | Ed25519 COSE W0 + reference sub-20 kHz W1 + sample-rate-gated reference W2 |

### v0.1.0 implemented product surface

The repository now builds a library and `codec` executable. The implemented slice is intentionally honest about what it can prove:

| Surface | v0.1.0 behavior |
|---|---|
| CODA | Append-only development profile with fixed header, ordered records, SHA-256 payload/chain evidence, commit trailers, final index, verification, exact S0 extraction, and non-mutating repair |
| Capture | Bounded file, stdin, HTTP, and HTTPS entity-body capture; sanitized descriptors; local descriptors are secured before archive creation; resolved addresses must be globally routable; implicit proxies and redirects are refused under the default private-network policy |
| Audio | Sample-exact integer PCM16 RIFF/WAVE read and write |
| W0 | Canonical-CBOR COSE_Sign1 statements signed and verified with Ed25519; validity windows and key identifiers enforced |
| W1 | Low-amplitude reference binary-FSK carrier below 20 kHz with CRC, preamble, repeated frames, and Goertzel correlation detection |
| W2 | Reference carrier above 24 kHz; automatically rejected below 96 kHz or without the configured Nyquist guard |
| Identity event | JSON Lines candidates; three matching hops plus a valid W0 produce `signature_bound_candidate`, never authoritative `verified_feed` in the stateless reference detector |
| APIs | C++20 archive/engine/audio/watermark interfaces and a size/version-checked C ABI |
| Neural separation | Stable backend boundary that returns `model_incompatible`; no fabricated stems or identity claims |

The W1 carrier is a measurable reference implementation, not a claim of perceptual transparency. W2 passing the digital sample-rate gate is not hardware path qualification. Replay-safe authoritative `verified_feed` requires the later stateful fusion layer. The later delivery phases remain normative requirements for the full engine.

### Quick start

Install CMake, a C++20 compiler, OpenSSL 3 development files, and libcurl development files, then build and test:

~~~bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/codec capabilities
~~~

Output paths must be new. v0.1.0 uses exclusive, no-follow creation and refuses to replace any existing file, symlink, hard-link name, archive, input, or key. Local capture files are opened without following symlinks and their descriptors are retained before the archive is created, closing pathname races and self-capture aliases. With the default private-network policy, HTTP(S) capture also disables environment proxies, rejects non-globally-routable resolved addresses, and refuses redirects.

Capture any authorized local file or HTTP(S) response body into one source-exact archive, verify it, and recover the feed bytes:

~~~bash
./build/codec record --archive session.coda \
  --feed news=https://example.net/authorized-feed \
  --feed backup=/srv/audio/backup-stream.bin
./build/codec verify session.coda --level full
./build/codec list feeds session.coda
./build/codec extract session.coda --feed news --fidelity source-exact \
  --output news-source.bin
~~~

Issue and detect a signed W1 derivative while leaving the input WAV unchanged:

~~~bash
issued_at=$(date +%s)
expires_at=$((issued_at + 3600))
./build/codec watermark keygen --private issuer.key --public issuer.pub
./build/codec watermark issue input.wav --output marked.wav \
  --statement feed.cose --private-key issuer.key \
  --feed-uuid 7c2b2f74-7e31-4a1d-b469-d88d63fc8fcb \
  --code 0x4a31 --issuer station-7 --key-id station-7-2026 \
  --issued-at "$issued_at" --not-before "$issued_at" \
  --expires-at "$expires_at" --w1
./build/codec watermark detect marked.wav --statement feed.cose \
  --public-key issuer.pub --format jsonl
~~~

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
| Signed feed-watermark statement | Cryptographically verifiable | A decoded code resolves to a valid signed feed statement or it does not |
| Acoustic watermark survival | Probabilistic | Survival depends on codec, filtering, mixing, gain, noise, resampling, and hardware |
| Live watermark detection | Probabilistic until signature validation | Neural or correlation peaks are candidates until a signed statement validates |
| Wideband identity evidence | Experimental | Usable only when the capture actually contains validated bands |
| Recovery of absent information | Impossible | Lossy encoding, filtering, gaps, and missing bands cannot be reversed exactly |

### Fidelity classes

- **S0 — source exact:** accepted encoded bytes, manifests, headers, sequence information, and timing observations.
- **S1 — sample exact:** losslessly compressed integer PCM reproducing decoded samples exactly.
- **D — derived:** analysis audio, resampling, neural stems, embeddings, inferred labels, and enhancements.

Calling a CODA archive lossless means S0 and/or S1 records are preserved without mutation. A lossy internet feed does not become equivalent to its pre-encoding master.

## Generalized CODA platform direction

CODEC is implemented audio-first, but CODA is intended to evolve into a **general authenticated temporal container** for heterogeneous machine-observed streams. This direction is normative for future interface design; it does **not** claim that the generalized record model is implemented in v0.1.0.

Audio remains the first specialization. Future CODA-compatible logical streams MAY include video, image/frame sequences, telemetry, sensor observations, navigation/position data, radar/lidar/sonar-derived payloads, documents and text, transcript segments, network/system events, model inputs and outputs, embeddings, identity evidence, recovery/FEC shards, audit/access events, authorization/policy records, and opaque provider payloads.

A logical stream identity MUST be independent of transport port, process ID, worker assignment, file name, or physical carrier frequency. Stream identity SHOULD survive reconnects, worker migration, cloud-region changes, archive segmentation, transport changes, and offline re-analysis.

The generalized conceptual record envelope is:

~~~text
RecordEnvelope {
  stream_id
  stream_type
  source_id
  monotonic_time
  observed_utc
  source_timestamp
  sequence
  format_epoch
  connection_epoch
  fidelity_class
  payload_type
  payload
  payload_hash
  previous_record_hash
  provenance
  policy_tags
}
~~~

This is a semantic model, not a frozen binary layout. Any future archive-format implementation MUST follow CODA versioning and compatibility rules, preserve unknown compatible record types, and retain the existing source/derived truth boundary.

The fidelity model extends conservatively:

- **S0 remains source exact.** Original accepted bytes, packets, manifests, headers, sequencing, and transport observations remain immutable source truth.
- **S1 remains exact only where an exact normalized representation is rigorously defined.** Audio S1 means sample-exact integer PCM. Non-audio S1-like state MUST NOT be claimed without a deterministic representation and round-trip test.
- **D remains derived.** Inference, resampling, translation, summarization, enhancement, interpolation, reconstruction, separation, embeddings, classifications, and model-generated outputs MUST remain distinguishable from S0/S1.

Every D-class result SHOULD be traceable to source stream IDs and intervals, model/runtime identity and hash, configuration, inference time, calibration where applicable, confidence/quality, transformation chain, and residual or uncertainty information where meaningful. New models MAY append new interpretations later without rewriting source history.

Unknown stream types MUST remain preservable even when the current engine cannot interpret them. This property is required so CODA can evolve without forcing every reader to understand every vertical or future payload type.

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
8. Detect signed feed watermarks live and emit low-latency identity events with provenance.
9. Preserve original audio separately from any watermark inserted by CODEC.
10. Use watermark identity to condition separation without calling a neural stem lossless.

## Non-goals

- Circumventing DRM, encryption, access controls, paywalls, or provider restrictions.
- Recording without required authorization, legal basis, or consent.
- Recovering frequencies or independent sources absent from the captured signal.
- Treating a voiceprint as legal proof of identity.
- Hiding uncertainty, residual energy, corruption, or failed checks.
- Kernel-space codecs, neural models, identity logic, or DSP graphs.
- Requiring GPU peer DMA for the first implementation.
- Mutating neural weights during live capture by default.
- Claiming an acoustic watermark survives every transform or is permanent outside CODA.
- Calling a sub-20 kHz carrier outside human hearing; it remains in the nominal audible band even when masked.
- Emitting audible spikes, unsafe ultrasonic energy, or covert active beacons.

## Architecture

~~~mermaid
flowchart TD
    A["Authorized feeds"] --> B["Ingest adapters"]
    B --> C["Exact source capture"]
    B --> D["Signed byte watermark"]
    B --> E["Decode and timeline"]
    D --> F["Watermark fusion"]
    E --> G["Audio watermark detector"]
    G --> F
    E --> H["Analysis queue"]
    F --> I["Live feed identity"]
    H --> J["Conditioned separation"]
    I --> J
    C --> K["CODA append writer"]
    I --> K
    J --> K
    K --> L["Self-contained .coda"]
    L --> M["Query and extraction API"]
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

### Watermark-aware byte-to-audio path

    IP packet, HTTP body, or media segment
      → exact S0 append
      → signed metadata watermark parse
      → demuxed encoded frame
      → native-rate S1 PCM decode
      → sub-20 kHz and optional above-24 kHz detection
      → evidence and signature fusion
      → live feed-identity event
      → identity-conditioned separator
      → D-class stems, residual, and claim trace

The neural network does not reinterpret encoded bytes as lossless audio. Bytes are preserved first, then decoded deterministically, and only then analyzed. Neural output remains D class even when feed identity is cryptographically verified.

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
| watermark_policy | No | detect, require_verified, or ignore |
| trusted_issuers | No | Public-key trust-set reference for W0 validation |

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

## Live watermark and feed identity

### Frequency interpretation

This specification interprets “24000k” as **24,000 Hz (24 kHz)**. A value of 24,000 kHz is 24 MHz and is not an audio-band requirement.

The requested watermark uses two acoustic bands, but only the second is outside the nominal human hearing range:

- **W1, sub-20 kHz:** a perceptually masked, low-energy carrier distributed below 20 kHz. It is designed to be unobtrusive, not guaranteed inaudible.
- **W2, above 24 kHz:** an optional ultrasonic carrier beginning above 24 kHz. It requires a source rate above 48 kHz and is enabled by default only for validated 96 kHz or higher paths.

### Three-layer identity design

| Layer | Location | Purpose | Permanence |
|---|---|---|---|
| W0 signed statement | IP metadata and CODA records | Cryptographic feed UUID, issuer, epoch, sequence, and policy | Permanent when preserved in CODA |
| W1 robust acoustic code | Below 20 kHz | Survive common codec, gain, resampling, noise, and mixing transforms | Measured, never absolute |
| W2 ultrasonic code | Above 24 kHz | Redundant high-band detection in qualified wideband paths | Fragile and optional |

W0 is the authoritative identity statement. W1 and W2 carry a compact rotating code that resolves to W0. This avoids trying to fit a large signature into every audio frame.

### Source-side issuance

The preferred design inserts W0, W1, and W2 at an authorized upstream encoder or trusted gateway before distribution:

    feed UUID + epoch + sequence + policy
      → canonical CBOR payload
      → COSE_Sign1 signature
      → W0 signed statement
      → short rotating acoustic code
      → W1 embedder
      → optional W2 embedder
      → IP encoder and stream

The signed payload uses COSE_Sign1 structures defined by [RFC 9052](https://datatracker.ietf.org/doc/rfc9052/). Private issuer keys remain outside the archive; public verification keys and key identifiers MAY be embedded.

If CODEC inserts a watermark after ingest, it MUST:

1. preserve the original S0 and S1 tracks;
2. create a separate D-class watermarked track;
3. record the issuer, key identifier, model, codebook, gain, frequency bands, and transformation;
4. never label the modified track source_exact or sample_exact relative to the input; and
5. require explicit authorization for issuance.

CODEC cannot retroactively make an unwatermarked upstream source permanently watermarked.

### Signed feed statement

W0 signs the protocol version, issuer and feed UUIDs, stream epoch, sequence, validity interval, rotating short_code, codebook ID, use policy, optional source digest, and public-key identifier. The short code rotates every 5 seconds by default to limit replay and cross-context tracking while the signed mapping retains a stable feed UUID.

### Acoustic carrier rules

| Carrier | Required behavior |
|---|---|
| W1 | Adaptively spread from 300 Hz–19 kHz under a psychoacoustic mask; use gain/peak/band limits, error correction, interleaving, repetition, and keyed codes; reduce or suspend in exposed low-mask intervals; validate every supported codec profile with blinded listening |
| W2 | Use 24–40 kHz at 96 kHz only when below guarded Nyquist and preserved by codec/hardware; disable at ≤48 kHz or on an unqualified path; pass alias/intermodulation tests; never replace W0/W1 or establish identity alone |

### Multi-feed codebook

Simultaneous feeds receive issuer-, revision-, and epoch-scoped low-cross-correlation codes. The issuer prevents active collisions; the detector reports top candidates and margin. Capacity is measured, and insufficient margin produces ambiguous rather than an identity.

### Detection pipeline

~~~mermaid
flowchart TD
    A["Exact IP bytes"] --> B["W0 parser and signature"]
    A --> C["Decode native PCM"]
    C --> D["W1 neural detector"]
    C --> E["W2 detector when valid"]
    B --> F["Evidence fusion"]
    D --> F
    E --> F
    F --> G["Hysteresis and replay checks"]
    G --> H["Live identity event"]
    H --> I["Conditioned separator"]
~~~

The detector MAY combine a neural localized-watermark posterior with a keyed matched-filter or correlation score. Neural audio watermark systems such as [AudioSeal](https://arxiv.org/abs/2401.17264) demonstrate localized, efficient detection, while [WavMark](https://arxiv.org/abs/2308.12770) demonstrates short payload recovery. CODEC treats these as model families to evaluate, not universal guarantees.

### Detection “spikes”

A spike is a detection-statistic peak, not a waveform amplitude spike.

For each hop, the engine records:

- W1 posterior and correlation;
- W2 posterior and correlation when enabled;
- decoded short-code candidates and bit confidence;
- noise-floor and masking context;
- signature state and key identifier;
- replay, collision, and continuity checks;
- top candidate margin;
- model hash and calibration ID.

Default live timing:

| Setting | Default |
|---|---:|
| Analysis window | 1.0 s |
| Hop | 250 ms |
| Start confirmation | 3 qualifying hops |
| End confirmation | 5 failing hops |
| Target verified-event latency | At most 2.0 s |
| Target identity-conditioned stem latency | At most 2.5 s for a supported live bundle |
| Code rotation | 5 s |

Thresholds use hysteresis. A single peak cannot produce verified_feed.

### Fusion states

| State | Requirements | Meaning |
|---|---|---|
| absent | No qualified evidence | No watermark detected |
| candidate | W1 or W2 peak | Acoustic candidate only |
| detected_unverified | Stable code but no valid W0 signature | Do not assign authoritative feed identity |
| verified_feed | Stable code plus valid, current W0 signature | Cryptographically bound feed identity |
| ambiguous | Multiple codes or insufficient margin | No automatic assignment |
| replay_suspected | Valid old/repeated code outside policy | Quarantine identity result |
| signature_invalid | Code resolves to invalid W0 | Security event |
| degraded | Band/path unavailable or detector outside calibration | Report capability loss |

### Live report

Each event contains state, archive time, feed/issuer UUIDs, short code, sequence, confidence, W1/W2 bands and scores, signature/key status, model hash, calibration ID, candidate margin, and report latency. Events are appended to CODA and MAY also use callbacks, JSON Lines, a local socket, or an authenticated service stream.

### Identity-conditioned live separation

When a mixed PCM interval contains one or more qualified watermark codes:

1. The detector produces code-specific conditioning embeddings.
2. The separator estimates a stem for each qualified code plus unknown stems and residual.
3. The tracker aligns stems with code continuity and acoustic embeddings.
4. The engine verifies that removing a claimed stem reduces the corresponding watermark score while retaining mixture consistency.
5. The identity resolver reports verified_feed only when the signed mapping remains valid.
6. Every separated stem remains D class.

Watermarks anchor feed association; they do not make blind separation lossless. The original mixture remains preserved as S0/S1.

### Permanence definition

- **Permanent provenance:** W0 and detection events remain verifiable in the CODA archive as long as the file and cryptographic algorithms remain supported.
- **Permanent acoustic presence in archived source:** if W1/W2 arrived in exact S0/S1 records, those samples remain preserved.
- **Not guaranteed permanent through external transforms:** transcoding, filtering, resynthesis, editing, and analog paths may damage or remove W1/W2.
- **Neural interpretation is never lossless:** the engine archives the input losslessly and stores interpretations as D records.

### Watermark safety and privacy

- Issuance and detection are policy-controlled and audited.
- Issuer private keys are never embedded.
- Public keys may be revoked with signed archive events.
- Rotating codes minimize persistent third-party tracking.
- Detection without an authorized codebook may report watermark_present but not feed identity.
- Audible-band quality and ultrasonic intermodulation must pass separate safety gates.
- The system does not attempt device fingerprinting from an unauthorized ultrasonic beacon.

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
| WATERMARK_ISSUER | Metadata | Issuer UUID, public keys, codebook policy, revocation state |
| WATERMARK_STATEMENT | Metadata/S0 | W0 COSE-signed feed statement and exact carrier metadata |
| WATERMARKED_CHUNK | D | CODEC-generated watermarked derivative; never replaces S1 |
| WATERMARK_OBSERVATION | D | Frame scores, decoded code candidates, bands, detector provenance |
| FEED_IDENTITY_EVENT | D | Fused live state, verified feed UUID, latency, and evidence |
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
- short watermark code and feed UUID to statement and observation intervals;
- issuer key identifier to validity and revocation intervals;
- live feed-identity state transitions;
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
- watermark generators, detectors, codebooks, and psychoacoustic configuration;
- issuer public keys, signed statements, and revocation records;
- label maps and identity profiles needed for named queries;
- model and dataset license notices;
- engine configuration and schema descriptors;
- model hashes.

Compiled GPU caches MAY be embedded, but portable model bytes remain authoritative.

## Neural decomposition

### Distinct tasks

1. **Watermark detection:** localize W1/W2 codes and validate W0 identity statements.
2. **Separation:** estimate waveforms from a mixture, optionally conditioned on watermark codes.
3. **Activity:** estimate when each source is active.
4. **Diarization:** determine which anonymous source is active when.
5. **Tracking:** keep a cluster stable between chunks.
6. **Identification:** compare a track with an enrolled identity or verified feed statement.

No single score substitutes for all six.

~~~mermaid
flowchart TD
    A["Analysis PCM"] --> B["W1/W2 detector"]
    A --> C["Activity and scene analysis"]
    B --> D["Watermark condition"]
    C --> E["K-source separator"]
    D --> E
    E --> F["Residual and consistency"]
    E --> G["Embedding extractors"]
    G --> H["Online clustering"]
    H --> I["Permutation tracker"]
    I --> J["Identity evidence fusion"]
    F --> K["Quality gate"]
    J --> L["Claims and indexes"]
    K --> L
~~~

### ModelBundle contract

| Property | Required value |
|---|---|
| task | watermark_generator, watermark_detector, separation, activity, diarization, embedding, classification, quality |
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
| frequency_contract | Permitted bands, sample-rate minimum, Nyquist guard, filtering assumptions |
| watermark_contract | Payload bits, codebook, error correction, detection state, robustness corpus |
| perceptual_contract | Masking method, gain limits, loudness/peak tolerance, listening-test evidence |
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
- verified or candidate watermark-code continuity;
- source-count transitions;
- feed metadata;
- costed split, merge, birth, and death events.

Every reassignment creates TRACK_EVENT; history is never silently rewritten.

### Live “feel”

The requested “feel for the feed” is a stateful, auditable belief model, not intuition. Live state may update cluster centroids, watermark continuity, replay windows, uncertainty, temporal probabilities, feed noise and codec profiles, and clock alignment. Base neural weights do not mutate during capture. Offline fine-tuning produces a new versioned ModelBundle.

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
- W0 signature validity and statement interval;
- W1/W2 short-code posterior, correlation, continuity, and collision margin;
- watermark replay and revocation state;
- known-feed association;
- content fingerprint;
- spatial/channel continuity;
- codec, device, or path signature;
- time and schedule evidence;
- user-confirmed labels;
- validated wideband evidence.

Each contribution stores its model hash, calibration, score, interval, frequency band, and exclusion reason. A probability is not legal proof.

### Outside-hearing-range evidence

A signal contains nothing at or above Nyquist: 44.1 kHz stops below 22.05 kHz and 48 kHz below 24 kHz. Wideband evidence follows the W2 qualification rules, remains experimental D data, states every band used, never fills missing frequencies, never establishes a name alone, and never emits an active probe.

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

Every claim records identity, anonymous track, intervals, score or probability, threshold, evidence, frequency bands, watermark state, issuer and key identifier, model and source hashes, separation quality, alternatives, enrollment revision, creation time, and revocation/supersession link.

## C++ components

| Component | Responsibility | Execution |
|---|---|---|
| FeedRegistry | Validate FeedSpec | Control thread |
| FeedAdapter | Protocol acquisition | Network pool |
| SourceRecorder | S0 records | Never waits on inference |
| MediaDecoder | FFmpeg decode | Decoder pool |
| Timeline | Clock and gap model | Lock-bounded |
| PcmArchiver | FLAC S1 chunks | Compression pool |
| WatermarkStatementParser | Parse W0 and validate COSE signatures | Network/control pool |
| WatermarkEmbedder | Authorized W1/W2 derived-track issuance | Inference pool |
| WatermarkDetector | Frame-local W1/W2 codes, scores, and bands | Bounded live inference |
| WatermarkFusion | W0/W1/W2 state, replay, collision, hysteresis | Stateful single owner |
| FeedIdentityReporter | Callback, JSON Lines, socket, and archive events | Nonblocking event loop |
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

Use CMakeLists.txt and cmake/ at root; public headers under include/codec; isolated archive, capture, CLI, core, decode, identity, inference, query, timeline, and watermark modules under src; plus models/manifests, schemas, tests/{unit,integration,fuzz,corpus}, benchmarks, and docs.

### Dependencies

| Dependency | Purpose |
|---|---|
| CMake | Build and package |
| FFmpeg libavformat/libavcodec/libavutil | Protocols, demux, decode |
| FLAC | S1 compression |
| ONNX Runtime | Portable inference |
| OpenSSL or audited equivalent | SHA-256 and signatures |
| Canonical CBOR and COSE implementation | W0 signed feed statements |
| fmt-compatible formatting layer | Internal formatting |
| Structured logging sink | Diagnostics |
| GoogleTest or Catch2 | Development tests |

Use FFmpeg public libraries, not parsed CLI subprocess output. See [libavformat](https://ffmpeg.org/doxygen/trunk/group__libavf.html) and [FFmpeg protocols](https://ffmpeg.org/ffmpeg-protocols.html). Pin versions in the build and store them in ARCHIVE_MANIFEST.

## C++ API

The installed v0.1.0 interface is the set of headers under `include/codec`; `examples/capture.cpp` is a compiled minimal example. The richer session, enrollment, query, async-analysis, and derived-stem examples in this section define the target API for the later delivery phases.

### Core types

The codec namespace exposes nanosecond time, strongly typed 128-bit StreamId/TrackId/IdentityId values, FidelityClass, ArchiveMode, InferenceMode, WatermarkState, calibrated Confidence, and an owned C++20-compatible Result<T> value-or-error type.

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

config.watermark.detect = true;
config.watermark.w1_enabled = true;
config.watermark.w1_max_hz = 19000;
config.watermark.w2_policy = codec::W2Policy::qualified_paths_only;
config.watermark.w2_min_hz = 24000;
config.watermark.minimum_w2_sample_rate = 96000;
config.watermark.confirmation_hops = 3;
config.watermark.max_verified_event_latency = std::chrono::seconds{2};
config.watermark.require_valid_signature_for_feed_identity = true;
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

### Receive live feed-identity events

~~~cpp
session->on_feed_identity(
    [](const codec::FeedIdentityEvent& event) {
      if (event.state == codec::WatermarkState::verified_feed) {
        print(event.feed, event.confidence, event.report_latency);
      } else {
        print(event.state, event.top_candidates);
      }
    });
~~~

The callback is nonblocking. Slow consumers receive events through a bounded queue and an explicit dropped-event counter; CODA remains the durable event record.

### Issue an authorized watermarked derivative

~~~cpp
codec::WatermarkIssueRequest request;
request.feed = codec::StreamSelector::label("news");
request.issuer_key = codec::KeyReference{"key://broadcast-issuer-7"};
request.code_rotation = std::chrono::seconds{5};
request.w1 = codec::CarrierPolicy::perceptually_masked;
request.w2 = codec::CarrierPolicy::if_path_qualified;
request.preserve_original = true;

auto watermarked_track = archive->issue_watermark(request);
~~~

The issuer key is resolved through the configured key provider and never written to the archive. This operation creates a D track while preserving original S0/S1.

### Open and verify

Open with codec::Archive::open("session.coda", {.verification = VerificationLevel::full}) and inspect verification_report() before trusting queries.

### Enroll from archive intervals

Pass EnrollmentRequest{label, type, reference intervals, consent reference} to archive.enroll(). Writable enrollment appends IDENTITY_PROFILE plus AUDIT_EVENT.

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

archive.anonymous_tracks() accepts minimum active time and track confidence and returns stable anonymous IDs, intervals, activity, and best authorized candidate.

### Async analysis

Long operations return cancellable tasks and accept a progress callback containing stage, completed units, total units, archive interval, and provider.

### Errors

Errors contain stable code, category, message, retryability, stream/track/record/model/time context, safe underlying code, and redacted diagnostics.

Categories: invalid_argument, unauthorized_source, network, protocol, decode, archive_io, archive_corrupt, model_incompatible, inference, watermark_model_missing, watermark_code_ambiguous, watermark_signature_invalid, watermark_replay_suspected, watermark_path_unqualified, identity_not_enrolled, identity_uncalibrated, cancelled, resource_exhausted, internal.

## C ABI

The v0.1.0 C ABI exposes opaque engine and archive handles plus versioned create, record_file, open, verify, extract_feed, and destroy functions. Every struct carries size and ABI version; every function returns an integer status and optional codec_error_t; exceptions never cross the boundary.

The full target ABI additionally includes opaque session handles, query_identity, query_watermark, feed-identity callbacks, cancellation, and asynchronous extraction.

## CLI usage

The quick-start commands and `codec --help` are implemented in v0.1.0. The broader examples below define the target CLI contract for later delivery phases; commands involving gateway publication, FFmpeg/HLS decoding, S1 FLAC, analysis, ModelBundles, enrollment, neural queries, anonymous tracks, or benchmarks are not reported as runtime capabilities yet.

### Build

~~~bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
~~~

### Record

~~~bash
codec record --archive session.coda --feed news=https://example.net/live/audio --feed radio=https://radio.example.org/stream
~~~

### Issue a signed watermarked derivative

~~~bash
codec watermark issue session.coda --feed news --issuer-key key://broadcast-issuer-7 --code-rotation 5s --w1 perceptually-masked --w2 qualified-paths-only --preserve-original
codec watermark gateway --input https://origin.example.net/news --publish icecast://authorized-publisher/news --feed-uuid 7c2b2f74-7e31-4a1d-b469-d88d63fc8fcb --issuer-key key://broadcast-issuer-7 --w1 perceptually-masked --w2 qualified-paths-only
~~~

The first command appends W0, a D-class watermarked track, issuer public metadata, and an audit event. The gateway form inserts W1/W2 before authorized IP publication while preserving an exact origin capture in its CODA session. Publishing credentials are external secret references.

### Watch live feed identity

~~~bash
codec watermark watch session.coda --states candidate,verified_feed,ambiguous,replay_suspected --format jsonl
codec watermark inspect session.coda --feed news --include-observations --include-signatures
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
codec watermark analyze session.coda --detector-bundle models/watermark-default --rebuild-events
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

[watermark]
detect = true
require_signature_for_feed_identity = true
w1_enabled = true
w1_min_hz = 300
w1_max_hz = 19000
w2_policy = "qualified-paths-only"
w2_min_hz = 24000
w2_max_hz = 40000
w2_minimum_sample_rate = 96000
nyquist_guard_hz = 2000
analysis_window_ms = 1000
hop_ms = 250
start_confirmation_hops = 3
end_confirmation_hops = 5
max_verified_event_latency_ms = 2000
code_rotation_seconds = 5

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
- Stateful trackers and watermark fusion are single-owner.
- Inference has a scheduler per provider and bounded device memory.
- Readers are concurrent and immutable.

| Queue | Drop? | Overload |
|---|:---:|---|
| Accepted bytes → S0 | No | Backpressure if safe; otherwise explicit gap/error |
| PCM → S1 | No in PCM modes | Slow when possible; preserve S0 and report failure |
| PCM → live watermark detector | Yes | Mark interval degraded and reprocess offline |
| PCM → live inference | Yes | Defer to offline |
| Live identity → callback | Yes | Increment dropped-callback count; durable CODA event remains |
| Accepted derived result → writer | No | Bound producer or cancel analysis |
| Debug metrics | Yes | Count and report dropped metrics |

Neural work cannot starve committed source capture.

## GPU evaluation

Version 1 path:

    feed → CPU network buffer → S0
         → CPU decode → native PCM/S1
         → pinned staging or runtime tensor
         → GPU watermark and separation inference
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
| Quality | Watermark BER/FAR/miss rate, SI-SDR improvement, residual, reconstruction, identity |
| Resources | CPU, GPU, VRAM, RAM, host/device bytes |
| Energy | Wall energy per processed audio hour |
| Stability | Resets, retries, skipped or corrupted intervals |

GPU advances only with a measured throughput, CPU, latency, or energy benefit and no integrity or quality violation.

## Observability

Structured events include archive/session/feed/track UUIDs, watermark issuer/code/state, signature status, model hash, frequency bands, correlation/posterior, collision margin, stage, queue, source/archive time, report latency, bytes, samples, gaps, overload, calibration, and redacted error context.

JSON Lines logs are required; OpenTelemetry-compatible metrics are optional. Credentials, keys, reference audio, and sensitive query text are excluded from logs.

## Security, privacy, and responsible use

- Capture and identification require authorization and lawful basis.
- Enrollment requires provenance and policy metadata.
- Identity profiles SHOULD be encrypted.
- Watermark issuer private keys remain in an external key provider.
- Codebooks and public keys are access-controlled and revocable.
- Acoustic codes rotate and are scoped to limit unintended persistent tracking.
- Unverified detections cannot be promoted to authoritative feed identities.
- Issuance requires explicit authorization and an audit event.
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

### Watermark

| Test group | Required measurements |
|---|---|
| Detection | Bit error, ROC, precision/recall, false candidates/hour, misses, verified false attribution, localization, event latency |
| Trust | Signature, expiry, revocation, replay, collision margin, simultaneous-code capacity |
| Quality | Watermark/program ratio by band, loudness, peaks, spectrum, distortion, blinded audibility |
| Separation | Conditioned quality, residual consistency, attribution after stem removal |
| Robustness | Lossless and supported lossy codecs, tandem transcode, resampling, gain/EQ/dynamics/clipping, loss/concealment, noise/reverb/mixing, crop/shift/time-scale; neural resynthesis only when declared |
| W2 qualification | End-to-end rate/passband, alias, intermodulation, audibility, and automatic W1/W0 degradation |

“Permanent” is tested as archive retention and integrity, not as an unbounded promise that an acoustic code survives arbitrary editing.

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

### Watermark MVP

- W0 valid, invalid, expired, revoked, and replayed statements classify deterministically.
- W1 reports verified_feed within 2 seconds under every declared live support condition.
- A supported live separator emits the identity-conditioned stem within 2.5 seconds while preserving the original mixture continuously.
- Validation produces zero verified-feed false attributions; candidate false alarms remain within the published per-hour budget.
- W1 message BER and miss rate stay within the ModelBundle’s declared codec/transformation operating points.
- W2 never enables below 96 kHz or without a qualified end-to-end passband.
- W2 failure degrades to W1/W0 without changing feed identity history silently.
- Blinded testing finds no statistically significant W1 detectability at the preregistered operating point.
- W2 passes alias, intermodulation, and audibility gates on each qualified output path.
- Multi-feed collision tests never promote ambiguous codes to verified_feed.
- Issuance leaves original S0/S1 byte and sample hashes unchanged.
- Watermark-conditioned separation does not violate mixture consistency and reports its delta versus unconditioned separation.

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

## Future extension profiles

The profiles below define architectural extension boundaries. They are **planned/future capabilities, not v0.1.0 runtime claims**, and none changes CODA source-truth semantics.

### Transport and recovery profile

Future CODEC transports MAY represent parity/recovery shards, erasure/FEC coding groups, shard sequence and block identity, reconstruction verification hashes, transport-loss observations, partial-recovery state, and codec enhancement/correction payloads.

The core format MUST NOT require a particular recovery algorithm. PAR2-compatible concepts, Reed-Solomon, fountain/Raptor families, or other validated schemes may be used by a profile without becoming CODA-core dependencies.

Recovery claims MUST distinguish:

1. exact reconstruction of a damaged or missing **encoded bitstream** using sufficient recovery information; and
2. exact reconstruction of a **pre-lossy source**, which requires a complete exactness/correction residual and cannot be implied by parity alone.

### Multiplexed and distributed transport profile

CODA semantics MUST NOT depend on one physical port, connection, frequency slot, or worker per logical feed. One endpoint MAY carry many logical streams, and one logical stream MAY migrate between endpoints or processing workers while retaining its immutable identity and provenance.

Deployments therefore distinguish:

- **physical transport** — file, pipe, shared memory, TCP, QUIC, UDP, provider protocol, radio/acoustic carrier, or another transport;
- **logical stream** — stable CODEC/CODA identity represented inside the protocol and archive; and
- **processing partition** — transient worker/shard ownership used to scale execution.

The same logical record semantics MUST remain valid for one stream, thousands of streams, or tens of millions of registered stream identities. Simultaneous throughput is bounded by bandwidth, compute, memory, storage, and latency budgets rather than port-number or identifier-space limits.

### Distributed/cloud profile

A future distributed deployment MAY use a topology such as:

~~~text
global or regional ingress
  -> stream-ID partitioning
  -> preservation workers
  -> optional recovery workers
  -> optional inference workers
  -> CODA writers/object archive
  -> query/index services
~~~

Core APIs and archive semantics MUST remain vendor-neutral. Kubernetes, AWS, Azure, GCP, specific brokers, and specific object stores are deployment choices, not CODA format requirements.

Distributed implementations SHOULD prefer stable stream-ID partitioning, explicit stream-state ownership, bounded queues, minimal data copies, independently scalable preservation/inference planes, and explicit hot/cold retention profiles.

### Trust and selective-disclosure profile

Future secure deployments MAY add encrypted payloads, envelope or threshold encryption, confidential-compute enclaves, scoped query-capability records, purpose/time/data-class restrictions, immutable disclosure/access logs, and selective exports of authorized records.

CODEC core MUST remain neutral about the legal or organizational authority that grants access. CODEC may record, verify, and enforce declared authorization/policy inputs where a profile implements them; it MUST NOT manufacture legal authority or convert a machine inference into an adjudicative decision.

### Vertical profiles

Justice, public safety, defense, industrial, healthcare, transportation, scientific, enterprise, and other domain-specific schemas SHOULD be implemented above the stable CODA substrate. Vertical requirements MUST NOT silently redefine CODA fidelity, provenance, identity, or compatibility semantics.

## ChatGPT/Codex implementation guidance

This README and approved design documents are normative instructions for future agentic development. ChatGPT, Codex, or another automated implementation agent working on this repository MUST:

1. Inspect current repository state, tests, public interfaces, and implementation-status tables before changing behavior.
2. Never claim an unimplemented capability is complete, simulated output is real inference, or a future profile is a current runtime capability.
3. Keep README status tables, CHANGELOG entries, capability reporting, and code synchronized with implementation reality.
4. Preserve deterministic versus probabilistic capability boundaries and the S0/S1/D truth classes.
5. Prefer narrow, independently testable primitives over speculative end-to-end stubs.
6. Keep CODA core and public abstractions vendor-neutral; cloud, orchestration, transport, codec, and vertical choices belong behind extension interfaces.
7. Implement archive, compatibility, provenance, and transport invariants before hyperscale deployment infrastructure.
8. Add round-trip, compatibility, corruption, truncation, and unknown-record tests for every archive-format change.
9. Ensure inference, watermark analysis, recovery, or downstream AI failure cannot corrupt committed preservation records.
10. Record model and transformation provenance for every D-class artifact.
11. Benchmark throughput, latency, tail latency, memory, CPU/GPU use, storage amplification, recovery overhead, and workload/hardware assumptions before making scale claims.
12. Treat security, privacy, authorization, retention, and auditability as first-class interfaces when implementing profiles that require them.
13. Preserve original source data separately from generated, enhanced, reconstructed, watermarked, translated, or otherwise derived representations.
14. Rebaseline the roadmap when implementation evidence changes rather than forcing code to match obsolete dates or aspirational documentation.

### Generalized delivery sequence

The existing delivery plan and dated 26-week v1.0 schedule remain authoritative for the audio-first product. The following sequence constrains and extends later development without representing current implementation status:

- **Stage A — preserve the v0.1 foundation:** maintain archive, secure capture, signed-watermark, API, CLI, and test guarantees.
- **Stage B — generalized record semantics:** introduce extensible stream/payload typing, generic provenance envelopes, compatibility tests, and unknown-type preservation before public interfaces become difficult to evolve.
- **Stage C — complete the audio 1.0 path:** deliver S1 FLAC/self-contained archival, production media adapters, stateful identity fusion, compatible neural runtime/model bundles, streaming inference, and offline re-inference according to the existing roadmap.
- **Stage D — transport/recovery profile:** add generic recovery-shard semantics, validated FEC implementations, streaming repair, exact verification, and explicit correction-residual semantics where exact pre-lossy reconstruction is supported.
- **Stage E — distributed processing profile:** add protocol multiplexing, stable stream partitioning, distributed workers, object-store backends, query indexes, retention controls, and measured operational benchmarks.
- **Stage F — trust/selective-disclosure profile:** add encryption envelopes, policy/authorization records, scoped access, immutable access logs, selective export, and confidential-compute integration where required.
- **Stage G — vertical profiles:** add domain-specific schemas, model bundles, policy adapters, and integrations without forking CODA truth semantics.

The generalized record model, recovery profile, distributed/cloud profile, trust/selective-disclosure profile, and vertical profiles above are architectural requirements and planned extension points. They are not part of the current v0.1.0 implemented product surface unless and until the status table, tests, and runtime capability reporting are updated together.

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

### Phase 3 — Signed live watermark

- Implement canonical W0 payloads, COSE signing/verification, key rotation, expiry, revocation, and replay checks.
- Implement W1 generator/detector adapters, codebooks, error correction, localized scores, and hysteresis.
- Implement live FEED_IDENTITY_EVENT reporting and archive indexes.
- Add W2 only behind sample-rate, passband, alias, intermodulation, and audibility gates.
- Validate robustness, false alarms, collision limits, latency, and perceptual impact.
- Keep issuer private keys outside CODA and preserve original S0/S1.

### Phase 4 — Separation and tracking

- Add activity, K-source separation, residual, and consistency.
- Add watermark-conditioned target extraction and watermark-removal consistency checks.
- Add embeddings, clustering, permutation tracking, and anonymous queries.
- Validate offline before bounded live mode.

Candidate families include [Conv-TasNet](https://arxiv.org/abs/1809.07454), [SepFormer](https://arxiv.org/abs/2010.13154), [EEND diarization](https://arxiv.org/abs/1909.06247), and [ECAPA-TDNN embeddings](https://www.isca-archive.org/interspeech_2020/desplanques20_interspeech.html). These are references, not fixed endorsements.

### Phase 5 — Identity and extraction

- Add enrollment and revisions.
- Add calibrated fusion and open-set rejection.
- Add claim trace, revocation, queries, extraction, and provenance.
- Keep wideband evidence disabled until independently validated.

### Phase 6 — Live and GPU

- Add bounded live inference and offline catch-up.
- Reuse pinned memory and tensors.
- Benchmark CPU, CUDA, TensorRT, OpenVINO, and MIGraphX where available.
- Degrade inference without weakening preservation.

### Phase 7 — Optional transport

- Evaluate DMA-BUF and zero-copy.
- Evaluate a custom PCIe audio endpoint.
- Advance only if measured value exceeds complexity, portability, synchronization, and power costs.

## 26-week path to v1.0.0

The release program runs from Monday, August 17, 2026 through Friday, February 12, 2027. It publishes one or two evidence-bearing updates per week and promotes a version only when its exit gate passes. Dates are planning targets, not permission to waive archive integrity, fidelity, identity, safety, security, or validation requirements.

| Weeks | Target | Date | Required exit evidence |
|---|---|---:|---|
| 1–2 | v0.1.1 | August 28, 2026 | Close known security and integrity findings; establish parser/archive fuzz infrastructure; freeze compatibility and development-profile migration rules |
| 3–7 | v0.2.0 | October 2, 2026 | Freeze CODA v1; add HLS and Icecast ingestion, private FFmpeg decoding, exact S0 plus sample-exact S1 FLAC, reconnect epochs, gaps, checkpoints, and a recoverable interrupted 24-hour run |
| 8–11 | v0.3.0 | October 30, 2026 | Load versioned ModelBundles; run the ONNX CPU reference path; persist model provenance, tensors, cancellation state, and D records without allowing inference to starve preservation |
| 12–15 | v0.4.0 | November 27, 2026 | Add stateful W0/W1 continuity, key rotation, expiry, revocation, replay windows, ambiguity handling, archive indexes, and qualified live `verified_feed` events; keep W2 behind complete path qualification |
| 16–18 | v0.5.0 | December 18, 2026 | Add offline K-source separation, mandatory residual, mixture-consistency reports, watermark-conditioned extraction, embeddings, anonymous clustering, and permutation tracking |
| 19–21 | Reliability and perceptual soak | January 8, 2027 | Complete extended capture, crash, corruption, codec-transform, collision, false-attribution, audibility, alias, intermodulation, and recovery campaigns; publish measured operating envelopes |
| 22 | v0.6.0 | January 15, 2027 | Add enrollment revisions, calibrated open-set identity fusion, revocation, claim traces, provenance-preserving queries, and identity/time-range extraction |
| 23–24 | v0.7.0 | January 29, 2027 | Add bounded live inference, offline catch-up, CPU/GPU equivalence reports, optional provider selection, tail-latency and energy benchmarks, and overload degradation that never weakens capture |
| 25 | v0.8.0 beta | February 2, 2027 | Freeze public API, C ABI, CODA v1, ModelBundle, CLI, configuration, and error contracts; complete operator, integration, migration, security, and known-limit documentation |
| 25 | v0.9.0 release candidate | February 5, 2027 | Produce installable release artifacts and pass the complete release matrix with no open critical or high-severity security, integrity, fidelity, or false-attribution defect |
| 26 | v1.0.0 | February 12, 2027 | Reproduce the release candidate from a clean environment; pass every applicable acceptance criterion below; publish signed checksums, dependency/model licenses, provenance, release notes, and supported operating envelopes |

Phase 7 transport experiments are not a v1.0.0 release dependency. They advance only after the portable path passes and measurements justify their cost.

### Weekly update contract

- Publish a Friday status update every week, including holiday and soak weeks.
- Publish an additional Tuesday engineering update when a build, benchmark, format decision, model candidate, security result, or milestone candidate lands.
- Identify every update by calendar week, branch, commit SHA, archive/model format revision, and available artifact or benchmark identifiers.
- Report completed work against the prior commitment, exact validation evidence, fidelity and performance movement, open risks or decisions, the next exit gate, and schedule confidence as green, yellow, or red.
- Link failures and negative results. An update is evidence, not a promotional summary.

### Promotion and rebaselining rules

1. A milestone promotes only after its scoped acceptance criteria, supported-platform CI, sanitizer, fuzz/corpus, security, documentation, and recovery gates pass.
2. A failed gate blocks promotion. The next Friday update records the failure, affected dependency chain, corrective owner, new evidence required, and revised target date.
3. A yellow schedule means a gate is threatened but the milestone date is still achievable. Red means the critical path has moved and dependent dates must be rebaselined within two published updates.
4. Beta freezes compatibility. After v0.8.0, only release-blocking correctness, security, documentation, packaging, or portability changes may alter a frozen surface, and each change requires an explicit compatibility note.
5. v1.0.0 requires no open critical or high-severity defect, no unresolved stop/redesign gate, clean installation from published instructions, and an independently recoverable archive produced by the release candidate.

### Definition of v1.0.0 release readiness

- Source-exact S0 and sample-exact S1 preservation, verification, recovery, and extraction meet the archive MVP over the 24-hour interrupted multi-feed corpus.
- Portable ModelBundles run on the CPU reference path; every supported GPU path stays inside declared numeric, quality, latency, memory, and energy tolerances.
- Watermark fusion never promotes ambiguous, replayed, expired, revoked, invalid, or unqualified evidence to an authoritative feed identity.
- Every neural separation contains a residual, mixture-consistency report, uncertainty, model provenance, and an explicit D-class fidelity label.
- Anonymous and enrolled identity queries expose calibrated confidence, alternatives, claim trace, consent/policy provenance, and revocation behavior.
- Capture remains correct under inference overload, cancellation, reconnects, crashes, truncated writes, malformed inputs, and unsupported model/provider combinations.
- The documented C++20 API, versioned C ABI, CLI, configuration, archive schema, ModelBundle schema, and upgrade path match the shipped artifacts.
- GCC, Clang, sanitizers, supported provider tests, integration tests, fuzz/corpus tests, release packaging, license inventory, and clean-environment reproduction all pass for the final commit.

## Stop/redesign gates

- One-file recovery cannot survive torn writes.
- Required queries need an external database.
- Sample-exact output cannot be proven.
- Model bundles are not portable or license-compliant.
- Separation fails consistency thresholds.
- W0 cannot bind a decoded acoustic code to a valid signed feed statement.
- W1 exceeds perceptual, false-alarm, miss, or latency budgets.
- W2 aliases, creates audible intermodulation, or cannot survive the declared path.
- Multi-feed code collisions cause verified false attribution.
- Any component calls an acoustic watermark permanent through arbitrary external transforms.
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
| W0 statement | Canonical CBOR + COSE_Sign1 | Another standardized signed binary envelope |
| W1 method | Neural localized detector plus keyed correlation | Non-neural spread spectrum if it wins the same tests |
| W1 band | Adaptive 300 Hz–19 kHz | Narrower domain-specific mask |
| W2 band | 24–40 kHz at 96 kHz | Qualified band below guarded Nyquist |
| Acoustic code rotation | 5 seconds | Risk- and latency-tested interval |
| Detection confirmation | 3 hops / 2-second budget | Calibrated domain-specific hysteresis |
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
- A named watermark identity requires a valid signed statement.
- A detection spike is statistical, never an audible impulse.
- Sub-20 kHz means perceptually masked, not outside hearing.
- Above-24 kHz is optional and path-qualified.
- Acoustic robustness is measured; CODA provenance is permanent.
- No band is used unless it exists in the capture.
- GPU optimization cannot weaken correctness.
- Repair never mutates the source.
- One file means required models and indexes are embedded.

## References

- [FFmpeg libavformat](https://ffmpeg.org/doxygen/trunk/group__libavf.html)
- [FFmpeg protocols](https://ffmpeg.org/ffmpeg-protocols.html)
- [HTTP Live Streaming, RFC 8216](https://datatracker.ietf.org/doc/html/rfc8216)
- [CBOR Object Signing and Encryption, RFC 9052](https://datatracker.ietf.org/doc/rfc9052/)
- [Xiph FLAC format](https://xiph.org/flac/format.html)
- [ONNX Runtime C++](https://onnxruntime.ai/docs/get-started/with-cpp.html)
- [ONNX Runtime execution providers](https://onnxruntime.ai/docs/execution-providers/)
- [Conv-TasNet](https://arxiv.org/abs/1809.07454)
- [SepFormer](https://arxiv.org/abs/2010.13154)
- [End-to-End Neural Speaker Diarization](https://arxiv.org/abs/1909.06247)
- [ECAPA-TDNN](https://www.isca-archive.org/interspeech_2020/desplanques20_interspeech.html)
- [AudioSeal: localized neural audio watermarking](https://arxiv.org/abs/2401.17264)
- [WavMark: payload-carrying neural audio watermarking](https://arxiv.org/abs/2308.12770)

## License

The software is licensed under the [Apache License 2.0](LICENSE). Model, dataset, codec, and provider licenses remain independently binding and are stored in each ModelBundle.

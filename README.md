# CODEC

## Channel-Oriented Decomposition, Extraction, and Capture

CODEC is a C++20 engine, API, archive format, and staged implementation specification for **authorized heterogeneous temporal streams**.

CODEC core is stream-first. It is designed to:

- ingest and aggregate heterogeneous logical streams without coupling stream identity to ports, files, workers, transports, or physical carrier frequencies;
- preserve accepted source representations as immutable source truth;
- normalize typed stream state exactly when a registered profile can define deterministic canonical state;
- decompose, extract, correlate, identify, and infer over streams through profile-specific processors without rewriting source evidence;
- retain temporal ordering, provenance, integrity, gaps, reconnects, format epochs, and transformation history;
- multiplex many logical streams through shared transports and distributed processing while retaining stable identity;
- support offline re-analysis from the archive as models and processors improve; and
- evaluate throughput, latency, fidelity, recovery, storage, CPU/GPU, energy, and inference-quality trade-offs without converting probabilistic results into deterministic claims.

The authenticated temporal archive is **CODA**, the Channel-Oriented Data Archive, using the **`.coda`** extension. CODA is payload-type agnostic by architectural contract: an archive may preserve a stream even when the current engine has no profile capable of interpreting that stream's payload.

**Audio is the first implemented reference profile, not the definition of CODEC core.** The repository's current v0.1.0 executable functionality is primarily the Audio Stream Profile plus generic source-exact archive primitives. Video, telemetry, sensor, document/event, network/system, recovery, distributed/cloud, and trust/selective-disclosure profiles described below are architectural direction until their implementations and tests exist.

This README is the normative implementation specification. When a generic stream requirement conflicts with a profile-specific requirement, the generic CODEC/CODA invariant governs core design and the profile-specific requirement governs only that profile.

## Status

| Item | Current repository state |
|---|---|
| Specification | Normative stream-first staged product specification |
| Implementation | v0.1.0 executable source-preservation and Audio Stream Profile signed-watermark MVP |
| Language | C++20 |
| Build | CMake 3.20+; GCC and Clang CI |
| Initial platform | Linux, with a portable public API |
| Generic archive | Append-only S0 development profile with integrity evidence, verification, indexing, and exact extraction |
| Generic stream model | Architectural direction; public API remains partly audio/feed-oriented for v0.1 compatibility |
| Audio profile | PCM16 WAV plus W0/W1/W2 reference identity functionality |
| Inference | Backend interface shipped; no neural weights or ONNX runtime bundled yet |
| S1 archive mode | Audio sample-exact FLAC and full self-contained profile planned; generalized state-exact semantics specified below |
| Neural mode | Explicitly unavailable until a compatible ModelBundle is installed |
| Non-audio profiles | Planned; not represented as implemented |

### v0.1.0 implemented product surface

The repository builds a library and `codec` executable. The implemented slice is intentionally honest about what it can prove:

| Surface | v0.1.0 behavior |
|---|---|
| CODA | Append-only development profile with fixed header, ordered records, SHA-256 payload/chain evidence, commit trailers, final index, verification, exact S0 extraction, and non-mutating repair |
| Capture | Bounded file, stdin, HTTP, and HTTPS entity-body capture; sanitized descriptors; local descriptors are secured before archive creation; resolved addresses must be globally routable; implicit proxies and redirects are refused under the default private-network policy |
| Audio Stream Profile | Sample-exact integer PCM16 RIFF/WAVE read and write |
| W0 | Canonical-CBOR COSE_Sign1 statements signed and verified with Ed25519; validity windows and key identifiers enforced |
| W1 | Low-amplitude reference binary-FSK carrier below 20 kHz with CRC, preamble, repeated frames, and Goertzel correlation detection |
| W2 | Reference carrier above 24 kHz; automatically rejected below 96 kHz or without the configured Nyquist guard |
| Identity event | JSON Lines candidates; three matching hops plus a valid W0 produce `signature_bound_candidate`, never authoritative `verified_feed` in the stateless reference detector |
| APIs | C++20 archive/engine/audio/watermark interfaces and a size/version-checked C ABI |
| Neural separation | Stable Audio Profile backend boundary that returns `model_incompatible`; no fabricated stems or identity claims |

The W1 carrier is a measurable Audio Profile reference implementation, not a claim of perceptual transparency. W2 passing the digital sample-rate gate is not hardware path qualification. Replay-safe authoritative `verified_feed` requires the later stateful fusion layer. Moving these capabilities under an Audio Stream Profile does not claim that other profiles are implemented.

### Quick start

Install CMake, a C++20 compiler, OpenSSL 3 development files, and libcurl development files, then build and test:

~~~bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/codec capabilities
~~~

The current CLI retains `--feed` terminology for compatibility with the implemented v0.1 surface. New generic core interfaces SHOULD converge on `Stream*` terminology without silently breaking existing users.

Capture authorized source bytes into one source-exact archive, verify it, and recover them:

~~~bash
./build/codec record --archive session.coda \
  --feed source=https://example.net/authorized-stream \
  --feed backup=/srv/data/backup-stream.bin
./build/codec verify session.coda --level full
./build/codec list feeds session.coda
./build/codec extract session.coda --feed source --fidelity source-exact \
  --output source.bin
~~~

The current watermark commands are Audio Stream Profile operations:

~~~bash
issued_at=$(date +%s)
expires_at=$((issued_at + 3600))
./build/codec watermark keygen --private issuer.key --public issuer.pub
./build/codec watermark issue input.wav --output marked.wav \
  --statement stream.cose --private-key issuer.key \
  --feed-uuid 7c2b2f74-7e31-4a1d-b469-d88d63fc8fcb \
  --code 0x4a31 --issuer station-7 --key-id station-7-2026 \
  --issued-at "$issued_at" --not-before "$issued_at" \
  --expires-at "$expires_at" --w1
./build/codec watermark detect marked.wav --statement stream.cose \
  --public-key issuer.pub --format jsonl
~~~

## Requirements language

**MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY** are normative.

## Core invariants

1. **Stream-first:** CODEC core operates on logical streams, not intrinsically on audio feeds.
2. **Type-agnostic archive:** CODA MUST preserve registered and unknown compatible payload types without requiring every reader to interpret them.
3. **Stable identity:** logical stream identity MUST be independent of transport endpoint, worker, archive segment, connection lifetime, and profile implementation.
4. **Temporal provenance:** source, capture, monotonic, sequence, epoch, and profile-specific clocks MUST remain distinguishable.
5. **Truth separation:** S0, S1, and D MUST never be silently conflated.
6. **Preservation priority:** profile decoding or inference MUST NOT corrupt accepted S0 capture.
7. **Derived traceability:** every D-class result MUST retain provenance to supporting source records/intervals and the processor/model that produced it.
8. **Profile isolation:** audio-, video-, sensor-, telemetry-, document-, network-, recovery-, trust-, or vertical-specific semantics MUST NOT become mandatory fields in generic core records unless they are truly universal.
9. **Scale-independent semantics:** one stream and tens of millions of registered streams use the same logical record semantics; throughput claims remain bounded by measured compute, bandwidth, memory, storage, and latency.
10. **Honest capability boundaries:** unavailable models/profiles return explicit unavailability; CODEC MUST NOT fabricate output or claim reconstruction of absent information.

## Truth and fidelity classes

### S0 — source exact

S0 is the exact accepted source representation: bytes, packets, manifests, headers, event records, sequence information, and relevant transport observations. S0 is preserved without semantic mutation.

### S1 — state exact

S1 is a deterministic normalized representation whose accepted state can be reproduced exactly according to a registered profile's canonicalization rules. **S1 is not intrinsically PCM.**

Examples:

- Audio Stream Profile: canonical integer PCM may be S1 and therefore sample-exact.
- Telemetry Stream Profile: canonical typed measurement/event records may be S1 when their exact representation is rigorously defined.
- A profile that cannot prove deterministic normalized-state semantics MUST NOT label that representation S1.

### D — derived

Inference, resampling, enhancement, summarization, separation, reconstruction, interpolation, denoising, classification, embedding, translation, prediction, concealment, or another transformation is D-class unless a profile proves that it satisfies an S0/S1 definition.

A D-class artifact MUST NOT silently replace S0 or S1.

Calling a CODA archive lossless means the relevant S0 and/or S1 records are preserved exactly under their declared profile semantics. A lossy source does not become equivalent to information that was absent before capture.

## Capability boundary

| Capability | Guarantee | Meaning |
|---|---|---|
| S0 preservation | Deterministic | Accepted source representation hash-identically matches the preserved representation |
| S1 preservation | Deterministic where profile-defined | Canonical normalized state round-trips exactly under registered profile rules |
| Archive integrity | Deterministic | Hashes, chain, checkpoints, and final index verify or locate damage |
| Complete-record extraction | Deterministic | Preserved records can be recovered without inference |
| Profile decomposition/inference | Probabilistic unless independently exact | Derived outputs carry quality/confidence and provenance |
| Identity correlation | Evidence-dependent | Identity strength depends on the registered identity mechanism and evidence |
| Signed identity statement | Cryptographically verifiable where implemented | A valid signature authenticates its statement, not every physical-world interpretation of it |
| Recovery of missing encoded data | Deterministic only with sufficient validated correction data | Recovery MUST be verified against hashes or equivalent exact evidence |
| Recovery of never-captured information | Impossible | Missing source information cannot be recreated exactly without sufficient exact correction/residual data |

## Architectural hierarchy

~~~text
CODEC
  -> generic Stream substrate
       -> capture / ingest
       -> temporal ordering and identity
       -> preservation
       -> provenance
       -> query / extraction / decomposition interfaces
       -> optional inference interfaces
       -> CODA authenticated temporal archive
            -> Audio Stream Profile          [v0.1 reference implementation]
            -> Video Stream Profile          [planned]
            -> Telemetry Stream Profile      [planned]
            -> Sensor Stream Profile         [planned]
            -> Document/Event Stream Profile [planned]
            -> Network/System Stream Profile [planned]
            -> Recovery Profile              [planned]
            -> Trust/Disclosure Profile      [planned]
            -> Distributed/Cloud Profile     [planned]
            -> future profiles
~~~

The generic substrate MUST NOT require an audio decoder, sample rate, channel layout, acoustic carrier, speaker model, image geometry, sensor unit, or another profile-specific concept.

## Core stream vocabulary

New generic core design SHOULD converge on concepts such as:

~~~text
StreamId
StreamType
StreamSpec
StreamDescriptor
StreamClock
StreamEpoch
StreamRecord
StreamProvenance
StreamAdapter
StreamProcessor
StreamInference
StreamExtraction
~~~

Existing `Feed*` and audio-oriented names MAY remain while required for v0.1 API/CLI compatibility. New core work MUST NOT deepen that coupling.

### Stream identity

Every logical stream receives a stable identifier independent of transport ports, process IDs, file names, worker assignments, cloud regions, physical carrier frequencies, archive segments, connection lifetimes, and decoder/profile implementations.

Identity MUST survive reconnects, worker migration, transport changes, archive segmentation, and offline re-analysis.

### Stream types

CODA's model MUST be extensible to at least audio, video, image/frame sequences, telemetry, generic sensor observations, navigation/position data, radar/lidar/sonar payloads, documents/text, transcript segments, network/system events, model inputs/outputs, embeddings/features, identity evidence, recovery/FEC shards, audit/access events, authorization/policy records, and opaque custom payloads.

Unknown compatible stream/payload types MUST remain preservable and extractable even when the current engine cannot interpret them.

## Generalized CODA record model

The conceptual record envelope is payload-type independent:

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
  truth_class
  payload_type
  payload
  payload_hash
  previous_record_hash
  provenance
  policy_tags
}
~~~

This is a semantic model, not a frozen v0.1 binary layout. Future archive-format implementation MUST follow CODA versioning and compatibility rules and preserve unknown compatible record types.

## Generic processing pipeline

~~~text
authorized heterogeneous source
  -> StreamAdapter
  -> S0 capture
  -> typed StreamRecord / CODA append
       -> optional registered exact normalization -> S1
       -> optional profile decomposition/extraction
       -> optional profile inference -> D
       -> optional identity evidence
       -> optional recovery/FEC records
~~~

Preservation has priority. Profile decoding, processing, or inference failure MUST NOT corrupt accepted S0 capture. Under bounded-resource exhaustion, the engine MAY reject new input explicitly rather than silently corrupting already accepted evidence.

The engine therefore has independent conceptual planes:

- **Preservation:** ingest, source truth, generic clocks/epochs, append writes, integrity, checkpoints, indexes, verification, and exact extraction.
- **Profile processing:** registered exact normalization, profile-specific decomposition/extraction, identity evidence, and other deterministic processors.
- **Inference:** optional probabilistic/learned analysis producing provenance-bearing D-class artifacts.

## Generic end-to-end flow

1. The caller supplies authorized stream specifications.
2. A `StreamAdapter` accepts source representations and records descriptors, transport observations, epochs, sequence, and timing evidence.
3. Each logical stream receives a stable stream identity.
4. Accepted source representation is appended as S0 before optional interpretation.
5. A registered profile MAY produce a deterministic canonical S1 state and MUST prove exact round-trip semantics before using the S1 label.
6. Bounded processing queues feed optional profile processors and inference without making preservation depend on them.
7. Derived processors/models emit D-class artifacts with source references, processor/model identity, configuration, quality/confidence, and transformation provenance.
8. Identity mechanisms MAY append evidence without converting probabilistic correlations into deterministic source truth.
9. Checkpoints/indexes keep a growing archive recoverable and queryable.
10. Clean close appends the final integrity/index structures required by the active CODA version/profile.
11. A user with only the archive and required profile implementations can verify, query, extract, and re-analyze preserved evidence offline.

## Generic ingest and transport

Physical transport, logical stream identity, processing partition, and archive placement are separate concepts.

Generic adapters MAY include files, pipes, HTTP(S), QUIC, TCP, UDP, shared memory, message/event buses, provider protocols, or profile-specific media/sensor transports. The core adapter contract preserves source representation and transport provenance without assuming media semantics.

Network behavior MUST use bounded reconnect/backoff policies, explicit connection epochs, gap/loss records where observable, policy-limited redirects/targets/timeouts, and MUST NOT bypass DRM, encryption, access controls, paywalls, or provider restrictions.

One endpoint MAY carry many logical streams. One logical stream MAY migrate between endpoints while retaining identity and history.

## Generic timing model

Every observation SHOULD retain the applicable combination of:

- archive monotonic time;
- capture-host monotonic time;
- observed UTC plus uncertainty;
- original source timestamp/time base where supplied;
- source sequence/counter where supplied;
- connection epoch;
- format/schema epoch; and
- profile-specific clock mapping.

Profile clocks map into this generic temporal model. Audio sample positions, video PTS/DTS, sensor clocks, event timestamps, and sequence counters do not redefine core time semantics.

## Provenance model

Every D-class result MUST be traceable to the exact source records or intervals supporting it and SHOULD record, where applicable:

- source stream IDs and record/interval references;
- processor/model/runtime identity and hash/version;
- configuration and calibration;
- inference/transformation timestamp;
- confidence/quality values;
- transformation chain; and
- residual/uncertainty information.

Later processors/models MAY append new interpretations without rewriting source history.

## Query and extraction model

Generic query/extraction SHOULD support combinations of stream/source identity, time/sequence range, truth class, stream/payload type, event identity, processor/model version, confidence threshold, provenance relationship, and authorization/policy scope.

Query results MUST retain links to supporting source records. Profile-specific exporters MAY then produce WAV/FLAC, video, telemetry, documents, forensic packages, or other representations.

## Multiplexing and scale

CODA semantics MUST NOT imply one port, connection, frequency, file, or worker per stream.

The architecture distinguishes:

1. physical transport;
2. logical stream identity;
3. processing partition/shard; and
4. archive placement.

The same logical semantics MUST work for one stream, thousands of streams, or tens of millions of registered logical streams. Simultaneous throughput is bounded by actual bandwidth, compute, memory, storage, and latency budgets—not identifier or port count.

## Stream profiles

### Audio Stream Profile — first reference implementation

The current implementation is primarily audio-oriented. The following concepts are **Audio Stream Profile semantics**, not CODEC/CODA core requirements:

- WAV/PCM and media decoding;
- sample rate and channel layout;
- S1 sample-exact integer PCM and future FLAC storage;
- W0/W1/W2 acoustic identity;
- diarization and speaker embeddings;
- neural source separation and stems;
- speech/music/ambience-specific evaluation;
- HLS/Icecast/audio rendition behavior;
- audio fidelity and watermark benchmarks.

#### Audio profile ingest scope

| Input | Delivery phase | Audio-profile preservation |
|---|---:|---|
| File or pipe | Current/Phase 1 | Exact S0 bytes where ownership and stream semantics permit |
| HTTP/HTTPS media | Current/Phase 1 | Body plus sanitized response metadata |
| HLS | Phase 1 | Playlist snapshots, chosen renditions, sequences, discontinuities, and segments |
| Icecast/Shoutcast | Phase 1 | Audio bytes and metadata intervals |
| MPEG-DASH | Phase 2 | MPD snapshots, adaptation sets, initialization data, and segments |
| RTSP/RTP | Phase 2 | Packets, timestamps, sequence, and loss observations |
| Custom provider | Extension point | Registered adapter implementation |

#### Audio profile specification

An audio stream profile MAY describe expected content, rendition policy, channel/sample layout, preserve-PCM policy, watermark policy, trusted W0 issuers, and audio-specific decoder/model configuration. These fields belong in profile metadata rather than the generic `StreamSpec` unless they become universally applicable.

#### Audio exactness

For the Audio Stream Profile, S1/sample-exact means canonical integer PCM reproduces the accepted decoded samples exactly. Resampling, denoising, enhancement, source separation, neural stems, embeddings, inferred labels, and concealment are D-class.

The Audio Profile MUST retain source bytes separately from CODEC-generated watermark derivatives. A watermark applied by CODEC MUST NOT mutate archived S0/S1 truth.

#### W0/W1/W2 identity

W0 is signed identity metadata; W1 is the reference low-amplitude sub-20 kHz acoustic carrier; W2 is the optional above-24 kHz reference carrier gated by sample rate and Nyquist guard. These are Audio Profile identity mechanisms, not universal stream identity semantics.

A valid W0 signature authenticates the signed statement. Acoustic survival and correlation remain evidence-dependent. W1/W2 detection MUST NOT convert a neural stem into lossless source truth.

#### Audio inference

Audio separation, diarization, speaker embeddings, anonymous continuity, named identity, residual estimation, and stem extraction are D-class unless a separate exactness proof applies. Preservation MUST continue if the neural backend is unavailable or overloaded.

The v0.1 backend intentionally returns `model_incompatible` when no compatible model bundle is installed.

### Additional stream profiles

Video, telemetry, sensor, document/event, network/system, and future profiles SHOULD register their own payload schemas, canonical S1 rules where valid, processors, inference capabilities, and extraction formats without modifying generic CODA semantics.

These profiles are **planned architectural extension points**, not v0.1 implementation claims.

### Recovery profile

Recovery is a profile boundary, not a mandatory CODA-core algorithm. It MAY represent parity/recovery shards, erasure/FEC groups, shard/block identity, loss observations, reconstruction hashes, partial-recovery state, and correction/enhancement payloads.

PAR2-compatible concepts, Reed-Solomon, fountain codes, or other validated schemes MAY be implementations without becoming core requirements.

Recovering a damaged/missing encoded bitstream exactly and reconstructing a pre-lossy source exactly are separate claims. The latter requires sufficient exact correction/residual information and MUST NOT be implied by parity alone.

### Distributed/cloud profile

Future deployments MAY use regional ingress, stream-ID partitioning, capture/recovery/inference workers, CODA writers, object storage, query/index services, hot/cold retention, and independently scalable preservation/inference planes. Kubernetes, AWS, Azure, GCP, and other platforms remain deployment choices rather than core semantics.

### Trust/selective-disclosure profile

Secure deployments MAY add encryption envelopes, threshold/envelope encryption, confidential computation, scoped capability tokens, purpose/time/data-class restrictions, immutable access records, and selective export. CODEC records/enforces configured authority inputs but does not manufacture legal authority.

## Generic acceptance criteria

A release claiming generalized CODEC/CODA core support MUST demonstrate, as applicable:

- byte-for-byte S0 round trips for generic/opaque payloads;
- exact S1 round trips for every profile claiming S1;
- unknown compatible type preservation and extraction;
- archive-version compatibility;
- sequence/gap/reconnect/epoch semantics;
- generic clock/timestamp behavior;
- provenance-link integrity;
- profile failure and inference-overload isolation;
- deterministic generic query/extraction behavior; and
- measured scale claims tied to actual hardware/workloads.

Audio Profile releases additionally retain audio regression, PCM exactness, watermark, separation/inference, and media-adapter tests appropriate to their implemented surface.

## Security, privacy, and authorization

- Capture MUST be authorized and policy-limited.
- Secrets SHOULD be external references rather than archived plaintext credentials.
- Descriptors and provenance MUST redact configured secrets.
- Output paths MUST use safe creation rules appropriate to the platform.
- Remote targets, redirects, response sizes, and timeouts MUST be bounded by policy.
- Archive verification MUST treat corruption and unsupported record/profile types explicitly.
- Trust/selective-disclosure implementations MUST enforce declared scope and audit access without treating CODEC itself as the source of legal authority.

## ChatGPT/Codex normative implementation rules

Future automated development MUST follow these rules:

1. Treat CODEC as stream-first and CODA as payload-type agnostic.
2. Treat audio as the first implemented reference profile, not as CODEC core.
3. When generic and audio-specific requirements conflict, preserve the generic core invariant and scope the audio requirement to Audio Stream Profile.
4. Inspect repository state and tests before changing behavior.
5. Never claim an unimplemented capability is complete.
6. Keep implementation-status tables accurate in every release.
7. Preserve S0/S1/D and deterministic/probabilistic boundaries.
8. Prefer narrow, testable generic primitives over speculative end-to-end stubs.
9. Do not add audio-only fields to generic records/APIs when they belong in profile metadata.
10. Implement archive, identity, time, provenance, compatibility, and transport invariants before hyperscale deployment infrastructure.
11. Add corruption/backward-compatibility tests for archive-format changes.
12. Ensure inference/profile failure cannot corrupt preservation.
13. Record model/processor/transformation provenance for every D-class artifact.
14. Benchmark throughput, latency, memory, CPU/GPU, storage amplification, and recovery overhead before claiming scale.
15. Keep core abstractions vendor-neutral.
16. Update documentation when implementation reality changes rather than forcing code to match obsolete milestones.

## Delivery sequence toward 1.0

### Stage A — preserve and classify current v0.1

Keep existing behavior working and explicitly classify current audio functionality as Audio Stream Profile implementation.

### Stage B — stabilize stream-first core semantics

Introduce/standardize generic stream IDs/types/specs, generic clocks/epochs, generalized S0/S1/D semantics, payload typing, provenance envelopes, unknown-type preservation, and compatibility tests.

### Stage C — generic adapter, processor, query, and extraction boundaries

Ensure non-audio profiles can be added without redesigning CODA or core APIs. Migrate audio-specific assumptions behind the Audio Profile while retaining compatibility.

### Stage D — Audio Stream Profile 1.0

Complete the audio specialization: self-contained S1 storage/FLAC where appropriate, production media adapters, stateful identity fusion, compatible neural runtime/model bundles, streaming inference, offline re-inference, and audio-specific validation.

Stage D MUST NOT block independent development of other stream profiles once Stage C is stable.

### Stage E — transport/recovery profile

Add generic multiplexing/recovery semantics, validated FEC implementations, streaming repair, and exact verification.

### Stage F — distributed processing profile

Add partitioning, distributed workers, object-store backends, indexes, operational benchmarks, and deployment integrations without changing stream/CODA truth semantics.

### Stage G — trust/selective-disclosure profile

Add encryption envelopes, scoped access, immutable access logs, policy records, and confidential-compute integration where required.

### Stage H — additional stream/vertical profiles

Build video, telemetry, sensor, document/event, network/system, and domain-specific schemas/model bundles/integrations on the stable substrate rather than forking core semantics.

## 1.0 success criteria

CODEC 1.0 direction is successful when:

1. A new reader cannot reasonably conclude that CODEC core means audio processing.
2. CODA can describe and preserve unknown non-audio streams without archive redesign.
3. Generic public concepts do not require PCM, sample rate, channel layout, speakers, acoustic watermarks, or neural stems.
4. Existing audio behavior remains available and honestly documented under Audio Stream Profile.
5. S1 means deterministic state-exactness generically; sample-exact PCM is the Audio Profile specialization.
6. New profiles can register typed semantics, exact-state rules, processors, inference, and extraction without changing core record semantics.
7. Every D-class result remains traceable to immutable source evidence.
8. The same stream semantics work on-device, offline, on-premises, and at distributed cloud scale.
9. Preservation remains operational when optional inference/profile processors are unavailable.
10. Future ChatGPT/Codex work builds generalized invariants before profile-specific convenience.

## Current implementation boundary

This README intentionally specifies more than v0.1.0 implements. **Today, the executable remains primarily an Audio Stream Profile and generic S0 archive MVP.** The generalized `Stream*` API vocabulary, generalized S1 canonical-state support, non-audio profiles, generic multiplexed transport, recovery/FEC, distributed/cloud operation, and trust/selective-disclosure profile are staged requirements until code and tests prove them.

Documentation MUST be updated whenever implementation reality changes. No future capability becomes a release claim merely because it appears in this specification.

## Design documents

- `docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md` — approved stream-first architecture.
- `docs/superpowers/plans/2026-08-18-stream-first-readme.md` — documentation correction implementation plan.

## License and contribution posture

Contributions SHOULD preserve the core invariants above, keep profile-specific semantics isolated, add tests for any new exactness or archive claim, and avoid weakening the distinction between preserved evidence and derived inference.

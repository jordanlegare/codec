# Stream-First CODEC / Generalized CODA Direction Design

Date: 2026-08-18
Repository: `jordanlegare/codec`
Status: Approved corrected architectural direction; README implementation pending

## Purpose

Define CODEC as a general stream-aggregation, preservation, decomposition, extraction, identity, provenance, and inference substrate. CODA is the type-agnostic authenticated temporal archive produced and consumed by that substrate.

Audio is the first implemented reference profile. Audio capabilities are important and remain supported, but audio does not define CODEC core, CODA core, the generic public model, or the long-term scaling architecture.

The governing principle is:

> CODEC operates on heterogeneous logical streams. CODA preserves authenticated temporal records. Stream profiles specialize those generic semantics. Derived intelligence never silently replaces immutable source truth.

When a generic core requirement and a profile-specific requirement conflict, the generic stream invariant governs CODEC/CODA core and the profile-specific requirement governs only that profile.

## Design goals

1. Preserve truthful v0.1.0 implementation claims while changing the architectural center of gravity from audio to heterogeneous streams.
2. Make stream identity, temporal ordering, source preservation, provenance, extraction, multiplexing, and extensibility core concepts.
3. Make CODA payload-type agnostic and capable of preserving unknown stream/payload types without interpretation.
4. Define fidelity/truth classes that work for audio, video, telemetry, sensor, document/event, network, model, and future stream profiles.
5. Keep preservation independent of inference and profile-specific decoding.
6. Permit one logical stream or hyperscale multiplexed deployments without changing record semantics.
7. Keep transport, recovery/FEC, cloud/distributed processing, inference, trust/selective disclosure, and vertical-domain behavior as profiles/extensions rather than hard dependencies.
8. Keep CODEC portable across embedded, edge, on-premises, offline, and cloud environments.
9. Direct ChatGPT/Codex to implement generic invariants before optimizing any one profile.

## Non-goals

- Do not make audio, PCM, FLAC, speech, music, W0/W1/W2, or neural source separation requirements of CODEC/CODA core.
- Do not remove or weaken currently implemented audio functionality; classify it under the Audio Stream Profile.
- Do not make Kubernetes, AWS, Azure, GCP, PAR2, Reed-Solomon, OFDM, QAM, AC-3, or another deployment technology a core dependency.
- Do not redefine probabilistic inference as deterministic evidence.
- Do not promise reconstruction of information absent from preserved evidence or sufficient correction data.
- Do not make CODEC a policy, adjudication, surveillance, or autonomous-enforcement authority.

## Architectural hierarchy

```text
CODEC
  -> generic Stream substrate
       -> capture / ingest
       -> temporal ordering and identity
       -> preservation
       -> provenance
       -> extraction / decomposition interfaces
       -> optional inference interfaces
       -> CODA authenticated temporal archive
            -> Audio Stream Profile
            -> Video Stream Profile
            -> Telemetry Stream Profile
            -> Sensor Stream Profile
            -> Document/Event Stream Profile
            -> Network/System Stream Profile
            -> Recovery Profile
            -> Trust/Selective-Disclosure Profile
            -> Distributed/Cloud Profile
            -> future profiles
```

The generic substrate must not require an audio decoder, sample rate, channel layout, acoustic carrier, speaker model, or other media-specific concept.

## Core stream abstraction

The public conceptual vocabulary should converge on generic names such as:

```text
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
```

Existing audio-oriented API names may remain temporarily for compatibility, but new core interfaces should not deepen audio coupling. Migration should be explicit and versioned rather than silently breaking existing users.

### Stream identity

Every logical stream has a stable identifier independent of:

- TCP/UDP port;
- file name;
- process or worker identity;
- cloud region;
- physical carrier frequency;
- archive segment;
- connection lifetime;
- decoder/profile implementation.

A 128-bit UUID or comparably large identifier is suitable for the public model.

Identity must survive reconnects, worker migration, transport changes, archive segmentation, and offline re-analysis.

### Stream types

The model must be extensible to at least:

- audio;
- video;
- image/frame sequences;
- telemetry and measurements;
- generic sensor observations;
- navigation/position data;
- radar/lidar/sonar payloads;
- documents and text;
- transcript segments;
- network/system events;
- model inputs/outputs;
- embeddings/features;
- identity evidence;
- recovery/FEC shards;
- audit/access events;
- authorization/policy records;
- opaque/custom provider payloads.

Unknown types must remain preservable and extractable even when the current engine has no profile capable of interpreting them.

## Generalized CODA record model

CODA evolves toward a stable record envelope independent of payload type:

```text
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
```

The exact binary representation is an implementation decision governed by archive versioning and compatibility rules.

Time is generic. Audio sample positions, video PTS/DTS, sensor clocks, event timestamps, sequence counters, and other profile clocks map into the stream temporal model without redefining core semantics.

## Truth and fidelity classes

### S0 — source exact

The exact accepted source representation: bytes, packets, manifests, headers, event records, sequence information, and relevant transport observations. S0 is preserved without semantic mutation.

### S1 — state exact

A deterministic normalized representation whose accepted state can be reproduced exactly according to a registered profile's canonicalization rules.

S1 is not intrinsically PCM.

Examples:

- Audio Profile: losslessly represented canonical integer PCM may be S1/sample-exact.
- Telemetry Profile: canonical typed measurement/event records may be S1.
- Other profiles may define S1 only when deterministic exact-state semantics are rigorously specified.

If exact normalized-state semantics cannot be established, the representation must not be labeled S1.

### D — derived

Inference, resampling, enhancement, summarization, separation, reconstruction, interpolation, denoising, classification, embedding, translation, prediction, or another transformation is D-class unless a profile proves it satisfies an S0/S1 definition.

D-class output must never silently replace S0 or S1.

## Provenance

Every D-class result must be traceable to the exact source records or intervals supporting it and, where applicable, record:

- source stream IDs;
- source record/interval references;
- model/runtime identity and hash/version;
- configuration;
- calibration;
- inference/transformation timestamp;
- confidence/quality values;
- transformation chain;
- residual/uncertainty information.

Later models may append new interpretations without rewriting source history.

## Generic processing pipeline

```text
authorized heterogeneous source
  -> StreamAdapter
  -> S0 capture
  -> typed StreamRecord / CODA append
       -> optional registered exact normalization -> S1
       -> optional profile decomposition/extraction
       -> optional inference -> D
       -> optional identity evidence
       -> optional recovery/FEC records
```

Preservation has priority. Profile decoding or inference failure must not corrupt or block S0 capture except where explicit bounded-resource policy requires rejecting new input.

## Profiles

### Audio Stream Profile — first reference implementation

All existing audio-specific functionality belongs here, including as applicable:

- WAV/PCM and media decoding;
- sample rate/channel layout semantics;
- S1 sample-exact PCM and future FLAC storage;
- W0/W1/W2 acoustic identity;
- diarization/speaker embeddings;
- neural source separation/stems;
- speech/music-specific evaluation;
- HLS/Icecast/audio transport adapters;
- audio fidelity and watermark benchmarks.

Current v0.1.0 implementation status remains truthful: moving these concepts under a profile does not claim that non-audio profiles are implemented.

### Other stream profiles

Video, telemetry, sensor, document/event, network/system, and future profiles register their own payload schemas, canonical S1 rules where valid, processors, inference capabilities, and extraction formats without modifying generic CODA semantics.

### Recovery profile

Represent parity/recovery shards, erasure/FEC groups, shard/block identity, loss observations, reconstruction hashes, partial-recovery state, and correction/enhancement payloads generically. PAR2-compatible concepts, Reed-Solomon, fountain codes, or other validated algorithms may be implementations, not core requirements.

Exact encoded-bitstream recovery and reconstruction of a pre-lossy source are separate claims and must remain separately proven.

### Identity profile

Identity evidence may bind stream ID, source/device identity, issuer, epoch, sequence, trust policy, signatures, or physical-carrier observations. W0/W1/W2 are Audio Profile specializations, not the universal identity mechanism.

### Inference profile

Inference is optional and subordinate to preservation. It supports stateful streaming and offline re-inference where compatible models exist. No compatible model means explicit unavailability. Every result is provenance-bearing D-class output unless independently proven otherwise.

### Distributed/cloud profile

Physical transport, logical streams, and processing partitions are distinct concepts. One endpoint may carry many logical streams; a stream may migrate while retaining identity.

Future implementations may use regional ingress, stream-ID partitioning, capture/recovery/inference workers, CODA writers, object storage, and query/index services. Kubernetes and cloud vendors remain deployment choices rather than core semantics.

### Trust/selective-disclosure profile

Secure deployments may add encryption envelopes, confidential computation, capability tokens, policy/time/data-class restrictions, immutable access records, and selective export. CODEC records/enforces configured authority inputs but does not manufacture legal authority.

## Query and extraction model

Generic query/extraction should eventually support combinations of:

- stream/source identity;
- time/sequence range;
- truth class;
- stream/payload type;
- event identity;
- model/version;
- confidence threshold;
- provenance relationship;
- authorization/policy scope.

Results retain links to supporting source records. Profile-specific exporters may then produce FLAC/WAV, video, telemetry, documents, forensic packages, or other representations.

## Multiplexing and scale

CODA semantics must never imply one port, connection, frequency, file, or worker per stream.

The architecture distinguishes:

1. physical transport;
2. logical stream identity;
3. processing partition/shard;
4. archive placement.

The same logical semantics must work for one stream, thousands of streams, or tens of millions of registered streams. Simultaneous throughput is bounded by actual bandwidth, compute, memory, storage, and latency budgets—not identifier or port count.

## ChatGPT/Codex normative implementation rules

1. Treat CODEC as stream-first and CODA as payload-type agnostic.
2. Treat audio as the first implemented reference profile, not as CODEC core.
3. When generic and audio-specific requirements conflict, preserve the generic core invariant and scope the audio requirement to Audio Stream Profile.
4. Inspect repository state/tests before modifying behavior.
5. Never claim an unimplemented capability is complete.
6. Keep implementation-status tables accurate.
7. Preserve S0/S1/D boundaries and deterministic/probabilistic distinctions.
8. Prefer narrow, testable generic primitives over speculative end-to-end stubs.
9. Do not add audio-only fields to generic records/APIs when they belong in profile metadata.
10. Implement archive, identity, time, provenance, compatibility, and transport invariants before hyperscale infrastructure.
11. Add corruption/backward-compatibility tests for archive changes.
12. Ensure inference/profile failure cannot corrupt preservation.
13. Record provenance for every D-class artifact.
14. Benchmark throughput, latency, memory, CPU/GPU, storage amplification, and recovery overhead before claiming scale.
15. Keep core abstractions vendor-neutral.
16. Update documentation when implementation reality changes.

## Delivery sequence

### Stage A — preserve and classify current v0.1

Keep existing behavior working and explicitly classify current audio functionality as Audio Stream Profile implementation.

### Stage B — stabilize stream-first core semantics

Introduce/standardize generic stream IDs/types/specs, generic clocks/epochs, generalized S0/S1/D semantics, payload typing, provenance envelopes, unknown-type preservation, and compatibility tests.

### Stage C — generic adapter, processor, query, and extraction boundaries

Ensure non-audio stream profiles can be added without redesigning CODA or core APIs. Migrate audio-specific assumptions behind the Audio Profile while retaining compatibility.

### Stage D — Audio Stream Profile 1.0

Complete the audio specialization: self-contained S1 storage/FLAC where appropriate, production media adapters, identity fusion, compatible neural runtime/model bundles, streaming inference, offline re-inference, and audio-specific validation.

This stage must not block independent development of other stream profiles once Stage C is stable.

### Stage E — transport/recovery profile

Add generic multiplexing/recovery semantics, validated FEC implementations, streaming repair, and exact verification.

### Stage F — distributed processing profile

Add partitioning, distributed workers, object-store backends, indexes, operational benchmarks, and deployment integrations without changing stream/CODA truth semantics.

### Stage G — trust/selective-disclosure profile

Add encryption envelopes, scoped access, immutable access logs, policy records, and confidential-compute integration where required.

### Stage H — additional stream/vertical profiles

Build video, telemetry, sensor, document/event, network/system, and domain-specific schemas/model bundles/integrations on the stable substrate rather than forking core semantics.

## Testing requirements

At minimum:

- byte-for-byte S0 round trips for generic/opaque payloads;
- exact S1 round trips for every profile claiming S1;
- unknown-type preservation and extraction;
- archive-version compatibility;
- sequence/gap/reconnect/epoch semantics;
- generic clock/timestamp tests;
- provenance-link integrity;
- profile failure/inference overload isolation;
- deterministic generic query/extraction behavior;
- audio regression tests proving the profile migration does not weaken current guarantees;
- corruption detection and bounded repair where recovery is implemented;
- authorization-scope tests for secure profiles;
- scale benchmarks tied to actual hardware/workloads.

## Success criteria

The correction is successful when:

1. A new reader cannot reasonably conclude that CODEC core means audio processing.
2. CODA can describe and preserve an unknown non-audio stream without an archive redesign.
3. Generic public concepts do not require PCM, sample rate, channel layout, speakers, acoustic watermarks, or neural stems.
4. Existing audio behavior remains available and honestly documented under Audio Stream Profile.
5. S1 means deterministic state-exactness generically; sample-exact PCM is the Audio Profile specialization.
6. New profiles can register typed semantics, exact-state rules, processors, inference, and extraction without changing core record semantics.
7. Every D-class result remains traceable to immutable source evidence.
8. The same stream semantics work on-device, offline, on-premises, and at distributed cloud scale.
9. Future ChatGPT/Codex work builds generalized invariants before profile-specific convenience.

## Immediate README implementation scope

After this corrected spec is reviewed and approved, update `README.md` substantially rather than adding another isolated future-direction section:

- rewrite the opening product definition around heterogeneous streams;
- rewrite top-level goals/invariants around generic stream semantics;
- make the generic processing architecture primary;
- redefine S1 as state-exact and state the audio sample-exact specialization separately;
- replace core `Feed*` conceptual vocabulary with `Stream*` direction while documenting compatibility where existing APIs remain audio-oriented;
- move audio decoding, PCM, W0/W1/W2, diarization, separation, stems, FLAC, and audio benchmarks conceptually under Audio Stream Profile;
- generalize ingest, timing, identity, inference, query, extraction, testing, acceptance, and roadmap language;
- preserve an explicit current-implementation section identifying which capabilities remain audio-only today;
- ensure planned non-audio profiles are never represented as implemented.

No source-code/API/archive-format behavior change is part of this immediate documentation correction.
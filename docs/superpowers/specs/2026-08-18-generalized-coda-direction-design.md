# Generalized CODA Platform Direction Design

Date: 2026-08-18
Repository: `jordanlegare/codec`
Status: Approved architectural direction; implementation pending

## Purpose

Evolve CODEC from an audio-first preservation and inference engine into a general authenticated temporal-stream substrate while preserving the current v0.1.0 guarantees and avoiding premature coupling to any specific cloud, transport, justice, surveillance, defense, or commercial deployment.

The core principle is:

> Audio remains the first specialization, while CODA becomes a general authenticated temporal container for heterogeneous machine-observed streams and all downstream derived intelligence remains distinguishable from immutable source truth.

## Design goals

1. Preserve all current v0.1.0 claims and implementation boundaries exactly.
2. Generalize the CODA record model early, before v1.0 freezes public interfaces.
3. Keep source-preserved data, sample-exact representations, and derived inference strictly separated.
4. Permit logical multiplexing from one stream to hyperscale deployments without changing record semantics.
5. Introduce future recovery/FEC, distributed transport, cloud storage, confidential-compute, and selective-disclosure capabilities as profiles/extensions rather than mandatory core dependencies.
6. Make future ChatGPT/Codex implementation work staged, testable, evidence-driven, and prohibited from fabricating unimplemented capability.
7. Keep the core engine portable and usable on-premises, embedded, edge, cloud, or offline.

## Non-goals

- Do not make Kubernetes, AWS, Azure, GCP, PAR2, Reed-Solomon, OFDM, QAM, AC-3, justice, surveillance, military, or any other deployment-specific technology a hard dependency of CODA core.
- Do not redefine probabilistic inference as deterministic evidence.
- Do not promise reconstruction of information absent from captured source material.
- Do not weaken the current source-exact and sample-exact fidelity definitions.
- Do not make CODEC itself a policy, adjudication, or autonomous enforcement authority.

## Core abstraction

CODA should evolve toward a general record envelope with stable semantics independent of payload type.

Conceptually:

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
  fidelity_class
  payload_type
  payload
  payload_hash
  previous_record_hash
  provenance
  policy_tags
}
```

The exact binary layout is an implementation decision and must retain backward compatibility requirements defined by the archive versioning rules.

### Stream identity

Every logical stream receives an immutable identifier independent of transport ports, process IDs, file names, or physical carrier frequencies. A 128-bit UUID or comparably large stable identifier is suitable for the public model.

Stream identity must survive:

- reconnects;
- node migration;
- cloud-region changes;
- archive segmentation;
- transport changes;
- offline re-analysis.

### Stream types

The initial implementation remains audio-oriented, but the model must be extensible to at least:

- audio;
- video;
- image/frame sequences;
- telemetry;
- sensor observations;
- navigation/position data;
- radar/lidar/sonar-derived payloads;
- documents and text;
- transcript segments;
- network/system events;
- model inputs/outputs;
- embeddings and derived features;
- identity and watermark evidence;
- recovery/FEC shards;
- audit/access events;
- authorization/policy records;
- opaque custom provider payloads.

Unknown stream types must remain preservable even when the current engine cannot interpret them.

## Fidelity and truth classes

The existing fidelity model remains foundational.

### S0 — source exact

Accepted source bytes, packets, manifests, headers, sequence information, and transport observations are preserved without mutation.

### S1 — sample/state exact

For decoded or normalized representations where CODEC can deterministically reproduce accepted integer samples or equivalent exact state, the archive may store a losslessly compressed exact representation.

For non-audio streams this class may be extended only where the representation is rigorously defined and deterministic.

### D — derived

Anything produced by inference, resampling, enhancement, translation, summarization, separation, reconstruction, interpolation, denoising, classification, embedding, or other transformation remains derived.

A D-class result must never silently replace S0 or S1.

## Provenance model

Every derived result must be traceable to:

- source stream IDs;
- precise source intervals or records;
- model/runtime identity;
- model hash/version;
- configuration;
- calibration where applicable;
- inference timestamp;
- confidence/quality values;
- transformation chain;
- residual or uncertainty information where meaningful.

The engine should permit later models to append new interpretations without rewriting the original archive history.

## Multiplexing and scale

CODA semantics must not depend on one physical transport channel per feed.

The architecture should distinguish:

- **physical transport** — TCP, QUIC, UDP, file, pipe, shared memory, radio/acoustic carrier, provider protocol;
- **logical stream** — immutable stream ID represented inside the CODEC protocol/archive;
- **processing partition** — worker/shard assignment used internally for scale.

One endpoint may therefore carry many logical streams, and one logical stream may migrate between endpoints while retaining identity and history.

The archive format must remain valid whether the deployment contains one stream, one thousand streams, or tens of millions of registered logical streams. Simultaneous throughput remains bounded by bandwidth, compute, memory, and storage rather than identifier-space or port-number limits.

## Transport and recovery profile

Recovery is a future extension boundary, not a mandatory CODA-core algorithm.

Define an abstraction capable of representing:

- parity/recovery shards;
- erasure/FEC coding groups;
- shard sequence and block identity;
- reconstruction verification hashes;
- transport loss observations;
- partial-recovery state;
- codec enhancement/correction payloads.

Implementations may later use PAR2-compatible concepts, Reed-Solomon, Raptor/fountain coding, or other validated schemes without altering the core archive semantics.

Recovery must distinguish two claims:

1. recovering a damaged/missing encoded bitstream exactly; and
2. reconstructing a pre-lossy-encoding source exactly.

The first can be achieved with sufficient parity. The second requires a complete exactness/correction residual and must never be implied by parity alone.

## Watermark and identity profile

Retain W0/W1/W2 as an audio identity specialization.

The generalized architecture should treat identity evidence as a profile that may bind:

- logical stream ID;
- issuer;
- epoch;
- sequence;
- device/sensor identity;
- trust policy;
- signed metadata;
- physical/acoustic carrier observations.

Watermark identity does not guarantee source separability and does not convert derived neural output into lossless source truth.

## Inference architecture

Inference remains a separate plane from preservation.

Requirements:

- preservation must not block on inference;
- bounded queues and backpressure must be explicit;
- overload may defer or drop D-class work but must not corrupt S0/S1 capture;
- stateful streaming inference should be supported when compatible models exist;
- offline re-inference must be supported from preserved evidence;
- derived outputs must carry model provenance;
- no compatible model means explicit unavailability, not fabricated output.

## Distributed/cloud profile

Cloud and cluster deployment should be represented as a later profile with no effect on archive truth semantics.

A future implementation may include:

```text
global endpoint
  -> regional ingress
  -> stream-ID partitioning
  -> capture workers
  -> recovery workers
  -> inference workers
  -> CODA writers/object storage
  -> query/index services
```

The design must avoid assuming Kubernetes or a specific cloud vendor in core APIs.

Preferred properties:

- horizontal partitioning by stable stream ID;
- stateless edge routing where possible;
- explicit state ownership for stream continuity;
- bounded queues;
- object-store/cold-archive integration;
- hot/cold retention profiles;
- minimal data copies;
- independently scalable preservation and inference planes.

## Confidential-compute and selective-disclosure profile

Future secure deployments may use sealed records and machine-enforced authorization without changing CODA source semantics.

The archive should permit policy metadata and immutable audit records sufficient to support implementations such as:

- encrypted payloads;
- threshold or envelope encryption;
- confidential-compute enclaves;
- scoped query capability tokens;
- purpose/time/data-class restrictions;
- immutable disclosure/access logs;
- selective export of only authorized records.

CODEC core must remain neutral about the legal authority that grants access. It records and enforces declared policy inputs; it does not itself create legal authority.

## Query model

A user or downstream system should eventually be able to query by combinations of:

- stream/source identity;
- time range;
- fidelity class;
- payload/stream type;
- event identity;
- model/version;
- confidence threshold;
- provenance relationship;
- authorization/policy scope.

A query result must retain links to the exact source records that support it.

## ChatGPT/Codex implementation guidance

The README should contain explicit agent guidance so future automated development follows these rules:

1. Treat the README and approved design documents as normative requirements.
2. Inspect repository state and tests before changing behavior.
3. Never claim an unimplemented capability is complete.
4. Keep current implementation-status tables accurate in every release.
5. Preserve deterministic/probabilistic capability boundaries.
6. Prefer narrow, testable primitives over speculative end-to-end stubs.
7. Keep core abstractions vendor-neutral.
8. Implement archive/transport invariants before hyperscale deployment infrastructure.
9. Add compatibility and corruption tests for every archive-format change.
10. Ensure inference failure cannot break preservation.
11. Record model and transformation provenance for every D-class artifact.
12. Benchmark throughput, latency, memory, CPU/GPU use, storage amplification, and recovery overhead before claiming scale.
13. Treat security, privacy, authorization, and retention as first-class interfaces where those profiles are implemented.
14. Update the roadmap when implementation reality changes rather than forcing code to match obsolete dates.

## Delivery sequence

The recommended direction is staged as follows.

### Stage A — preserve current v0.1 foundation

Maintain existing archive, secure capture, signed watermark, API, CLI, and test guarantees.

### Stage B — generalized record semantics

Introduce extensible stream/payload typing, generic provenance envelopes, compatibility tests, and unknown-type preservation.

### Stage C — complete audio 1.0 path

Implement S1 FLAC/self-contained archive profile, real media adapters, stateful identity fusion, compatible neural runtime/model bundles, streaming inference, and offline re-inference.

### Stage D — transport/recovery profile

Add generic recovery-shard semantics, validated FEC implementation(s), streaming repair, and exact verification.

### Stage E — distributed processing profile

Add protocol multiplexing, stream partitioning, distributed workers, object-store backends, query indexes, and operational benchmarks.

### Stage F — trust/selective-disclosure profile

Add encryption envelopes, policy records, scoped access, immutable access logs, and confidential-compute integration where required.

### Stage G — vertical profiles

Build domain-specific schemas, model bundles, policies, and integrations on top of the stable substrate rather than embedding vertical assumptions in CODA core.

## Testing requirements

Every implementation stage must include tests appropriate to its invariants.

At minimum:

- byte-for-byte S0 round trips;
- exact S1 round trips where supported;
- corruption detection and bounded repair;
- unknown stream-type preservation;
- archive-version compatibility;
- sequence/gap/reconnect semantics;
- provenance-link integrity;
- inference-overload isolation;
- deterministic query/extraction behavior;
- authorization-scope enforcement for secure profiles;
- scale benchmarks that state actual hardware and workload.

## Success criteria

This direction is successful when:

1. CODEC can continue shipping an honest audio-first product without breaking current users.
2. The CODA data model no longer assumes that every stream is audio.
3. New stream types can be preserved and indexed without redesigning the archive.
4. Every derived result remains traceable to immutable source evidence.
5. Recovery, cloud, and secure-disclosure systems can be added as profiles instead of forks.
6. The same logical stream semantics work on-device, on-premises, offline, and at cloud scale.
7. Future agents have explicit instructions to build capability incrementally and keep documentation synchronized with implementation reality.

## README implementation scope

The immediate implementation after approval of this design is documentation-only:

- add a concise generalized-platform direction section to `README.md`;
- add normative core invariants for generalized temporal streams;
- add future profile boundaries for recovery, distributed/cloud scale, and selective disclosure;
- add ChatGPT/Codex implementation guidance;
- update the delivery roadmap without changing the truthful current v0.1.0 status;
- explicitly state that these later profiles are planned, not implemented.

No source-code behavior or public archive format changes are part of the immediate README update.
# Stage B.5 Generic Provenance Design

**Date:** 2026-08-27  
**Status:** Approved under the user's global decision approval  
**Base:** `main` at `802da5aa4a91a47883ab1b496b6583374d18252f`  
**Roadmap stage:** B — stabilize stream-first core semantics

## Context

Stage B.4 persists generic S0 timing, sequence, epoch, gap, and exact
source-record integrity links. The remaining Stage B preservation gate is a
generic provenance envelope for exact normalized S1 records and derived D
records. The current CODA development-profile envelope has no truth-class or
provenance field, and changing that envelope would break the compatibility
boundary established by Stages B.2–B.4.

## Goals

- Persist a versioned generic provenance sidecar for S1 and D subjects.
- Link every declared subject and input to an exact committed record using
  stream ID, 16-bit type code, archive sequence, and SHA-256.
- Support multi-input and cross-stream transformations.
- Record generic transformation identity without putting profile semantics in
  core structures.
- Preserve streaming append, structural verification, exact S0 extraction,
  unknown-type preservation, and non-mutating repair.
- Reject malformed or semantically invalid provenance deterministically while
  keeping committed source evidence available.

## Non-goals

- Policy or authorization tags, trust decisions, selective disclosure, or
  access enforcement.
- Interval queries, generic adapter/processor/query APIs, or profile exporters.
- Interpreting confidence, calibration, quality, residual, or uncertainty data
  in generic core.
- Adding provenance to the C ABI in v0.1.
- Changing the 64-byte CODA header, 96-byte record envelope, commit trailer,
  archive format version, or existing record encodings.
- Inferring truth classes or requiring provenance for arbitrary legacy/raw
  records. This milestone validates provenance that callers explicitly append.

## Architecture

Register `RecordType::stream_provenance = 7`. A provenance record uses a
versioned `SPV1` payload and the subject stream ID in its unchanged CODA record
envelope. It is appended after its subject. Every input must precede the
subject, which makes the provenance graph acyclic by construction.

The sidecar approach keeps payload bytes and profile encodings untouched. A
subject can therefore be a registered or unknown record type, while the
provenance layer remains generic. Transformation chains are represented by
linking a later subject to an earlier S1/D subject; readers traverse the graph
through exact subject/input links rather than a mutable chain field.

## Public C++ model

Add the following archive-facing types to `include/codec/archive.hpp`:

```cpp
struct ProvenanceRecordLink {
  StreamId stream{};
  RecordTypeCode type{};
  std::uint64_t sequence{};
  Sha256 hash{};
};

struct ProvenanceProcess {
  std::string operation;
  std::string implementation_id;
  std::string implementation_version;
  std::optional<Sha256> implementation_hash;
  std::optional<Sha256> configuration_hash;
  std::int64_t created_utc_ns{};
  std::string details_type;
  std::vector<std::byte> details;
};

struct StreamProvenance {
  TruthClass subject_truth{TruthClass::derived};
  ProvenanceRecordLink subject{};
  std::vector<ProvenanceRecordLink> inputs;
  ProvenanceProcess process;
};
```

`implementation_id` identifies the transformation implementation, runtime, or
model pipeline. `implementation_hash` and `configuration_hash` are optional
exact identifiers; they do not authenticate the identified artifact. Profile
metadata such as confidence, calibration, quality, or residual information can
be stored as integrity-protected opaque `details` with a required media/type
identifier. Generic core preserves those bytes but does not interpret them.

Add these APIs:

```cpp
Result<RecordInfo> CodaWriter::append_stream_provenance(
    const RecordInfo& subject, TruthClass subject_truth,
    std::span<const RecordInfo> inputs,
    const ProvenanceProcess& process);

Result<std::vector<StreamProvenance>> CodaArchive::provenance(
    ArchiveReadPolicy policy = ArchiveReadPolicy::complete_archive) const;
```

The writer accepts `RecordInfo` values rather than caller-built links so it can
prove that every link matches the exact records committed by that writer.

## SPV1 development-profile encoding

`SPV1` is a little-endian, version-1, variable-length payload containing:

- magic, version, reserved fields, truth class, and optional-hash flags;
- input count and byte lengths for every variable field;
- creation time;
- one fixed-size subject link;
- optional implementation and configuration hashes;
- a fixed-size array of input links;
- operation, implementation ID/version, details type, and opaque details bytes.

Each record link encodes the 16-byte stream ID, 16-bit type code, reserved
field, 64-bit archive sequence, and 32-byte SHA-256. Unknown flags, nonzero
reserved fields, impossible length arithmetic, or trailing bytes are corrupt.

Development-profile resource bounds are deterministic:

- 1–4096 input links;
- required operation, implementation ID, and implementation version, each no
  more than 4096 bytes;
- optional details type no more than 4096 bytes;
- optional details no more than 1 MiB;
- details type and details must either both be empty or both be nonempty.

Text fields are preserved byte-exactly and may not contain an embedded NUL.
The development profile does not normalize or reinterpret them.

## Writer validation and data flow

`append_stream_provenance()` performs all validation before appending:

1. Confirm the writer is active.
2. Resolve the subject to one exact committed record by sequence, stream, type
   code, and hash.
3. Require `state_exact` or `derived`; reject `source_exact`.
4. Reject provenance and final-index subjects.
5. Require at least one unique input. Resolve every input exactly, reject
   provenance/final-index inputs, and require every input sequence to precede
   the subject sequence.
6. Reject a second provenance sidecar for the same subject.
7. Validate process fields and resource bounds.
8. Encode `SPV1` and append it on the subject stream with the subject's envelope
   start/end times.

The writer updates its duplicate-subject state only after the provenance record
commits successfully.

## Reader validation and failure isolation

`CodaArchive::provenance()` scans the selected structurally verified record
list in archive order. For every `stream_provenance` record it decodes `SPV1`
and repeats the writer's semantic checks against the actual record list:

- the subject exists, precedes the provenance record, matches every link field,
  and uses the same stream as the sidecar envelope;
- every input exists, matches exactly, and precedes the subject;
- input links and subject provenance are unique;
- subject truth class, flags, reserved fields, lengths, and process metadata
  are valid.

Typed writer errors return `invalid_argument`. Hash-valid malformed or
semantically invalid persisted metadata makes `provenance()` return
`archive_corrupt`. Structural `verify()`, `records()`, `read_payload()`, raw or
typed extraction, descriptors, feeds, continuity, and repair do not call the
provenance interpreter. Invalid optional metadata therefore cannot corrupt or
block committed S0 evidence.

Legacy archives and archives without provenance return an empty provenance
view. `ArchiveReadPolicy::verified_prefix` applies identically to existing
metadata views.

## Repair and compatibility

Repair copies the structurally valid prefix using the existing raw-code path.
Because recovered records retain their order, envelope fields, and chain prefix,
their record hashes remain stable and `SPV1` links remain exact. The repaired
archive receives a new final index only; no provenance payload is rewritten.

The C ABI and all existing C++ append/extract APIs remain source-compatible.
Unknown record types remain preservable. The new public model is explicitly
part of the provisional development profile and does not freeze CODA v1.

## Security and privacy

Provenance hashes identify exact bytes; they are not signatures or trust
decisions. Callers remain responsible for authorization and for redacting
secrets before producing `details`. Configuration bytes are not stored by the
generic model; only an optional digest is retained. All decoding uses checked
size arithmetic and deterministic resource limits.

## Test strategy

Use test-driven development, with each behavior observed failing before its
production implementation:

1. Round-trip one S1 and one multi-input, cross-stream D provenance record,
   comparing every field and exact record link.
2. Reject writer inputs with an S0 subject, absent or forged records, sources
   that do not precede the subject, duplicates, prohibited record types,
   duplicate subject provenance, invalid process fields, and exceeded bounds.
3. Inject hash-valid raw `SPV1` fixtures with invalid magic, version, reserved
   fields, flags, truth class, counts, lengths, or trailing bytes.
4. Inject exact-link failures for missing, later, wrong-stream, wrong-type, or
   wrong-hash subjects/inputs, duplicate inputs, and duplicate subject
   provenance.
5. Prove malformed provenance does not affect structural verification or exact
   S0 extraction.
6. Prove repair preserves valid provenance and every subject/input link.
7. Prove legacy archives expose an empty provenance view.
8. Run the complete Release and ASan/UBSan suites, capability sanity, and an
   installed-package consumer using the public provenance API.

## Documentation and claim boundary

On successful proof, update README and CHANGELOG to claim only that the
provisional development profile persists and validates declared S1/D
provenance with exact record links and opaque profile details. Continue to list
policy tags, generic processing/query boundaries, profile interpretation,
models, distributed execution, trust, and frozen CODA v1 as unimplemented.

## Completion criteria

Stage B.5 is complete when the typed API, reader, corruption isolation, repair,
compatibility, package consumer, Release suite, sanitizer suite, exact-head CI,
README, CHANGELOG, and roadmap evidence log all agree with this design.

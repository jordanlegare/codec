# Stage D.3 Provenance-Verified PCM16 State Reader Design

## Purpose

Stage D.1 defines deterministic Audio Profile PCM16 S1 state (`Pcm16State`)
and its exact APS1 encoding. Stage D.2 adds a preservation-first WAV ingest
workflow that stores exact WAV S0 first and conditionally appends APS1 plus a
`state_exact` provenance sidecar.

What is still missing is a profile-owned archive consumption boundary. A caller
can currently query `pcm16` physical records, decode APS1, and separately query
provenance, but the caller must correctly join and validate those views before
claiming S1. That duplicates a truth-sensitive operation in every exporter,
inference implementation, or offline analysis path.

Stage D.3 adds one bounded, fail-closed Audio Profile query that returns decoded
PCM16 state only when the archive is finalized, generic archive verification
succeeds, the selected physical record is the exact subject of valid
`state_exact` provenance, and that provenance resolves to the exact same-stream
S0 source record expected from the D.2 workflow.

## Decision

Add a profile-only `query_verified_pcm16_states()` API. It consumes an already
opened `CodaArchive`; it does not create a second archive reader, normalize
audio, or persist anything.

The API uses the existing generic `RecordQuery`, `ProvenanceQuery`, archive
verification, physical record metadata, payload verification, and D.1 APS1
decoder. The generic archive surface remains unchanged.

Unprovenanced `pcm16` records are not errors and are not returned as S1. They
remain physical records with no truth classification. In contrast, a selected
`state_exact` PCM16 provenance declaration whose lineage contradicts the Audio
Profile contract is an integrity/semantic failure and the query fails closed.

## Alternatives Considered

### Return every PCM16 record and attach optional provenance

Rejected. It would force every downstream caller to decide which records are
safe to treat as S1 and would recreate the ambiguity D.3 is intended to remove.

### Infer S1 from `RecordType::pcm16`

Rejected. D.1 explicitly established that the record type alone does not imply
truth class. S1 requires valid provenance.

### Add audio-specific methods to `CodaArchive`

Rejected. CODA/core must remain profile neutral. The query belongs under
`codec::profiles::audio` and composes existing generic archive primitives.

### Require the exact D.2 process identity string

Rejected. The trusted boundary must validate truth and exact lineage, while
preserving generic process metadata for inspection. Requiring one implementation
ID/version would unnecessarily prevent a future compatible Audio Profile
canonicalizer from producing valid APS1 S1 with equivalent exact lineage.

## Public API

Create `include/codec/profiles/audio_state_reader.hpp`:

```cpp
namespace codec::profiles::audio {

struct Pcm16StateQuery {
  std::optional<StreamId> stream;
  std::optional<RecordTimeRange> time;
  std::size_t maximum_results{1024};
  std::uint64_t maximum_encoded_bytes{64ULL * 1024ULL * 1024ULL};
};

struct VerifiedPcm16State {
  Pcm16State state;
  RecordInfo state_record;
  RecordInfo source_record;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedPcm16State>> query_verified_pcm16_states(
    const CodaArchive& archive, const Pcm16StateQuery& query = {});

}  // namespace codec::profiles::audio
```

The canonical `<codec/profiles/audio.hpp>` facade includes this header. No new
root-level `codec::*` compatibility symbol is added because D.3 is new profile
behavior.

`VerifiedPcm16State` deliberately returns both decoded state and exact physical
metadata. Downstream exporters/inference can consume `state` while retaining
`state_record`, `source_record`, and the full `StreamProvenance` evidence used
to justify the S1 classification.

## Query Contract

The query accepts optional logical stream and authenticated envelope-time
filters. Time uses the existing half-open `RecordTimeRange` semantics.

Caller bounds are mandatory and non-zero:

- `maximum_results` limits returned verified states;
- `maximum_encoded_bytes` limits the sum of selected APS1 physical payload
  sizes before state payloads are read/decoded.

An invalid time range is rejected through the existing generic record-query
validation. Bounds of zero return `ErrorCode::invalid_argument`. Exceeding a
bound returns `ErrorCode::resource_exhausted`.

## Finalized Archive Requirement

D.3 intentionally accepts only finalized archives. Before semantic selection,
`archive.verify()` must report both `ok == true` and `finalized == true`.

If generic verification fails, D.3 propagates the verification report's error
code/message. If verification is otherwise successful but no final index is
present, D.3 returns `ErrorCode::archive_corrupt` with an explicit finalized
archive requirement.

There is no `verified_prefix` mode in this profile API. A partial archive may
still be examined through generic CODA APIs, but D.3 does not classify partial
state as trusted S1.

## Selection and Lineage Validation

D.3 builds a physical subject query with:

- `RecordType::pcm16` fixed by the profile;
- optional caller stream filter;
- optional caller time filter.

It then calls the existing provenance query using:

- `subject_truth = TruthClass::state_exact`; and
- the physical PCM16 query as the subject filter.

Only provenance-backed PCM16 subjects enter the D.3 candidate set. A physical
PCM16 record with no matching state-exact sidecar is ignored, preserving D.1's
rule that record type alone does not classify truth.

For each selected provenance object, D.3 requires:

1. the exact subject link resolves to one physical `pcm16` record;
2. there is exactly one direct input;
3. the direct input resolves exactly to `RecordType::source_bytes`;
4. source and subject have the same logical `StreamId`;
5. source and subject have identical authenticated `start_ns`/`end_ns`
   intervals; and
6. the exact physical stream/type/sequence/hash values in both links match the
   current verified archive records.

The generic provenance reader already validates backward-only record existence,
hashes, duplicate inputs, and metadata restrictions. D.3 adds the Audio
Profile-specific one-source, same-stream, same-interval constraints defined by
the D.2 ingest contract.

If any selected state-exact PCM16 provenance object violates those profile
constraints, D.3 returns `ErrorCode::archive_corrupt` rather than silently
skipping the contradictory truth declaration.

## APS1 Decode

After lineage and resource-bound validation, D.3 reads the exact subject payload
through `CodaArchive::read_payload()` and passes it unchanged to
`decode_pcm16_state()`.

APS1 decode is therefore the sole state interpretation. D.3 performs no
resampling, remixing, sample conversion, WAV decoding, normalization,
watermarking, enhancement, inference, or repair.

A malformed APS1 payload with otherwise valid state-exact provenance returns
the existing `ErrorCode::decode` error. The source S0 remains independently
available through generic archive extraction.

## Ordering

Results preserve provenance/archive order from the existing generic provenance
query. D.3 does not sort by time, stream, or sample count.

This keeps deterministic behavior and avoids inventing a second ordering model.

## Truth and Compatibility

- S0 remains the exact accepted source representation.
- APS1 remains the sole PCM16 S1 representation defined by D.1.
- A `pcm16` physical tag without valid state-exact provenance remains
  unclassified and is not returned by D.3.
- A returned `VerifiedPcm16State` is justified by both valid APS1 and exact
  state-exact S0 lineage.
- D is unchanged.
- CODA header/envelope/version/record types are unchanged.
- Generic `CodaArchive`, `RecordQuery`, and `ProvenanceQuery` signatures are
  unchanged.
- Root audio ABI, generic Engine, CLI, C ABI, legacy feed behavior, WAV,
  watermark, statement, and inference boundaries remain compatible.

## Verification Contract

Tests must prove:

1. a finalized D.2 success archive returns exactly one verified state with the
   expected sample rate, channels, samples, state record, source record, and
   full provenance;
2. stream and half-open time filters deterministically select the same state;
3. a valid physical APS1 `pcm16` record without state-exact provenance is not
   returned as S1;
4. a selected state-exact PCM16 declaration with a non-source direct input
   fails closed with `archive_corrupt`;
5. a selected state-exact PCM16 declaration with a source from another stream
   or a different interval fails closed;
6. a malformed APS1 payload carrying otherwise valid state-exact lineage
   returns `decode`;
7. an unfinalized archive is rejected as `archive_corrupt`;
8. zero limits are rejected and result/payload bounds fail as
   `resource_exhausted` before unbounded state decoding;
9. existing D.1/D.2, archive, Engine, CLI, C ABI, watermark, statement, and
   unavailable inference behavior remains green; and
10. GCC, Clang, package install, AI-contract, and sanitizer CI remain green.

Mutation proof: changing a valid D.2 provenance input from the exact S0 record
to another physical record must make the new reader test fail; restoring exact
S0 lineage must return green.

## Documentation Claim

README and changelog may claim that the Audio Stream Profile provides a bounded,
provenance-verified PCM16 state query over finalized archives and returns APS1
S1 only when exact source lineage validates.

They must not claim that all PCM16 physical records are S1 or that D.3 adds
FLAC, media conversion, inference, identity fusion, or Audio Profile 1.0
completion.

## Explicit Non-Claims

D.3 does not add or claim:

- FLAC or another lossless external representation;
- additional media adapters/codecs, float PCM, conversion, resampling,
  remixing, dither, channel-layout semantics, enhancement, or reconstruction;
- a new ingest/normalization path or archive writer transaction;
- CODA format/version/record changes;
- generic query/provenance API changes;
- CLI or C ABI access to verified PCM16 state;
- model/runtime bundles, neural availability, streaming/offline inference,
  diarization, embeddings, or identity fusion;
- recovery/FEC, scale, deployment, frozen CODA v1, or completion of Stage D.

The next Stage D milestone should consume this trusted state boundary rather
than independently re-join physical records and provenance.
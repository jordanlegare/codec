# Stage D.2 Preservation-First Audio PCM16 WAV Ingest Design

## Status and Context

Stage D.1 defines one container-independent, deterministic Audio Profile S1
state: `Pcm16State`, encoded as APS1. Generic CODA APIs can store source bytes,
APS1 bytes, and exact state provenance, but callers must currently coordinate
capture, WAV decoding, canonicalization, record ordering, failure handling,
and finalization themselves.

That leaves a preservation risk at the first production-facing Audio Profile
boundary. A caller can decode before preserving source bytes, accidentally
discard S0 when media interpretation fails, parse a different file snapshot
than the one it stores, or persist a `pcm16` record without the provenance
required for an S1 claim.

Stage D.2 adds one profile-scoped workflow that makes the correct ordering the
default while retaining every generic CODA primitive unchanged.

## Decision

Implement a preservation-first Audio PCM16 WAV ingest operation in
`codec::profiles::audio`.

The operation captures one bounded exact source snapshot through the existing
hardened capture path. It then creates a new archive, persists a generic audio
stream descriptor and the exact snapshot as S0, and only afterward attempts
WAV decoding and D.1 canonicalization. Valid PCM16 WAV input additionally
produces APS1 plus exact `state_exact` provenance. Profile interpretation
failure produces a finalized, verified S0-only archive and an explicit
`profile_error` in the successful ingest report.

This is a logical preservation transaction, not a filesystem or database
atomicity claim. Validation and capture failures occur before archive creation.
Archive I/O failures may still leave the verified prefix behavior already
defined by generic CODA.

## Alternatives Considered

### All-or-nothing archive publication

Building a temporary archive and publishing it only when S1 succeeds appears
transactional, but it violates the repository invariant that optional profile
failure must not discard accepted S0. It is rejected.

### Extend `codec::Engine`

Adding automatic WAV decoding to generic recording would couple core capture
to sample rates, channels, RIFF, and PCM. It is rejected because the behavior
belongs to the Audio Stream Profile.

### Caller-composed generic APIs only

The existing APIs remain valid, but they do not prove correct ordering or
single-snapshot behavior. Keeping only the manual path would leave the D.2
failure-isolation gate unmet.

## Public API

Add `include/codec/profiles/audio_ingest.hpp`:

```cpp
namespace codec::profiles::audio {

struct Pcm16WavIngestRequest {
  std::string source_uri;
  std::filesystem::path archive_path;
  StreamDescriptor descriptor;
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::size_t capture_chunk_bytes{256U * 1024U};
  std::uint64_t maximum_source_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint32_t maximum_redirects{5};
  bool deny_private_network{true};
};

struct Pcm16WavIngestReport {
  std::filesystem::path archive_path;
  RecordInfo descriptor;
  RecordInfo source;
  std::optional<RecordInfo> state;
  std::optional<RecordInfo> provenance;
  std::optional<Error> profile_error;

  bool state_exact() const noexcept {
    return state.has_value() && provenance.has_value() &&
           !profile_error.has_value();
  }
};

Result<Pcm16WavIngestReport> ingest_pcm16_wav(
    const Pcm16WavIngestRequest& request);

}  // namespace codec::profiles::audio
```

The canonical `<codec/profiles/audio.hpp>` facade includes this header. No new
root-level `codec::*` symbol is required: D.2 is new profile behavior, while
all existing root audio symbols and ABI remain unchanged.

## Request Contract

Before source capture or archive creation, the operation requires:

- a non-empty source URI;
- an archive path that names a file;
- `end_ns >= start_ns`;
- capture chunks in the inclusive range 4 KiB through 16 MiB;
- a non-zero maximum source byte count representable by `std::size_t`;
- no more than 20 redirects;
- a generic descriptor accepted by the existing descriptor encoder;
- `descriptor.type == StreamType::audio`; and
- `descriptor.payload_type == "audio/wav"`.

The destination must not already name a file, directory, hard-link target, or
symlink. A read-only early check avoids unnecessary capture, while
`CodaWriter::create` remains the race-safe `O_EXCL | O_NOFOLLOW` authority.

The source URI supports only the schemes already supported by
`PreparedCapture`: local paths, `file://`, stdin (`-`), HTTP, and HTTPS.
Existing no-follow local-file handling, private-network policy, redirect
limits, and byte limits are reused. No new authority or access bypass is
introduced. The source URI is never persisted automatically; the caller owns
the already-redacted `descriptor.source_id` value.

## Single-Snapshot Data Flow

1. Validate the complete request without creating the output.
2. Prepare and run the existing hardened capture into one bounded owned byte
   vector. Capture failure returns an outer `Result` error and creates no
   archive.
3. Create the destination archive without replacing any existing entry.
4. Append the supplied generic `StreamDescriptor`.
5. Append the captured vector unchanged as one `source_bytes` S0 record.
6. Decode PCM16 WAV directly from that same in-memory vector.
7. On decode/canonicalize/APS1-encode failure, finalize the archive with only
   descriptor and S0, then return a report containing `profile_error` and no
   state/provenance records.
8. On success, append APS1 as the existing `RecordType::pcm16` under the same
   stream and interval.
9. Append `TruthClass::state_exact` provenance whose only input is the exact
   S0 record and whose fixed process identity is:
   - operation: `audio.pcm16.canonicalize`
   - implementation ID: `codec-audio-profile`
   - implementation version: `1`
   - created time: `request.end_ns`
10. Finalize and return all exact record metadata.

Successful record order is descriptor, source bytes, PCM16 state, provenance,
and final index. Source-only order is descriptor, source bytes, and final
index.

## WAV Decoder Refactor

The current path-based `WavPcm16::read` already reads a byte vector and parses
it. Move only the parser into an internal
`detail::decode_wav_pcm16(std::span<const std::byte>)` declared in
`src/audio/wav_codec.hpp`.

Both `WavPcm16::read` and D.2 call this one parser. It is not installed and is
not public API. The refactor preserves all existing WAV acceptance and error
behavior while proving that D.2 interprets exactly the captured S0 bytes and
never reopens the source.

## Result and Failure Semantics

An outer successful `Result<Pcm16WavIngestReport>` guarantees:

- the archive finalized successfully;
- the report's descriptor and S0 records are exact records in that archive;
- S0 independently extracts byte-for-byte; and
- exactly one of these states holds:
  - `state_exact() == true`, both optional records are present, and the
    provenance sidecar binds the APS1 subject to the exact S0 input; or
  - `profile_error` is present and both optional records are absent.

Request validation, capture, archive creation, record commit, provenance
commit, or finalization failure returns an outer error. D.2 does not weaken
the generic verified-prefix semantics for archive I/O failure.

Invalid or unsupported WAV content is a profile error, not a preservation
failure. The report keeps the decoder's exact error code and message.

## Truth and Compatibility

- Captured bytes remain S0 exactly as accepted.
- APS1 remains the sole Audio PCM16 S1 representation.
- A `pcm16` tag without valid `state_exact` provenance remains unclassified as
  S1.
- D is untouched.
- CODA header, record envelope, record types, version, C ABI, CLI, legacy
  `--feed`, generic `Engine`, and root audio symbols do not change.
- The operation adds no automatic inference, watermarking, identity, or
  recovery behavior.

## Verification Contract

Tests must prove:

1. a literal PCM16 WAV with noncanonical container bytes (including an unknown
   RIFF chunk) is stored byte-for-byte as S0;
2. that same exact snapshot decodes to the expected D.1 state and APS1 bytes;
3. the descriptor, record order, stream, interval, type, sequence, and hashes
   are exact;
4. the state-exact sidecar has the fixed process identity and exact S0 input;
5. invalid WAV bytes still produce a finalized, verified S0-only archive plus
   an explicit decode error;
6. invalid request shape, byte-limit failure, private/unsupported source, and
   output collision create no new archive and never replace an existing file;
7. existing WAV, archive, engine, CLI, C ABI, watermark, and unavailable-model
   behavior remains green;
8. an installed-package consumer completes success-path ingest and exact
   readback; and
9. Release and Debug ASan/UBSan suites pass with unchanged capabilities.

Mutation proofs must show that decoding before S0 persistence breaks the
invalid-media preservation test and that omitting the provenance sidecar
breaks the success-path S1 proof.

## Documentation Claim

The README and changelog may claim a profile-scoped, bounded,
preservation-first PCM16 WAV ingest operation that captures one exact S0
snapshot and conditionally appends D.1 APS1 S1 with exact provenance.

They must state that profile failure returns a finalized S0-only archive, and
must not call the operation a general media pipeline or filesystem-atomic
transaction.

## Explicit Non-Claims

D.2 does not add or claim:

- FLAC, float PCM, additional WAV encodings, sample-width conversion,
  resampling, remixing, dithering, channel-layout interpretation, enhancement,
  concealment, or reconstruction;
- multi-stream or incremental/streaming PCM canonicalization;
- filesystem-wide atomicity or rollback after archive I/O failure;
- new capture schemes, authorization, DRM/access-control bypass, or automatic
  persistence of unredacted source URIs;
- CODA format/version/record changes, CLI or C ABI access, or root ABI
  migration;
- model/runtime bundles, neural availability, streaming/offline inference,
  diarization, embeddings, or identity fusion;
- recovery/FEC, performance, scale, deployment, frozen CODA v1, or completion
  of Stage D or Audio Stream Profile 1.0.

The next Stage D dependency should consume finalized D.2 archives and D.1
state rather than introduce a second ingest path or normalization target.

# Stage D.4 Provenance-Verified PCM16 WAV Export Design

## Purpose

Stage D.1 defines deterministic Audio Profile PCM16 S1 state and the APS1
encoding. Stage D.2 provides preservation-first WAV ingest that commits exact
S0 before optional APS1 S1. Stage D.3 provides the trusted finalized-archive
read boundary that returns PCM16 S1 only when APS1 and exact `state_exact`
source lineage both validate.

Stage C.5 already provides a generic bounded `StreamExporter` execution
boundary for typed external representations, but the Audio Stream Profile has no
profile exporter that consumes trusted D.3 state. Callers can currently decode
APS1 and write WAV manually, which duplicates profile output logic and makes it
easier to accidentally treat a physical `pcm16` tag as verified S1.

Stage D.4 adds one bounded, profile-owned WAV export workflow over D.3 trusted
state. It exports each selected verified PCM16 state independently as a
canonical PCM16 RIFF/WAVE byte vector and retains the exact state/source
provenance evidence that justified the S1 input.

## Decision

Add a high-level profile-only `export_verified_pcm16_wav()` API. It accepts an
opened `CodaArchive`, the existing D.3 `Pcm16StateQuery`, and an aggregate output
byte limit. It first calls `query_verified_pcm16_states()`. Only D.3 results are
eligible for export.

For each verified state, D.4 reads the exact APS1 subject record, invokes an
internal Audio Profile `StreamExporter` through the existing C.5
`invoke_exporter()` boundary, and returns the generated `audio/wav` bytes plus
the exact D.3 state/source/provenance evidence.

The raw exporter implementation is intentionally not exposed as a public Audio
Profile class. This keeps the canonical public D.4 path truth-aware: a caller
cannot obtain the D.4 API's verified-export result merely by presenting an
unprovenanced physical `pcm16` record.

## Alternatives Considered

### FLAC as D.4

Deferred. FLAC remains an Audio Profile 1.0 objective, but it introduces a new
codec/dependency and a larger compatibility/test surface. D.3's immediate
unblocked dependency is a concrete consumer of trusted state, and WAV already
has exact PCM16 semantics in the repository. A verified WAV export proves the
trusted state-to-output boundary before adding another codec.

### Identity-fusion hardening as D.4

Deferred. Identity fusion is also a Stage D objective, but it does not consume
the newly established D.3 state boundary and would leave the trusted S1 output
path unproven.

### Public `Pcm16WavExporter : StreamExporter`

Rejected for D.4. Such an exporter could legitimately transform any syntactically
valid APS1 physical record, but callers could mistake that transform for an S1
verification step. D.4 instead keeps the generic exporter private and exposes
only the wrapper that obtains its inputs from D.3.

### Write WAV files directly

Rejected. C.5 export semantics are caller-paced and perform no filesystem
writes. D.4 returns owned bytes and leaves file/network/storage placement to the
caller.

### Concatenate all selected PCM16 states into one WAV

Rejected. Concatenation would require new continuity, format-change, gap, and
interval semantics. D.4 returns one independent WAV per D.3 verified state in
D.3 order.

## Public API

Create `include/codec/profiles/audio_export.hpp`:

```cpp
namespace codec::profiles::audio {

struct Pcm16WavExportLimits {
  std::uint64_t maximum_output_bytes{64ULL * 1024ULL * 1024ULL};
};

struct VerifiedPcm16WavExport {
  ExportResult output;
  RecordInfo state_record;
  RecordInfo source_record;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedPcm16WavExport>> export_verified_pcm16_wav(
    const CodaArchive& archive,
    const Pcm16StateQuery& query = {},
    Pcm16WavExportLimits limits = {});

}  // namespace codec::profiles::audio
```

The canonical `<codec/profiles/audio.hpp>` facade includes this header. No
root-level `codec::*` compatibility alias is added because D.4 is new profile
behavior.

`VerifiedPcm16WavExport::output.payload_type` is always `"audio/wav"` on
success. `output.supporting_records` contains exactly the physical APS1 state
record passed through `invoke_exporter()`. `state_record`, `source_record`, and
`provenance` retain the full D.3 evidence chain back to exact S0.

## Input and Selection Contract

D.4 delegates archive verification, stream/time selection, result-count bounds,
APS1 aggregate encoded-byte bounds, exact provenance-link resolution, and APS1
decode validation to D.3.

Consequences:

- an unfinalized or corrupt archive fails exactly as D.3 defines;
- an unprovenanced physical `pcm16` record is ignored and produces no export;
- a selected contradictory `state_exact` PCM16 declaration fails closed;
- malformed APS1 with otherwise valid lineage returns `ErrorCode::decode`;
- optional stream and half-open time filters have exactly D.3 semantics.

D.4 does not independently infer truth from `RecordType::pcm16` and does not
re-implement the D.3 provenance join.

## Output Resource Contract

`Pcm16WavExportLimits::maximum_output_bytes` is an aggregate bound over all WAV
payloads returned by one call. Zero is invalid and returns
`ErrorCode::invalid_argument` before D.3 querying or export invocation.

Before any WAV output allocation for a selected state, D.4 computes the exact
encoded PCM16 RIFF/WAVE byte count:

```text
44-byte canonical RIFF/WAVE header + 2 bytes per interleaved PCM16 sample
```

The existing RIFF 32-bit size limit remains authoritative. Invalid PCM shape or
RIFF overflow uses the same existing `invalid_argument` /
`resource_exhausted` semantics as `WavPcm16::write`.

D.4 accumulates expected output sizes using overflow-safe subtraction. If the
next output would exceed `maximum_output_bytes`, the entire call returns
`ErrorCode::resource_exhausted` before allocating that WAV or invoking the
exporter for it. No partial vector is returned.

## Shared WAV Encoding

The existing `WavPcm16::write()` implementation currently builds canonical
44-byte PCM16 RIFF/WAVE bytes inline and then writes them to disk. D.4 needs the
same bytes in memory.

Refactor the implementation into internal helpers in `src/audio/wav_codec.hpp`
and `src/audio/wav.cpp`:

```cpp
Result<std::size_t> encoded_wav_pcm16_size(
    std::uint32_t sample_rate,
    std::uint16_t channels,
    std::size_t sample_count);

Result<std::vector<std::byte>> encode_wav_pcm16(
    std::uint32_t sample_rate,
    std::uint16_t channels,
    std::span<const std::int16_t> samples);
```

These remain `codec::detail` implementation APIs, not new public root symbols.
`WavPcm16::write()` delegates to `encode_wav_pcm16()` then to the existing file
writer. Therefore existing WAV file bytes and validation behavior must remain
byte-for-byte compatible.

The D.4 internal exporter decodes its one exact APS1 input using
`decode_pcm16_state()` and calls the same internal encoder. This means file WAV
writing and verified archive WAV export have one encoding implementation.

## Generic Exporter Integration

D.4 defines a private implementation of `StreamExporter` in the Audio Profile
implementation file. Its contract is deliberately narrow:

1. exactly one input record is required;
2. the input physical type must be `RecordType::pcm16`;
3. the input payload must decode as APS1 through `decode_pcm16_state()`;
4. output payload type is exactly `audio/wav`;
5. output bytes come from the shared internal WAV encoder.

The high-level D.4 wrapper obtains the exact state payload from
`VerifiedPcm16State::state_record`, constructs one exact `ExtractedRecord`, and
calls `invoke_exporter()` with one-input limits. `CodaArchive::read_payload()`
verifies the exact record/trailer hash before returning those bytes;
`invoke_exporter()` then validates the input payload size and resource limits
and returns the exact state support link.

The wrapper does not read the full S0 source payload merely to manufacture an
additional exporter support link. Exact S0 lineage is already retained in the
returned `source_record` and `StreamProvenance`; avoiding the redundant source
payload read preserves resource proportionality.

## Ordering

D.4 preserves the D.3 result order. It does not sort, concatenate, merge, or
resample states. Each `VerifiedPcm16WavExport` corresponds to exactly one D.3
`VerifiedPcm16State`.

## Truth and Provenance Semantics

- S0 remains the exact accepted source representation.
- APS1 remains the implemented PCM16 S1 representation.
- D.3 remains the sole D.4 truth-verification gate.
- WAV export bytes are an external lossless representation of verified PCM16
  state; D.4 does not append a new CODA S1 or D record and therefore does not
  assign an archive truth class to the returned external bytes.
- `output.supporting_records` identifies the exact state record consumed by the
  generic exporter.
- `source_record` and `provenance` preserve the exact S0 evidence chain used by
  D.3.
- A raw `pcm16` record with no valid D.3 provenance cannot produce a
  `VerifiedPcm16WavExport` through the public D.4 API.

## Error Handling

D.4 preserves upstream D.3 errors unchanged.

Additional D.4 failures are deterministic:

- `maximum_output_bytes == 0` -> `invalid_argument`;
- selected outputs exceed the aggregate caller byte bound ->
  `resource_exhausted`;
- state record payload read fails -> propagate the archive error unchanged;
- internal exporter contract violation -> `invalid_argument`;
- malformed APS1 -> `decode`;
- RIFF 32-bit size overflow -> `resource_exhausted`;
- generic exporter output-bound/type validation -> preserve existing C.5 error
  semantics.

No output is persisted by D.4, so export failure cannot modify the archive or
weaken committed S0/S1 preservation.

## Verification Contract

Tests must prove:

1. an actual D.2 `ingest_pcm16_wav` success archive exported through D.4 yields
   exactly one `audio/wav` result with samples/rate/channels equal to the D.1
   state and with exact D.3 state/source/provenance evidence;
2. D.4 WAV bytes are byte-for-byte identical to existing `WavPcm16::write()`
   output for the same PCM state;
3. the generic exporter support link equals the exact D.3 state record;
4. stream and half-open time filters are inherited from D.3 and preserve order;
5. an unprovenanced valid APS1 `pcm16` physical record produces no export;
6. contradictory source type/stream/interval provenance fails closed via D.3;
7. malformed APS1 with otherwise valid state-exact lineage returns `decode`;
8. zero output limit is rejected before export and a too-small aggregate output
   limit returns `resource_exhausted`;
9. multiple verified states return independent WAV outputs in D.3/provenance
   order, while an aggregate limit that fits the first but not all outputs fails
   the whole call;
10. existing `WavPcm16::write()` byte format and validation remain compatible;
11. D.1/D.2/D.3, generic exporter, archive, Engine, CLI, C ABI, watermark,
   statement, and unavailable inference tests remain green; and
12. GCC, Clang, package install, AI-contract, and sanitizer CI remain green.

Mutation proof: alter a D.4 test fixture so the `state_exact` subject points to
an invalid source lineage; D.4 must fail rather than emit WAV. Restore exact D.2
lineage and the same export must return green.

## Documentation Claim

README and changelog may claim that the Audio Stream Profile provides a bounded,
provenance-verified PCM16 WAV export path over finalized archives, returning
lossless in-memory WAV bytes with exact state/source evidence.

They must not claim that every physical `pcm16` record is S1, that WAV is a new
CODA truth record, that FLAC is implemented, or that Audio Stream Profile 1.0 / Stage D is complete.

## Explicit Non-Claims

D.4 does not add or claim:

- FLAC storage/export or another new codec;
- media/container adapters beyond the existing PCM16 WAV behavior;
- float PCM, resampling, remixing, dither, channel-layout interpretation,
  enhancement, concealment, or reconstruction;
- archive writes, automatic persistence, CLI export, C ABI export, or network
  delivery;
- CODA header/envelope/version/record changes;
- generic `StreamExporter` API changes;
- model/runtime bundles, neural availability, streaming/offline inference,
  diarization, embeddings, or identity fusion;
- recovery/FEC, scale, deployment, frozen CODA v1, or completion of Stage D.

The next Stage D milestone should be selected from the remaining Audio Profile
1.0 objectives after D.4 is merged, with FLAC/media-adapter work preferred before
neural-runtime claims unless repository evidence identifies a smaller blocking
dependency.

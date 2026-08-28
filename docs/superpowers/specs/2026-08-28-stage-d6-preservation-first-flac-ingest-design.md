# Stage D.6 Preservation-First Native FLAC Ingest Design

## Status

Approved for autonomous implementation on 2026-08-28 as the next Audio Stream Profile 1.0 milestone after D.5.

## Context

Stage D is active. D.1 defines deterministic APS1 PCM16 S1, D.2 provides preservation-first PCM16 WAV ingest, D.3 is the trusted provenance-verified state reader, D.4 and D.5 export verified state as WAV and FLAC. The remaining Stage D exit scope still includes production media adapters beyond the bounded WAV path.

D.5 already introduces libFLAC as a required packaged dependency. The smallest remaining media-adapter dependency is therefore native FLAC ingest that preserves exact FLAC S0 first and derives the same APS1 PCM16 S1 used by every existing trusted reader/exporter.

## Goal

Add a bounded Audio Profile API that ingests authorized native FLAC sources by:

1. capturing one owned source snapshot through the existing hardened capture boundary;
2. committing the exact source bytes as S0 before profile interpretation;
3. decoding supported native FLAC losslessly to exact PCM16 under a caller-supplied decoded-output bound;
4. encoding that PCM16 into the existing APS1 canonical state;
5. appending exact `state_exact` provenance from the APS1 subject to the exact FLAC S0 record;
6. finalizing a verified S0-only archive with an explicit profile error when FLAC interpretation fails after S0 commit.

APS1 remains the only canonical PCM16 S1 representation.

## Public API

Extend `include/codec/profiles/audio_ingest.hpp` with:

```cpp
namespace codec::profiles::audio {

struct Pcm16FlacIngestRequest {
  std::string source_uri;
  std::filesystem::path archive_path;
  StreamDescriptor descriptor;
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::size_t capture_chunk_bytes{256U * 1024U};
  std::uint64_t maximum_source_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_decoded_pcm_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint32_t maximum_redirects{5};
  bool deny_private_network{true};
};

struct Pcm16FlacIngestReport {
  std::filesystem::path archive_path;
  RecordInfo descriptor;
  RecordInfo source;
  std::optional<RecordInfo> state;
  std::optional<RecordInfo> provenance;
  std::optional<Error> profile_error;

  bool state_exact() const noexcept;
};

Result<Pcm16FlacIngestReport> ingest_pcm16_flac(
    const Pcm16FlacIngestRequest& request);

}
```

No root-level alias and no CLI/C ABI surface are added in D.6.

## Truth and transaction semantics

D.6 follows D.2's preservation order exactly:

- validate the request before source capture or archive creation;
- capture the source once into owned memory with `maximum_source_bytes` enforced;
- create the output CODA archive only after successful capture;
- append the caller's exact `StreamDescriptor` and exact source snapshot;
- only then attempt FLAC profile decoding;
- on profile decode/format/resource failure, finalize the archive and return a successful ingest report whose `profile_error` explains why S1 was not produced;
- on successful decode, append APS1 `pcm16`, exact `state_exact` provenance, and finalize.

Capture errors and archive I/O errors remain outer `Result` failures. Profile interpretation failures after S0 commit do not erase or invalidate preservation.

The descriptor must be `StreamType::audio` with payload type exactly `audio/flac`.

## Native FLAC decoder boundary

Add private `src/audio/flac_decoder.hpp` and `src/audio/flac_decoder.cpp` under `codec::profiles::audio::detail`.

```cpp
Result<Pcm16State> decode_flac_pcm16(
    std::span<const std::byte> source,
    std::uint64_t maximum_decoded_pcm_bytes);
```

The decoder uses the libFLAC stream decoder API entirely in memory.

Required behavior:

- require native FLAC magic `fLaC`; Ogg-FLAC and arbitrary containers are rejected;
- enable libFLAC MD5 checking;
- require STREAMINFO/decoded frames to represent signed 16-bit PCM exactly;
- preserve exact decoded sample rate, channel count, frame order, and interleaved signed samples;
- perform no resampling, remixing, dither, gain, normalization, channel-layout reinterpretation, concealment, or floating-point conversion;
- enforce `maximum_decoded_pcm_bytes` incrementally before growing the decoded sample vector;
- never trust STREAMINFO total-sample count as the sole resource bound;
- reject inconsistent frame geometry or sample values outside signed 16-bit range;
- map malformed/unsupported FLAC to `ErrorCode::decode`;
- map decoded PCM budget exhaustion or allocation exhaustion to `ErrorCode::resource_exhausted`.

The decoder may reserve from a trustworthy, in-bound STREAMINFO total-sample estimate but must still enforce the incremental write bound.

## Successful canonicalization

On decoder success, the returned `Pcm16State` is encoded with the existing `encode_pcm16_state()` implementation. D.6 does not define another PCM state format.

The state record is appended as `RecordType::pcm16` on the same logical stream and exact authenticated interval as S0.

The provenance process is:

```text
truth: state_exact
operation: audio.flac.decode-pcm16
implementation_id: codec-audio-profile
implementation_version: 1
created_utc_ns: request.end_ns
input: exactly the committed FLAC source_bytes record
subject: exactly the committed APS1 pcm16 record
```

D.3 remains the downstream authority that validates this lineage before any trusted export or later processing.

## Bounds and validation

Before capture, reject:

- empty source URI;
- empty/invalid archive output path;
- inverted interval;
- capture chunk outside the same D.2 supported range;
- zero or process-unrepresentable `maximum_source_bytes`;
- zero or process-unrepresentable `maximum_decoded_pcm_bytes`;
- redirect count above the same D.2 limit;
- descriptor type other than audio;
- descriptor payload type other than `audio/flac`;
- invalid encoded stream descriptor;
- an archive output path that already exists.

Decoded PCM accounting is exact payload accounting: two bytes per signed PCM16 sample. Arithmetic must be overflow-safe before multiplication, vector reserve, or append.

## Tests

Create `tests/test_audio_flac_ingest.cpp`.

Required proofs:

1. **Native FLAC happy path**
   - build a deterministic PCM16 state fixture;
   - encode it with the actual D.5 native FLAC exporter/encoder path or a test-local libFLAC fixture;
   - ingest through actual D.6;
   - verify finalized archive;
   - prove exact S0 bytes equal the input FLAC snapshot;
   - query through actual D.3 and prove exact sample rate/channels/samples;
   - prove exact `state_exact` same-stream/same-interval provenance.

2. **Round-trip integration**
   - D.6 ingest -> D.3 verified state -> D.5 FLAC export;
   - independently decode the D.5 output and prove exact PCM equality;
   - do not require byte equality between input and exported FLAC.

3. **Preservation-first failure isolation**
   - malformed native-FLAC-like bytes are preserved exactly as S0;
   - unsupported 24-bit FLAC is preserved exactly as S0;
   - Ogg-FLAC/non-native input is preserved exactly as S0;
   - each case finalizes/verify succeeds with no APS1/provenance and a profile error.

4. **Decoded output bound**
   - a valid compressed FLAC whose PCM exceeds `maximum_decoded_pcm_bytes` finalizes as S0-only with `resource_exhausted` profile error;
   - zero decoded bound fails before capture/archive creation.

5. **Request and filesystem validation**
   - wrong payload type/type, inverted interval, invalid chunk/redirect/source bound, and existing archive path fail before replacement.

6. **Compatibility**
   - existing D.2 WAV ingest behavior remains unchanged;
   - D.3/D.4/D.5 tests remain green;
   - C ABI, CLI, archive, watermark, inference-unavailable, and package-consumer tests remain green.

7. **Toolchain**
   - GCC and Clang warnings-as-errors build/test/install/package-consumer pass;
   - sanitizer build/test passes.

## Documentation

README and CHANGELOG must state that native PCM16 FLAC is now a preservation-first Audio Profile ingest adapter: exact FLAC remains S0, APS1 is the canonical decoded S1, and unsupported/malformed FLAC still preserves S0.

Do not describe arbitrary media conversion, Ogg-FLAC, 24-bit canonical state, or source-FLAC byte normalization as implemented.

## Non-claims

D.6 does not add or claim:

- CODA header/envelope/version/record-type changes;
- another S1 format or truth class;
- source-exact FLAC rewriting;
- byte identity between source FLAC and re-exported FLAC;
- Ogg-FLAC;
- 24-bit/32-bit/float PCM S1;
- FFmpeg or general media conversion;
- resampling, remixing, dither, gain, enhancement, concealment, or channel-layout semantics;
- CLI or C ABI FLAC ingest/export;
- archive/filesystem/network persistence beyond the existing ingest archive transaction;
- neural runtime/model bundles;
- streaming or offline inference;
- diarization, embeddings, or identity fusion;
- recovery/FEC;
- performance, scale, or deployment guarantees;
- frozen CODA v1;
- Stage D completion.

## Stage D continuation

After D.6, Stage D remains active. The next milestone should select the smallest remaining dependency from broader production media adapters, identity fusion, compatible neural runtime/model bundles, streaming inference, offline re-inference, and broader audio-specific validation. The trusted processing/export side must continue to consume D.3 rather than inventing a parallel S1-selection path.
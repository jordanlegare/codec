# Stage D.1 Deterministic Audio PCM16 S1 Design

## Purpose

Define and prove the first deterministic canonical-state contract of the Audio
Stream Profile: decoded interleaved integer PCM16 audio can become a
self-contained, reproducible S1 payload without conflating its original WAV
container bytes with canonical audio state.

This milestone establishes the exact normalized state that later Audio Profile
adapters, exporters, identity fusion, and inference can consume. It adds no
neural runtime, model, resampling, enhancement, or streaming behavior.

## Current State

CODEC currently provides:

- exact S0 preservation and generic record extraction;
- generic S1/D provenance sidecars with exact physical links;
- an existing `RecordType::pcm16` development-profile tag;
- exact PCM16 RIFF/WAVE read/write through `WavPcm16`;
- an additive `codec::profiles::audio` public facade; and
- passing root-level C++/C ABI/CLI compatibility tests.

No Audio Profile type defines canonical PCM state independently of RIFF/WAVE,
and no self-contained payload encoding defines the rate, channel count, sample
order, or version carried by a `pcm16` record. The record tag alone therefore
cannot justify an S1 claim.

## Truth Boundary

- Original accepted WAV/RIFF bytes remain S0.
- A successfully validated `Pcm16State` is the Audio Profile's deterministic
  canonical state for interleaved signed PCM16 at one sample rate and channel
  count.
- The canonicalization step does not resample, remix, dither, enhance, infer,
  watermark, or otherwise alter sample values or order.
- A stored `pcm16` record is classified as S1 only by a valid generic
  `stream_provenance` sidecar whose `subject_truth` is `state_exact` and whose
  direct inputs link to the exact preserved source record or records.
- A `pcm16` tag without that provenance remains an unclassified physical
  record; CODEC does not infer truth class from record type.
- D remains unchanged and no D artifact is produced.

## Public Audio Profile API

Add to `include/codec/audio.hpp` in the compatibility root namespace:

```cpp
struct Pcm16State {
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  std::vector<std::int16_t> samples;

  std::size_t frames() const noexcept;
  double duration_seconds() const noexcept;
};

Result<Pcm16State> canonicalize_pcm16(const WavPcm16& source);
Result<std::vector<std::byte>> encode_pcm16_state(const Pcm16State& state);
Result<Pcm16State> decode_pcm16_state(
    std::span<const std::byte> payload);
```

Add exact using-declarations for the type and functions to
`include/codec/profiles/audio.hpp`. Existing root symbols remain ABI-bearing and
source compatible; the Audio Profile facade is the canonical new entry point.

`Pcm16State` is intentionally distinct from `WavPcm16`. `WavPcm16` represents
decoded data associated with a particular supported input/output container;
`Pcm16State` represents the container-independent canonical sample state.

## Canonicalization

`canonicalize_pcm16()` accepts a `WavPcm16` value only when:

- `sample_rate` is non-zero;
- `channels` is non-zero; and
- `samples.size()` is an exact multiple of `channels`.

It returns the identical sample rate, channel count, and interleaved sample
sequence. An empty sequence is valid when rate and channel count are valid.

Because the accepted source object is already signed integer PCM16, no numeric
conversion is required. Sample values `-32768` through `32767` are copied
exactly.

## Versioned APS1 Payload

`encode_pcm16_state()` produces one deterministic little-endian byte sequence:

| Offset | Size | Field | Required value |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `APS1` |
| 4 | 2 | payload version | `1` |
| 6 | 2 | sample format | `1` = signed little-endian PCM16 |
| 8 | 4 | sample rate | non-zero Hz |
| 12 | 2 | channel count | non-zero |
| 14 | 2 | reserved flags | `0` |
| 16 | 8 | interleaved sample count | unsigned count |
| 24 | 2 × count | samples | signed PCM16 bit patterns, little-endian |

The sample count, not frame count, is stored. Complete frames are proven by
`sample_count % channels == 0`. Channel slots and interleaving order are part of
the exact state; this milestone does not assign speaker/layout semantics to
those slots.

All fields are written explicitly byte by byte. Encoding never copies native
integer object representation and is deterministic across host endianness.

## Decode and Malformed Input

`decode_pcm16_state()` accepts only exact APS1 version 1 payloads. It rejects:

- fewer than 24 bytes;
- wrong magic;
- unknown version or sample format;
- non-zero reserved flags;
- zero sample rate or channel count;
- a sample count that does not exactly match the remaining payload bytes;
- incomplete channel frames; and
- any size relation that cannot be represented safely.

Malformed profile payloads return `ErrorCode::decode`. Invalid in-memory states
passed to canonicalize/encode return `ErrorCode::invalid_argument`; an encoding
that cannot fit the process address space returns `resource_exhausted`.

Decode allocates only after all header, length, and frame checks pass.

## CODA Storage Contract

No generic archive API is specialized for audio. Callers use the stable generic
surface:

1. preserve the accepted source as `RecordType::source_bytes` (S0);
2. canonicalize and encode `Pcm16State`;
3. append the APS1 payload as `RecordType::pcm16` under the logical stream and
   authenticated interval;
4. append `stream_provenance` for that subject with
   `TruthClass::state_exact`, the exact S0 source record(s), and a bounded
   `ProvenanceProcess`; and
5. finalize normally.

Readback uses `RecordQuery`/`extract_records()` with the existing `pcm16` type,
then `decode_pcm16_state()`. Provenance is selected through the existing direct
S1 query boundary.

This is a versioned Audio Profile payload inside the existing provisional CODA
development envelope. It adds no record type and changes no envelope, commit
trailer, final index, chain, or C ABI layout.

## Failure Isolation

Canonicalization and encoding occur before archive append in the demonstrated
flow. Invalid audio state therefore cannot create or mutate an archive.

The generic writer remains append-only and non-transactional. If a caller
appends a valid PCM record and later fails to append provenance, the record is
not silently S1; it remains a physical record with no state-exact declaration.
This milestone adds no transaction claim.

Profile decode failure cannot corrupt or block access to committed S0. Callers
can still extract the source record independently.

## Compatibility

The change is additive and preserves:

- all existing `codec::*` audio types, functions, and ABI-bearing symbols;
- the `codec::profiles::audio` facade identity for existing types/functions;
- existing WAV read/write, watermark, statement, and separation behavior;
- generic writer, reader, query, extraction, repair, and provenance signatures;
- legacy `FeedSpec`, feed descriptors, `--feed` extraction, generic stream CLI,
  C ABI, and capability JSON; and
- current unavailable neural/GPU capability states.

## Proof Contract

Tests must establish:

1. canonicalization copies rate, channels, frame/sample order, negative values,
   and integer extremes exactly;
2. a hand-derived APS1 fixture equals encoded bytes byte for byte;
3. decode reproduces the exact state and re-encoding is identical;
4. a stored APS1 `pcm16` record reopens, extracts, and decodes exactly;
5. the state-exact provenance sidecar links the exact PCM subject and S0 input
   stream/type/sequence/hash;
6. committed S0 remains independently extractable;
7. zero rate/channels and incomplete frames fail before archive mutation;
8. every malformed APS1 header/length/frame case fails deterministically;
9. mutation of sample endianness or omission of the S1 provenance sidecar is
   caught by the exact fixture/integration proof;
10. the installed Audio Profile facade exposes the new type/functions and an
    external consumer completes canonicalize/store/provenance/readback; and
11. Release, C ABI, CLI, AI-contract, audio/watermark/inference, and sanitizer
    suites remain green with unchanged capability output.

## Documentation Claim

The README and changelog may claim that the Audio Stream Profile implements a
deterministic, self-contained, versioned PCM16 S1 state payload with exact
sample round-trip and explicit S0-linked state-exact provenance.

They must continue to state that FLAC, production media adapters, model/runtime
bundles, streaming inference, identity fusion, and Audio Profile 1.0 completion
remain unimplemented.

## Explicit Non-Claims

This milestone does not add:

- FLAC or another compressed/lossless codec;
- float PCM, sample-width conversion, resampling, remixing, dithering, channel
  layout interpretation, enhancement, concealment, or reconstruction;
- automatic WAV ingest, canonicalization, archive persistence, or transactions;
- a new CODA record type or generic envelope/version change;
- CLI or C ABI access to the new state codec;
- a model/runtime bundle, neural availability, streaming/offline inference,
  diarization, embeddings, or identity fusion;
- recovery/FEC, performance, scale, deployment, or frozen CODA v1 guarantees;
  or
- completion of Stage D or Audio Stream Profile 1.0.

The next dependency after D.1 should build on this exact state rather than
inventing a parallel audio normalization target.

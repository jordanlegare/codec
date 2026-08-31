# H.1 Verified Audio for Video Ingest and MP4 Export

Date: 2026-08-30

## Status

Design for extending the existing H.1 Video Profile so video ingest preserves and canonicalizes an accompanying audio track and verified MP4 export muxes that audio instead of silently producing muted video.

This design keeps the existing preservation-first truth model:

- S0 is the exact captured source material.
- S1 is a deterministic, versioned canonical state with explicit provenance back to S0.
- MP4 export is a derived representation and never mutates the CODA archive or re-fetches the source.

## Problem

Current H.1 ingest selects and canonicalizes only `AVMEDIA_TYPE_VIDEO`. The preserved S0 source can contain audio, but no verified canonical audio state is written. The current MP4 exporter consumes only verified VFR1 video states and therefore emits a video-only MP4.

The required behavior is:

1. If the captured media has no audio stream, H.1 continues to behave exactly as a video-only profile.
2. If the captured media has an audio stream, ingest must attempt to canonicalize that audio into CODEC's existing `Pcm16State` representation.
3. A known audio stream must not be silently dropped. If audio canonicalization fails, the archive remains preservation-valid and retains any usable video S1 states, but the ingest report carries a profile error and `state_exact()` is false.
4. Verified MP4 export must mux verified video and verified audio when the H.1 audio state is present.
5. Old H.1 archives that predate this feature and contain no H.1 audio state remain exportable as video-only MP4s. Export does not re-demux old S0 solely to infer missing historical audio.

## Approaches Considered

### A. Re-demux or copy the original encoded audio during export

Rejected. This would mix verified S1 video with an audio path whose truth semantics differ, and HLS export would require reconstructing demux behavior from captured child resources. It would make export semantics dependent on S0 interpretation rather than verified canonical state.

### B. Create a second `StreamType::audio` stream for every camera

Rejected for this version. It would require a durable association between camera video and derived audio stream IDs, changes to grouped ingest/merge and CLI enumeration semantics, and a new cross-stream pairing contract for export.

### C. Store a Video-Profile audio state on the same camera stream ID using the existing `Pcm16State` payload encoding

Selected. The camera remains one logical stream. A new Video-Profile record type identifies the payload as H.1 audio while `encode_pcm16_state()` and `decode_pcm16_state()` provide the canonical representation. This avoids weakening or overloading the standalone Audio Profile's `RecordType::pcm16` contract.

## Public Data Model

Add a Video-Profile custom record type:

```cpp
inline constexpr RecordTypeCode video_pcm16_audio_state_record_type = 0x0102;
```

The payload is exactly the existing `codec::Pcm16State` encoding produced by `encode_pcm16_state()`.

H.1 v1 supports one canonical audio state per video stream. The state contains one contiguous interleaved PCM16 timeline with:

- stable sample rate for the complete state;
- stable channel count for the complete state;
- mono or stereo channels only in this first version;
- finite, bounded sample count;
- no discontinuities or timestamp regressions.

Multichannel audio, sample-rate changes, channel-count changes, or timeline discontinuities produce a profile-level audio canonicalization error. Exact S0 remains preserved.

Add a verified reader result equivalent in shape to verified video frames:

```cpp
struct VerifiedVideoPcm16Audio {
  Pcm16State state;
  RecordInfo state_record;
  std::vector<RecordInfo> source_records;
  StreamProvenance provenance;
};

struct VideoAudioQuery {
  std::optional<StreamId> stream;
  std::optional<RecordTimeRange> time;
  std::size_t maximum_results{1024};
  std::uint64_t maximum_encoded_bytes{1024ULL * 1024ULL * 1024ULL};
};

Result<std::vector<VerifiedVideoPcm16Audio>>
query_verified_video_pcm16_audio(
    const CodaArchive& archive,
    const VideoAudioQuery& query = {});
```

The reader is part of the Video Profile. The existing standalone `query_verified_pcm16_states()` contract is unchanged.

## Ingest Request and Report

Extend `FfmpegVideoIngestRequest` with an independent audio bound:

```cpp
std::uint64_t maximum_decoded_audio_bytes{1024ULL * 1024ULL * 1024ULL};
```

Expose it in the CLI as:

```text
--maximum-decoded-audio-bytes N
```

Extend `FfmpegVideoIngestReport` with:

```cpp
bool audio_present{};
std::optional<RecordInfo> audio_state;
std::optional<RecordInfo> audio_provenance;
```

`state_exact()` remains the overall H.1 exact-state predicate:

- at least one verified video state;
- video state/provenance counts match;
- if no source audio is present, no audio state is required;
- if source audio is present, exactly one H.1 audio state and one matching provenance record are required;
- no profile error is present.

An audio failure after successful video canonicalization does not discard the video S1 records. It finalizes the archive with those video states plus S0 and reports a profile error.

CLI JSON gains additive fields such as:

```json
{"audio_present":true,"audio_state_exact":true}
```

for both single and grouped H.1 ingest reports.

## FFmpeg Decode Architecture

Direct media and HLS each use one demux pass with independent video and audio decoder contexts.

After `avformat_find_stream_info()`:

1. Select the required best video stream exactly as today.
2. Determine whether any audio stream exists.
3. If no audio stream exists, continue video-only.
4. If audio exists, select the best decodable audio stream. Decoder absence or audio metadata failure becomes an audio profile error, not silent absence.
5. Route packets to the selected video and audio decoders during the same `av_read_frame()` loop.
6. A video decode error remains fatal to H.1 canonicalization as today.
7. An audio-only decoder/conversion error disables further audio canonicalization but allows video decoding to finish where demux itself remains usable.

HLS continues to use the existing memory-only primary manifest and same-origin captured secondary-resource callback. Video and audio packets therefore consume the same preservation-controlled FFmpeg session; no second network traversal is introduced.

## PCM16 Canonicalization

Add `libswresample` to the optional FFmpeg Video Profile dependency set.

Each decoded `AVFrame` from the audio decoder is converted with `SwrContext` to interleaved signed 16-bit PCM while preserving the selected source sample rate and channel count.

The first version accepts mono and stereo. It does not downmix multichannel audio because `Pcm16State` currently carries channel count but no explicit channel-layout metadata.

The canonicalizer appends converted samples into one bounded `Pcm16State`. It verifies:

- sample rate is positive and stable;
- channel count is 1 or 2 and stable;
- converted byte count stays within `maximum_decoded_audio_bytes`;
- decoded audio timestamps are monotonic;
- successive audio frames form one contiguous sample timeline within a one-sample rounding tolerance.

The existing video timeline remains anchored exactly as today: the first verified video frame maps to `request.start_ns`.

Audio timing is mapped relative to that same first-video timestamp. This preserves existing H.1 video timestamps while retaining the source A/V offset. Audio samples before the first video timestamp are deterministically trimmed. Audio starting later than video retains that positive offset. Audio after `request.end_ns` is trimmed.

If the first audio timestamp cannot be related to the selected video timeline, audio canonicalization fails rather than inventing synchronization.

The single H.1 audio state record uses:

- `stream = request.descriptor.id`;
- `type = video_pcm16_audio_state_record_type`;
- `start_ns = absolute timestamp of the first retained PCM sample`;
- `end_ns = start_ns + canonical PCM sample duration`, bounded to the requested interval.

## Provenance

The audio state receives `TruthClass::state_exact`, using its own explicit Video-Profile process identity.

Direct-media operation:

```text
codec.video.pcm16.canonicalize
```

with:

```text
implementation_id      = codec.video
implementation_version = 1
details_type            = application/vnd.codec.video.audio-canonicalization.v1
details                 = 0x01
```

Direct inputs are exactly:

```text
[primary S0 source_bytes]
```

HLS operation:

```text
codec.video.pcm16.canonicalize.hls
```

with:

```text
implementation_id      = codec.video
implementation_version = 1
details_type            = application/vnd.codec.video.hls-audio-canonicalization.v1
details                 = 0x01
```

HLS inputs are:

```text
[primary manifest S0] + [ordered secondary S0 frontier used by the demux session]
```

Because v1 emits one aggregate PCM state, its HLS provenance uses the final ordered secondary-resource frontier observed by the successful audiovisual demux/decode session. Every referenced secondary record must retain the existing `codec.video.hls-resource` opaque descriptor contract.

## Verified H.1 Audio Reader

`query_verified_video_pcm16_audio()` verifies before returning any decoded PCM:

- archive verification succeeds and the archive is finalized;
- subject record type is exactly `0x0102`;
- subject stream has a unique declared `StreamType::video` descriptor;
- at most one H.1 audio state exists for a stream in v1;
- subject provenance is `TruthClass::state_exact`;
- process identity matches exactly one of the direct/HLS audio operations above;
- direct provenance contains exactly the primary same-stream S0 source;
- HLS provenance contains primary same-stream S0 first plus one or more unique HLS child S0 sources;
- each HLS child has exactly one opaque descriptor with `source_id = "codec.video.hls-resource"`;
- provenance is not self-referential and all links resolve exactly by stream/type/sequence/hash;
- the PCM payload decodes with `decode_pcm16_state()`;
- sample rate/channels/sample count satisfy H.1 audio limits;
- record interval is consistent with the PCM sample duration within the defined nanosecond rounding tolerance.

Malformed purported H.1 audio is `archive_corrupt`, not silently ignored.

## MP4 Export

`export_verified_video_mp4()` continues to query only verified H.1 state.

For the selected video stream it:

1. queries verified VFR1 video frames as today;
2. queries verified H.1 PCM16 audio for the same stream;
3. if no H.1 audio state exists, emits the current video-only MP4;
4. if one verified H.1 audio state exists, creates an AAC audio track and muxes it with the video track;
5. if purported H.1 audio exists but fails verification, export fails rather than stripping sound.

The video encoding path remains MPEG-4 Part 2/YUV420P. The new audio export path uses FFmpeg's AAC encoder.

PCM16 is converted to an encoder-supported audio sample format using `libswresample`. Export may resample for AAC encoder compatibility because MP4 is a derived representation; CODA's verified S1 PCM16 remains unchanged.

A/V PTS use one common export origin equal to the first verified video state start. The H.1 ingest contract guarantees retained audio does not begin before that origin. Audio's positive start offset is preserved in the MP4 audio timestamps.

If verified audio is present but the AAC encoder or required conversion path is unavailable, export returns `model_incompatible`; it must not fall back to a muted MP4.

Extend `VerifiedVideoMp4Export` with optional audio evidence so callers can distinguish audiovisual from video-only export, for example:

```cpp
std::optional<RecordInfo> audio_state_record;
std::optional<StreamProvenance> audio_provenance;
```

CLI export JSON gains an additive boolean:

```json
{"audio":true}
```

Bulk `--all` preserves its current partial-success behavior: an audio/export failure for one stream reports `status:"error"` for that stream while verified peers are still written.

## Grouped Concurrent Ingest

No new shared-writer concurrency is introduced.

Each concurrent `--video` worker still writes an independent staged CODA. The existing deterministic merge already re-appends unknown/custom payload records and rewrites stream provenance links into destination sequence space, so the new `0x0102` PCM payload is compatible with that model.

Regression tests must explicitly prove that an audiovisual staged stream survives merge with both verified VFR1 and verified H.1 PCM provenance intact.

## Limits and Safety

- `maximum_decoded_audio_bytes` must be nonzero and process-size representable.
- PCM sample allocation must check multiplication/accumulation overflow.
- Audio canonicalization is limited to mono/stereo in v1.
- No source re-fetch during export.
- No cross-origin HLS relaxation.
- No encrypted HLS relaxation.
- No change to generic CODA preservation or unknown-type handling.
- No mutation of an existing CODA during export.
- No fallback from failed verified audio to silent output when an H.1 audio state is present or source audio was detected during new ingest.

## TDD and Verification

Implementation proceeds TDD-first.

Required RED/GREEN coverage:

1. **PCM reuse / direct ingest**
   - audiovisual MP4 fixture ingests one verified H.1 PCM state;
   - payload decodes via existing `decode_pcm16_state()`;
   - no-audio MP4 remains exact video-only;
   - audio-present decode/conversion failure is reported rather than silently ignored.

2. **HLS audiovisual ingest**
   - HLS with audio produces one verified H.1 PCM state;
   - audio provenance contains primary plus ordered captured secondary frontier;
   - cross-origin/private/encrypted HLS policies remain unchanged;
   - no second network capture path is introduced.

3. **Verified reader corruption tests**
   - wrong process operation/details rejected;
   - missing/duplicate/invalid source links rejected;
   - invalid HLS child descriptor rejected;
   - malformed PCM payload rejected;
   - interval/sample-duration mismatch rejected;
   - multiple H.1 audio states for one stream rejected in v1.

4. **MP4 export**
   - audiovisual archive exports an MP4 with one video and one audio stream;
   - decoded exported audio is nonempty;
   - positive audio start offset is preserved;
   - old/no-audio archive remains video-only;
   - verified audio plus unavailable AAC backend fails instead of muting;
   - bulk export keeps peer partial success.

5. **Grouped ingest**
   - two audiovisual cameras ingested concurrently into one CODA retain independently verified video and audio;
   - both bulk-export with sound;
   - one audio profile failure does not discard another successful camera.

6. **Build matrix**
   - GCC build/tests/install/package consumer;
   - Clang build/tests/install/package consumer;
   - ASan/UBSan/LSan build/tests;
   - FFmpeg-disabled build/tests/install/package consumer;
   - exact-head CI evidence before any completion claim.

## Files Expected to Change

Likely implementation surface:

- `CMakeLists.txt` — add `libswresample` to the optional FFmpeg backend and new tests/source if split.
- `include/codec/profiles/video.hpp` — H.1 audio record/query/report/request API.
- `include/codec/profiles/video_export.hpp` — optional verified audio evidence.
- `src/video/ffmpeg_ingest.cpp` — direct audiovisual demux/decode and PCM16 canonicalization.
- `src/video/ffmpeg_ingest_hls.cpp` — HLS audiovisual demux/decode and audio frontier.
- `src/video/frame_state_reader.cpp` or a focused new `src/video/audio_state_reader.cpp` — strict verified H.1 audio reader. Prefer the focused new file.
- `src/video/ffmpeg_export.cpp` — verified PCM16 to AAC plus A/V mux.
- `src/cli/main.cpp` — audio resource limit/help/export JSON.
- `src/cli/video_multi_ingest.hpp` — grouped option parsing/report fields if required.
- video unit/integration/package-consumer tests and an audiovisual fixture.

No generic Audio Profile reader semantics should be weakened or broadened for this feature.

## Non-Goals

This version does not add:

- multichannel audio canonicalization;
- arbitrary audio-only ingest through `codec video ingest`;
- source audio passthrough/remux as a truth shortcut;
- multiple audio tracks/languages per camera;
- subtitle/data-track preservation as S1;
- audio mixing, loudness normalization, denoising, or resampling in S1;
- a generic cross-stream audiovisual association format;
- changes to H.2 telemetry.

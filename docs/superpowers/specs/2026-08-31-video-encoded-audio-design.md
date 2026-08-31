# H.1 Preservation of Original Encoded Audio

Date: 2026-08-31

## Status

Approved design for replacing new H.1 PCM16 audio-state writes with a
versioned, provenance-verified bundle of the selected source audio stream's
unchanged compressed packets. The existing H.1 `0x0102` PCM16 contract is
frozen as a compatibility tombstone: it remains readable and exportable, but
new audiovisual ingest does not write it.

This is a Video Profile change only. Generic CODA, the standalone Audio
Profile, HLS authorization, video-frame S1, grouped ingest, and the CLI command
surface remain unchanged.

## Problem and measured cost

Current H.1 audiovisual ingest stores exact source/container or HLS-resource
bytes as S0 and then stores the same audio content again as interleaved PCM16
under profile-local record type `0x0102`. Ingest decodes and resamples the full
track into one aggregate sample vector. MP4 export later resamples and AAC
encodes that vector.

At 48 kHz stereo, PCM16 consumes 192,000 bytes/second or 691.2 MB/hour. A
128-kbit/s compressed stream consumes 57.6 MB/hour before the bounded packet
table, approximately twelve times less than the PCM payload. The exact ratio
depends on the source codec, bitrate, channels, and sample rate.

The accepted source bytes already contain the compressed track. The new state
intentionally duplicates only the selected compressed packets because doing so
gives a stable verified extraction/export boundary without re-demuxing S0,
reconstructing HLS resource traversal, or depending on a future FFmpeg version
to reproduce the same packetization.

## Compatibility boundary

- `video_pcm16_audio_state_record_type` (`0x0102`) stays public.
- `query_verified_video_pcm16_audio()` and the PCM16-to-AAC export path stay
  intact for old archives and tests.
- New ingest never emits `0x0102`.
- New ingest emits `video_encoded_audio_state_record_type` (`0x0103`).
- An archive that purports to contain both verified H.1 audio-state forms for
  one video stream is contradictory and export fails closed.
- Existing report fields (`audio_present`, `audio_state`,
  `audio_provenance`) remain generic evidence handles and require no CLI JSON
  change.
- `maximum_decoded_audio_bytes` and its existing CLI spelling remain accepted
  for source/API compatibility. For the new path this value bounds the retained
  EAP1 fixed header, packet table, packet payloads, and decoder configuration;
  packet count also has an independent one-million-packet ceiling. The legacy
  name is documented; a separate CLI migration is outside this change.

## EAP1 state model

The profile adds the following media-library-independent public model:

```cpp
inline constexpr RecordTypeCode video_encoded_audio_state_record_type = 0x0103;

enum class EncodedAudioCodec : std::uint16_t {
  aac = 1,
};

struct EncodedAudioPacket {
  std::int64_t pts_offset_ns{};
  std::int64_t dts_offset_ns{};
  std::uint64_t duration_ns{};
  std::uint32_t flags{};
  std::vector<std::byte> payload;
};

struct EncodedAudioState {
  EncodedAudioCodec codec{EncodedAudioCodec::aac};
  std::int32_t codec_profile{};
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  std::uint64_t decoded_frames{};
  std::uint64_t trim_start_frames{};
  std::uint64_t presentation_frames{};
  std::vector<std::byte> decoder_config;
  std::vector<EncodedAudioPacket> packets;
};
```

EAP1 v1 supports one AAC audio track with stable mono or stereo layout and
sample rate. Adding another codec requires a new profile enum value and tests;
FFmpeg codec identifiers are not persisted as the portable contract.

`pts_offset_ns` and `dts_offset_ns` are signed offsets from the audio state
record's logical `start_ns`. Packet payload bytes are copied exactly from the
selected demuxed `AVPacket`. The configuration bytes are copied from the
selected stream's codec extradata. Packet flags retain only the explicitly
supported v1 mask; unknown flag bits fail encoding/decoding.

The big-endian EAP1 encoding contains a fixed header, decoder configuration,
then an ordered sequence of fixed packet descriptors and length-delimited
packet payloads. It includes explicit counts and aggregate sizes. Decoding is
bounded by nonzero limits for packet count, configuration bytes, individual
packet bytes, and aggregate payload bytes. Reserved fields must be zero and
trailing bytes are rejected.

The record interval is the logical presentation window. Its duration must
match `presentation_frames / sample_rate` within one nanosecond of integer
rounding. `trim_start_frames + presentation_frames` must not exceed
`decoded_frames`. Packet PTS/DTS arithmetic must be representable, ordered, and
support the complete logical window without gaps; wholly out-of-window packets
are invalid.

EAP1 v1 does not persist FFmpeg packet side data. Skip/discard metadata on a
packet wholly before the logical window may be discarded with that packet.
Retained skip/discard metadata, configuration-changing side data, or any other
unsupported side data produces an explicit profile incompatibility rather than
silently changing the presentation.

## Ingest architecture

Direct media and HLS retain their single preservation-controlled FFmpeg demux
session.

1. Select the best audio stream and require a supported AAC decoder.
2. Copy codec parameters/extradata and each selected compressed packet into a
   bounded packet bundle before submitting it to the decoder.
3. Decode packets only to validate stable sample rate/channel count, usable
   timestamps, monotonic contiguous decoded sample time, and synchronization
   with the first video timestamp.
4. Count decoded frames and discard decoded `AVFrame` data immediately. Do not
   allocate a `SwrContext`, convert to PCM16, or retain decoded samples.
5. Apply the existing logical video-origin/start/end calculation as
   `trim_start_frames` plus `presentation_frames` metadata.
6. Retain packets that support that logical window, normalize packet timing to
   nanosecond offsets from the state record, encode EAP1, and write `0x0103`.

A source audio stream that cannot satisfy this contract remains a profile
error. Exact S0 and successfully produced video S1 are retained, but
`state_exact()` is false. There is no silent mute fallback.

The existing HLS capture callback, same-origin checks, child S0 records, final
ordered resource frontier, and direct-video path remain unchanged.

## Provenance and verified reader

New direct provenance uses:

```text
operation              = codec.video.encoded-audio.preserve
implementation_id      = codec.video
implementation_version = 1
details_type            = application/vnd.codec.video.encoded-audio.v1
details                 = 0x01
```

New HLS provenance uses:

```text
operation              = codec.video.encoded-audio.preserve.hls
implementation_id      = codec.video
implementation_version = 1
details_type            = application/vnd.codec.video.hls-encoded-audio.v1
details                 = 0x01
```

Direct input is exactly the primary same-stream S0 record. HLS inputs are the
primary same-stream S0 record followed by the final unique ordered HLS child
S0 frontier. The child descriptor contract stays
`source_id = "codec.video.hls-resource"`.

`query_verified_video_encoded_audio()` fails closed on malformed EAP1,
contradictory/missing provenance, non-video descriptors, duplicate states,
invalid HLS frontiers, caller bounds, invalid packet ordering/timing, or an
interval that does not match the presentation frame count. It returns no
decoded PCM.

## MP4 export

`export_verified_video_mp4()` checks H.1 encoded audio before the legacy PCM16
form:

1. No audio state: retain video-only behavior.
2. One verified `0x0103` state: create an MP4 audio stream from the verified
   codec/configuration and copy packet payload bytes without decoding or AAC
   encoding.
3. One verified legacy `0x0102` state: retain the existing PCM16-to-AAC path.
4. Both forms, invalid provenance, unsupported codec/configuration, or a
   leading trim that cannot be represented without changing compressed audio:
   fail explicitly instead of muting.

For packet passthrough, packet PTS/DTS are offset by the verified audio record's
position relative to the first verified video frame and rescaled into the MP4
audio time base. A final packet duration may be shortened to the logical
presentation end without changing its compressed payload. Leading
sample-accurate trim is not fabricated: v1 reports explicit incompatibility
when the container path cannot represent it without transcoding.

The export result adds an evidence boolean indicating encoded-packet
passthrough. The CLI command and required arguments do not change.

## Storage and runtime proof

Tests must establish behavior rather than claim universal benchmarks:

- new ingest writes `0x0103` and writes no `0x0102`;
- EAP1 packet/configuration bytes round-trip exactly;
- forged counts and aggregate sizes cannot reserve or copy beyond caller
  limits, and fixed header/table bytes count against ingest bounds;
- the fixture EAP1 record is smaller than its validated PCM16-equivalent byte
  count;
- decoded audio validation does not create a resampler or aggregate sample
  vector;
- compatible MP4 export reports packet passthrough and the output decodes with
  one nonempty audio stream;
- old `0x0102` archives retain the AAC-transcode export path;
- direct, HLS, grouped-ingest, CLI, installed-consumer, FFmpeg-disabled,
  GCC/Clang, and sanitizer gates remain green.

No general throughput, latency, codec coverage, or compression-ratio claim is
made without a named workload and measured evidence.

## Non-goals

- Rewriting or stripping audio bytes from exact S0.
- Changing standalone WAV/FLAC PCM16 or separation/model contracts.
- Adding audio-only CLI commands or changing existing video command syntax.
- Supporting multiple audio tracks, multichannel audio, encrypted/cross-origin
  HLS, source re-fetch, or native FFmpeg network/file authority.
- Treating raw concatenated AAC packets as a universally playable file.
- Claiming exact sample trimming where the original compressed/container form
  cannot represent it without transcoding.

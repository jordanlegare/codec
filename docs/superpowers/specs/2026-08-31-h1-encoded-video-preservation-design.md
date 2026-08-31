# H.1 Encoded Video Preservation Design

Date: 2026-08-31  
Repository: `jordanlegare/codec`  
Status: Approved  
Base: `main` at `2900742a52c0dd8c6dea1277e767a8592db1d840`

## Decision

New Stage H.1 audiovisual ingest stops persisting decoded video pixels as the primary S1 representation. CODEC continues to preserve accepted source/container/HLS-resource bytes as exact S0, but the selected H.264 video stream is additionally persisted as a bounded versioned encoded-packet S1 state. Decoding remains a fail-closed validation step; decoded frames are immediately discarded.

Legacy `VFR1` (`0x0101`) remains readable and exportable. New ingest writes `EVP1` (`0x0104`) instead. Existing encoded AAC `EAP1` (`0x0103`) remains the audio state and gains export-time bitstream normalization when container-global AAC configuration is absent.

## Goals

1. Eliminate new per-frame raw pixel persistence from FFmpeg direct and HLS video ingest.
2. Preserve selected H.264 packet payloads byte-for-byte together with exact packet timing/flags and codec configuration metadata.
3. Keep decoder validation without accumulating or persisting decoded pixel buffers.
4. Export compatible H.264 + AAC to MP4 by packet remux/bitstream filtering only, with no H.264 or AAC encoder use.
5. Retain legacy `VFR1` raw-frame and `0x0102` PCM16 readers/export paths.
6. Keep all codec/container semantics inside `codec::profiles::video`; generic CODA structures and record envelopes remain unchanged.

## Non-goals

- Recompressing H.264 at ingest or export.
- Changing exact S0 capture/extract semantics.
- Removing `RawVideoFrameState`, `VFR1`, or its verified reader.
- Supporting every video codec in EVP1 v1. EVP1 v1 supports H.264/AVC only.
- Claiming byte-identical MP4 container output; the exact preserved compressed packet payload is the state claim, while MP4 is a remuxed derived export.
- Adding HEVC/AV1/VP9 in this change.
- Moving profile fields into generic core records.

## Profile record type

The Video Profile adds:

```cpp
inline constexpr RecordTypeCode video_encoded_video_state_record_type = 0x0104;
```

Existing codes remain unchanged:

- `0x0100` VPD1 descriptor
- `0x0101` VFR1 raw video frame state (legacy/new manual API remains valid)
- `0x0102` legacy video PCM16 audio state
- `0x0103` EAP1 encoded AAC state
- `0x0104` EVP1 encoded H.264 video state

## Public state model

```cpp
enum class EncodedVideoCodec : std::uint16_t {
  h264 = 1,
};

enum class EncodedVideoPacketFraming : std::uint8_t {
  unknown = 0,
  length_prefixed = 1,
  annex_b = 2,
};

struct EncodedVideoPacket {
  std::int64_t pts_offset_ns{};
  std::int64_t dts_offset_ns{};
  std::uint64_t duration_ns{};
  std::uint32_t flags{};
  std::vector<std::byte> payload;
};

struct EncodedVideoState {
  EncodedVideoCodec codec{EncodedVideoCodec::h264};
  EncodedVideoPacketFraming framing{EncodedVideoPacketFraming::unknown};
  std::int32_t codec_profile{};
  std::int32_t codec_level{};
  std::uint32_t coded_width{};
  std::uint32_t coded_height{};
  std::uint32_t sample_aspect_ratio_numerator{1};
  std::uint32_t sample_aspect_ratio_denominator{1};
  std::uint64_t validated_frames{};
  std::uint64_t presentation_lead_ns{};
  std::vector<std::byte> decoder_config;
  std::vector<EncodedVideoPacket> packets;
};
```

`pts_offset_ns` and `dts_offset_ns` are signed offsets from the encoded packet origin. The packet origin is the minimum retained DTS. Consequently all retained DTS offsets are non-negative after finalization, while PTS remains allowed to differ because H.264 reorder delay is normal. `presentation_lead_ns` is the delay from packet origin to the first validated presented frame. This prevents B-frame reorder timestamps from being mistaken for archive corruption and provides one global synchronization reference for encoded audio.

## EVP1 binary schema

EVP1 is big-endian and length-delimited. The fixed header contains:

- magic `EVP1`;
- schema version 1;
- codec and packet-framing enums;
- codec profile and level;
- coded width/height;
- sample-aspect-ratio numerator/denominator;
- validated decoded-frame count;
- presentation lead in nanoseconds;
- decoder-config length;
- packet count;
- aggregate packet-payload length.

Each packet table entry contains signed PTS/DTS offsets, duration, flags, and payload length. Packet payloads follow the table in packet order.

The decoder validates before allocation or multiplication:

- codec is H.264;
- framing enum is known;
- dimensions are non-zero and within Video Profile bounds;
- SAR denominator is non-zero;
- validated frame count is non-zero;
- packet count is non-zero and bounded;
- each payload is non-empty and bounded;
- durations are positive;
- DTS offsets are non-negative and monotonic non-decreasing;
- packet-table/config/payload arithmetic cannot overflow;
- exact input length matches the declared lengths.

Decoder configuration is allowed to be empty because MPEG-TS/HLS H.264 may carry SPS/PPS in-band. Empty configuration is a valid EVP1 state, not archive corruption.

## Verified reader

Add `query_verified_video_encoded_video()` parallel to `query_verified_video_encoded_audio()`.

The reader must verify:

1. archive integrity first;
2. record type exactly `0x0104`;
3. canonical EVP1 decode within explicit limits;
4. exactly one matching S1 provenance sidecar;
5. every provenance link resolves to an exact committed record;
6. at least one direct input is S0;
7. process contract identifies `codec.video.encoded-video.preserve` version 1;
8. state interval and selected stream/time query agree;
9. aggregate encoded-byte/result limits are enforced.

## Ingest data flow

### Direct media

```text
captured source bytes
  -> commit exact S0
  -> FFmpeg demux
      -> capture selected H.264 AVPacket payload/timing/flags
      -> copy codecpar extradata/profile/level/dimensions/SAR
      -> send same packet to decoder
      -> validate decoded frames/timeline/dimensions
      -> discard decoded frame immediately
  -> finalize one EVP1 state + S1 provenance
  -> existing AAC EAP1 state + provenance
```

### HLS

The existing memory-only/same-origin resource interception remains authoritative. Every accepted manifest/segment remains exact S0 before FFmpeg reads it. The selected H.264 packet capture happens at the same `av_read_frame` interception seam already used for AAC. Decoded frames are used only to establish presentation timing and validation; no `canonicalize_frame()`, `sws_scale()`, `av_image_copy_to_buffer()`, or VFR1 write occurs for new HLS ingest.

## Packet framing

At stream configuration time:

- H.264 extradata beginning with AVCDecoderConfigurationRecord version byte `0x01` implies `length_prefixed` packet framing.
- Annex-B start-code extradata implies `annex_b`.
- If extradata is empty, packet inspection may classify Annex B only when a valid start-code prefix is present; otherwise framing remains `unknown` and MP4 passthrough later fails explicitly.

Packet bytes stored in EVP1 are never rewritten to change framing.

## MP4 export

Export dispatch prefers EVP1 when present.

### EVP1 video

For H.264 `length_prefixed` with usable AVC decoder configuration, construct an MP4 video stream directly from EVP1 codec parameters and write the stored packets with rescaled timestamps.

For H.264 `annex_b`, pass stored packet copies through FFmpeg's `extract_extradata` bitstream filter when global configuration is absent. The filter is used to recover codec metadata; stored archive packet bytes remain unchanged. The MOV/MP4 muxer receives Annex-B configuration/packets and performs its normal Annex-B-to-MP4 NAL conversion without video decoding or encoding.

If configuration/framing cannot be made representable, return `model_incompatible`, never `archive_corrupt` solely because container-global configuration was absent.

### EAP1 audio

When AAC decoder configuration exists, retain the current direct packet passthrough.

When it is empty and packets are ADTS-framed, run packet copies through FFmpeg `aac_adtstoasc`; use its derived AudioSpecificConfig and stripped packet output for MP4 muxing. This is a compressed-domain bitstream transformation, not AAC decoding/re-encoding. If neither stored configuration nor ADTS recovery can produce an MP4-compatible AAC stream, return `model_incompatible`.

### A/V synchronization

Video packet origin is output timestamp zero. First presented video frame occurs at `presentation_lead_ns`. Encoded audio is shifted by:

```text
presentation_lead_ns + (audio_state_record.start_ns - video_state_record.start_ns)
```

Any additional global non-negative timestamp shift required by the muxer applies equally to video and audio so relative synchronization is unchanged.

## Legacy export

If no EVP1 state exists, current VFR1 export remains unchanged: query verified raw frames, encode H.264, then add EAP1/legacy PCM16 audio as today.

An archive containing both verified EVP1 and VFR1 states for the same selected export interval is contradictory for automatic H.1 export and fails closed instead of silently choosing one.

## Resource accounting

`FfmpegVideoIngestRequest::maximum_decoded_bytes` remains a compatibility-named guard but changes meaning for new ingest: it bounds retained encoded-video state bytes plus bounded decoder working validation rather than accumulated persisted pixels. A later API cleanup may rename it, but this change does not break the public request structure.

EVP1 has independent decode limits for packet count, decoder configuration, individual packet size, and aggregate payload size.

## Compatibility

- Existing manual VFR1 encode/decode/query APIs remain source-compatible.
- Existing VFR1 archives remain readable/exportable.
- Existing EAP1 and legacy PCM16 readers remain available.
- S0 capture/extract is unchanged.
- Generic CODA record format is unchanged.
- FFmpeg-disabled builds can encode/decode/query EVP1 records but cannot perform media ingest/export and continue to fail with explicit `model_incompatible`.

## Test contract

1. EVP1 encode/decode round-trip including negative PTS relative to presentation, non-negative DTS, B-frame reorder, config, limits, and corrupt lengths.
2. Verified EVP1 reader provenance/corruption/filter/limit tests.
3. Direct FFmpeg ingest fixture proves exactly one EVP1 state, no VFR1 records, exact H.264 packet payload round-trip, decoded validation success, and EAP1 preservation.
4. HLS ingest fixture proves EVP1 with exact primary+child S0 provenance frontier and no VFR1 writes.
5. MP4 export from EVP1+EAP1 proves H.264/AAC streams are present and decodable, packet passthrough flags report true, and no H.264/AAC encoder path is invoked.
6. ADTS AAC fixture with empty codecpar extradata exports successfully through `aac_adtstoasc` instead of producing `archive_corrupt`.
7. Legacy VFR1 + PCM16/EAP1 export tests remain green.
8. FFmpeg-disabled build compiles/tests EVP1 schema/reader and reports export backend unavailable.
9. CLI `video export --all` succeeds for encoded-video archives and preserves per-stream error JSON for genuinely incompatible states.

## Performance evidence

The implementation may claim that new ingest no longer persists per-frame raw pixels and compatible export no longer H.264/AAC encodes because tests can directly prove those code paths and record forms. It must not claim a universal compression ratio or throughput multiplier without measured workload/hardware evidence.

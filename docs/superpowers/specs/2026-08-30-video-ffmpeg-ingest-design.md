# FFmpeg Video Ingest Bridge Design

Date: 2026-08-30  
Repository: `jordanlegare/codec`  
Base: `main` at `226e7d099a3ebaf8fc12b38a8464881ed7608b04`  
Status: Approved by the user's explicit request to implement the previously proposed FFmpeg-backed S0-to-S1 bridge.

## Decision

Add a profile-owned, compile-time optional FFmpeg integration that accepts one bounded source URI/path, captures the accepted encoded/container representation through CODEC's existing capture policy, commits those exact bytes as S0, and then decodes the captured in-memory bytes into canonical H.1 `VFR1` raw-frame S1 records with exact same-stream provenance.

This is a user-directed H.1 video integration follow-on. It does not mark H.2 telemetry, H.3 sensor, H.4 document/event, H.5 network/system, or H.6 schema/model-bundle work complete. Stage G remains deferred.

## Goals

1. Preserve the exact accepted encoded/container source bytes before media interpretation.
2. Decode ordinary FFmpeg-supported video containers/codecs, including MP4/H.264 when the linked FFmpeg build supports them.
3. Emit only existing H.1 canonical layouts: Gray8, RGB24, RGBA32, or YUV420P8.
4. Keep each emitted `VFR1` state verifiable by the existing `query_verified_raw_video_frames()` contract.
5. Bound source bytes, decoded bytes, frame count, dimensions, and archive output behavior.
6. Keep builds without FFmpeg valid and explicit about backend unavailability.
7. Prevent FFmpeg demuxers from independently opening nested network/file resources during interpretation.

## Non-goals

This change does not add playback, video export/transcoding, streaming inference, a media server, HLS/DASH fetching, arbitrary FFmpeg protocol access, a production model, GPU decode, quality claims, codec-coverage guarantees, performance claims, or a new CLI command.

It does not change CODA envelopes, generic `RecordType`, stream identity, S0/S1/D meanings, the H.1 VPD1/VFR1 binary schemas, or the verified-reader process contract.

## Public API

The existing `<codec/profiles/video.hpp>` remains the installed Video Profile entry point and gains:

```cpp
struct FfmpegVideoIngestRequest {
  std::string source_uri;
  std::filesystem::path archive_path;
  StreamDescriptor descriptor;
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  PixelLayout output_layout{PixelLayout::yuv420p8};
  std::size_t capture_chunk_bytes{256U * 1024U};
  std::uint64_t maximum_source_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_decoded_bytes{1024ULL * 1024ULL * 1024ULL};
  std::size_t maximum_frames{4096};
  std::uint32_t maximum_redirects{5};
  bool deny_private_network{true};
};

struct FfmpegVideoIngestReport {
  std::filesystem::path archive_path;
  RecordInfo descriptor;
  RecordInfo source;
  std::vector<RecordInfo> states;
  std::vector<RecordInfo> provenance;
  std::optional<Error> profile_error;

  bool state_exact() const noexcept;
};

bool ffmpeg_video_ingest_available() noexcept;

Result<FfmpegVideoIngestReport> ingest_video_ffmpeg(
    const FfmpegVideoIngestRequest& request);
```

No FFmpeg type appears in a public header.

## Request validation

The request is rejected with `invalid_argument` before capture when:

- `source_uri` is empty;
- `archive_path` is empty or has no filename;
- `end_ns <= start_ns`;
- capture chunk size is outside 4 KiB through 16 MiB;
- source/decoded limits or frame limit are zero or outside process bounds;
- redirect limit exceeds 20;
- the descriptor is not `StreamType::video`;
- the descriptor has an empty payload type;
- the requested output layout is not one of the four H.1 layouts.

An existing output path is refused exactly as existing preservation-first Audio ingest does.

When the library was built without the FFmpeg video backend, valid requests return an explicit `model_incompatible` error stating that the FFmpeg video ingest backend is unavailable. The availability probe returns `false`.

## Preservation-first data flow

1. Validate the request and backend availability.
2. Prepare and run the existing bounded CODEC capture path.
3. Accumulate at most `maximum_source_bytes` accepted bytes.
4. Create the archive, append the caller's video `StreamDescriptor`, append one same-stream `source_bytes` S0 record covering `[start_ns, end_ns)`, and retain its exact `RecordInfo`.
5. Decode only the already captured in-memory S0 bytes through FFmpeg custom AVIO.
6. If demux/decode/canonicalization fails after the S0 record is committed, finalize the archive with descriptor + S0 only and return success with `profile_error` populated.
7. If decode succeeds, append one `raw_video_frame_state_record_type` VFR1 record per canonical frame and one S1 provenance sidecar per frame, then finalize.

No decode error removes or rewrites accepted S0.

## FFmpeg isolation boundary

FFmpeg is never handed the original `source_uri`. The demuxer receives a custom `AVIOContext` backed by the captured byte vector. The read callback only advances within that immutable span; the seek callback supports bounded seeks and `AVSEEK_SIZE`.

The format context installs an `io_open` callback that rejects all secondary resource opens. This prevents playlists, manifests, or demuxers from using FFmpeg's own URL protocols to fetch nested network/file resources. Sources that require secondary resources therefore become source-only archives with a profile decode error rather than expanding CODEC's authorization surface.

## Stream selection and decode

`avformat_find_stream_info()` is run against custom AVIO. `av_find_best_stream(..., AVMEDIA_TYPE_VIDEO, ...)` chooses one video stream. Audio, subtitles, attachments, and data streams are ignored.

The decoder context is created from the selected stream's codec parameters. Decode thread count is fixed to one. Packets from other streams are discarded. Frames are buffered only up to `maximum_frames` and `maximum_decoded_bytes`.

A malformed source, absent video stream, unavailable decoder, packet/send/receive failure, unsupported dimensions, or resource-limit breach is a profile error after S0 preservation.

## Canonical pixel conversion

The requested `output_layout` maps to:

- Gray8 -> `AV_PIX_FMT_GRAY8`;
- RGB24 -> `AV_PIX_FMT_RGB24`;
- RGBA32 -> `AV_PIX_FMT_RGBA`;
- YUV420P8 -> `AV_PIX_FMT_YUV420P`.

Every decoded frame is converted through `libswscale` to remove decoder stride/padding and format variation. The resulting planes are copied row-by-row into the exact H.1 canonical layout with no row padding.

For YUV420P8, odd decoded dimensions are rejected because H.1 requires even coded width and height. Other H.1 geometry and payload bounds are revalidated by `encode_raw_video_frame_state()`.

The descriptor records decoded width/height, requested layout, valid sample aspect ratio when available (else 1/1), and valid average frame rate when available (else 0/1). Color range/primaries/transfer/matrix are mapped only when FFmpeg values have an exact H.1 counterpart; otherwise they remain `unspecified`. RGB/RGBA output uses `MatrixCoefficients::identity`.

## Frame time mapping

Frame timestamps use `best_effort_timestamp` from the selected stream. The first valid decoded timestamp becomes the zero point for the caller's `start_ns`. Relative timestamps are rescaled with FFmpeg integer rational rescaling into nanoseconds.

If a frame lacks a timestamp, the backend uses the selected stream's valid average frame rate to advance from the preceding frame. If neither timestamp nor usable frame rate can establish a monotonic time, decode fails closed.

After all frames are decoded, each frame end is the next frame start. The last frame end uses one nominal frame duration when available, capped at `request.end_ns`; otherwise it uses `request.end_ns`. Every emitted frame must satisfy `start_ns <= frame_start < frame_end <= end_ns`, preserving the verified reader's source/state overlap invariant.

## Provenance

Each VFR1 record points directly to the one exact same-stream S0 source record. It uses the existing H.1 canonical process contract unchanged:

```text
operation: codec.video.raw-frame.canonicalize
implementation_id: codec.video
implementation_version: 1
details_type: application/vnd.codec.video.canonicalization.v1
details: exactly one byte 0x01
```

The FFmpeg library version and requested output layout are hashed into `configuration_hash` for additional traceability without changing the H.1 verified-reader contract. No claim is made that a decoder is mathematically equivalent to an encoded source; the claim is that the archived canonical decoded frame bytes are exact S1 state produced by the recorded process from exact S0 input.

## Build integration

Add CMake option:

```cmake
option(CODEC_ENABLE_FFMPEG_VIDEO
  "Enable the optional FFmpeg Video Profile ingest backend" OFF)
```

When enabled, CMake requires pkg-config modules `libavformat`, `libavcodec`, `libavutil`, and `libswscale`, defines `CODEC_HAS_FFMPEG_VIDEO=1`, and links their imported targets. The source file is always compiled so the unavailability API exists in all builds.

The installed CMake package records whether the library was built with this backend; when enabled, the package config recreates the required FFmpeg pkg-config imported targets so static consumers link correctly.

CI installs the four FFmpeg development packages and enables the option in GCC, Clang, sanitizer, install, and package-consumer jobs. A separate no-FFmpeg configuration proves that the default dependency-free build still succeeds and the backend reports unavailable.

## Tests

`tests/test_video_ffmpeg_ingest.cpp` embeds a tiny deterministic one-frame MP4/H.264 fixture as bytes. The fixture is source-extracted after ingest and compared byte-for-byte with the input.

With FFmpeg enabled the tests prove:

- availability is true;
- MP4/H.264 is demuxed/decoded from custom in-memory AVIO;
- exact S0 source preservation;
- one canonical YUV420P8 VFR1 frame is emitted with expected dimensions/pixels;
- `query_verified_raw_video_frames()` returns that frame and exact source link;
- malformed encoded bytes produce a finalized source-only archive with `profile_error`;
- resource limits and invalid requests fail safely;
- a secondary-open-requiring input cannot escape custom AVIO authorization.

Without FFmpeg enabled the tests prove availability is false and a valid ingest request returns explicit backend unavailability without creating an archive.

The installed package consumer exercises the availability API unconditionally and, when the package was built with FFmpeg, compiles against the ingest request/report API without including FFmpeg headers.

## Documentation claims

README and CHANGELOG may claim only an optional bounded FFmpeg-backed Video Profile ingest bridge. They must continue to state that playback, export/transcoding, arbitrary protocol fetching, GPU decode, streaming inference, model quality, throughput, latency, scale, H.2-H.6 completion, and Stage G completion are not provided.

## Exit criteria

The change is complete only when the exact PR head passes GCC and Clang warnings-as-errors builds, full CTest, install/package consumer, ASan/UBSan, dependency-free backend-unavailable configuration, H.1 regression tests, CLI/C ABI regressions, and the repository AI contract; issue #10 records the exact head and CI evidence before merge.

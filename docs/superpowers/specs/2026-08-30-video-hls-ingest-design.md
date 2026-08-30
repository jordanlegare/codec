# Preservation-First HLS Video Ingest Design

Date: 2026-08-30
Repository: `jordanlegare/codec`
Status: Approved in chat; written-spec review pending
Base: `main` at `f68515068a022ef4f16eefdc1df0512b94bcec77`
Branch: `codex/video-hls-ingest`

## Decision

Pause Stage H.2 Telemetry work without modifying its branch and add one focused H.1 video-integration follow-on:

> Extend `ingest_video_ffmpeg()` and `codec video ingest` to support bounded HTTP/HTTPS HLS by preserving the primary manifest and every FFmpeg-requested child resource as exact S0 before FFmpeg may read that child, while keeping all network authorization and capture inside CODEC.

FFmpeg remains a demux/decode engine, not a network authority. The implementation must never enable FFmpeg's native HTTP, HTTPS, file, crypto, or other URL protocols as a fallback.

## Why this shape

The existing bridge captures one source object, stores it as S0, and decodes only those captured bytes through custom AVIO. That is correct for MP4 and other self-contained media, but an HLS manifest is a resource graph. A media playlist may reference segments, initialization objects, sub-playlists, and later live-manifest snapshots.

Three approaches were considered:

1. **Reimplement HLS assembly in CODEC.** Rejected because it would duplicate mature libavformat behavior for master/media playlists, timestamp/discontinuity handling, MPEG-TS/fMP4, byte ranges, and live refresh.
2. **Pre-parse and prefetch the full manifest graph, then hand a frozen bundle to FFmpeg.** Safer than native FFmpeg networking but still duplicates playlist traversal and does not naturally support live refresh.
3. **FFmpeg-driven, CODEC-authorized on-demand resource capture.** Selected. FFmpeg decides which HLS resource it needs; CODEC independently validates, captures, archives, and bounds that request, then exposes only the captured bytes through custom AVIO.

## Security boundary

### Primary source

The initial `source_uri` continues through `codec::detail::PreparedCapture`. Its accepted bytes are committed as the parent video stream's normal `RecordType::source_bytes` S0 record before media interpretation.

For an HLS manifest, FFmpeg receives the original source URI only as demux/probe/base-URL context while `AVFormatContext::pb` remains the already-captured memory AVIO. The primary URL must not be reopened by FFmpeg networking.

### Secondary HLS resources

A custom FFmpeg `io_open` callback is installed only for an input recognized as HLS. For every child URL requested by the HLS demuxer:

1. reject write access;
2. require an absolute `http://` or `https://` URL;
3. require the normalized origin to equal the primary HLS source origin (same scheme, host, and effective port);
4. reject `file:`, `crypto:`, `data:`, `concat:`, arbitrary FFmpeg protocols, cross-origin URLs, and malformed URLs;
5. run a fresh `PreparedCapture::prepare()` using the request's existing capture chunk/private-network/redirect policy;
6. bound the individual resource and aggregate HLS capture before accepting it;
7. scan playlist-shaped resources for unsupported encryption before exposing their bytes;
8. append the child resource's descriptor and exact `source_bytes` S0 record to CODA;
9. only then return a read-only seekable custom AVIO context over the captured in-memory bytes.

There is no fallback to FFmpeg's default protocol opener. Any denied or failed child capture is recorded as the ingest `profile_error`, and FFmpeg receives an I/O failure.

### Origin normalization

Use libcurl's URL parsing API or an equivalently strict parser. Normalize scheme and host case and normalize default ports (`http:80`, `https:443`) before equality comparison. User-info, path, query, and fragment do not define origin. Fragments are not sent as resource identifiers.

Cross-origin HLS is deliberately unsupported in this slice. This prevents an attacker-controlled public manifest from expanding one authorized source into arbitrary public-network reads. A future explicit allowlist can broaden this policy if a demonstrated stream requires it.

### Redirects

No new redirect behavior is introduced. Secondary requests inherit the existing capture policy. The current capture implementation refuses HTTP redirects rather than letting a child resource change authorization origin after validation.

## HLS detection

Treat the primary source as HLS only when its captured bytes, after optional UTF-8 BOM and leading ASCII whitespace, begin with `#EXTM3U` and contain HLS `#EXT-X-` tags. A `.m3u8` URI is useful base-URL/probe context but does not by itself override non-HLS bytes.

For HLS, call `avformat_open_input()` with the original source URI as the filename/context argument while retaining the custom primary `pb`. For non-HLS media, retain the existing direct-memory behavior and reject all secondary opens exactly as today.

## Encryption policy

This first HLS slice does not fetch or persist decryption keys.

Before FFmpeg sees any manifest-shaped resource, parse `#EXT-X-KEY` lines case-sensitively according to HLS tag spelling. `METHOD=NONE` is allowed. Any other method, including AES-128 and SAMPLE-AES variants, returns `ErrorCode::model_incompatible` with a stable message that encrypted HLS is unsupported.

The rejection occurs before a key URI can be fetched. The raw manifest bytes already accepted remain preserved as S0.

## Child-resource archive model

Do not concatenate HLS resources onto the parent stream's `source_bytes`; that would make generic source-exact extraction produce bytes that never existed as one source object.

Each accepted secondary open gets its own deterministic child stream:

- `StreamType::opaque`;
- `source_id = "codec.video.hls-resource"`;
- `payload_type = "application/octet-stream"`;
- label derived from the parent label plus a zero-padded resource ordinal, without persisting the requested URL;
- `StreamId` derived from a versioned constant, the parent stream ID, and the monotonically increasing accepted-resource ordinal.

Append one stream descriptor and one exact `RecordType::source_bytes` record for each accepted child snapshot. Reopening the same URL creates a new child snapshot rather than silently reusing stale bytes; the resource-count/byte limits bound repeated live refreshes.

The requested child URL and query string remain process-memory routing data and are not persisted in `StreamDescriptor` or new profile metadata. Existing raw source bytes may of course contain URLs because HLS manifests themselves are preserved exactly.

## Public API changes

Extend `FfmpegVideoIngestRequest` with HLS-specific capture limits:

```cpp
std::size_t maximum_hls_resources{256};
std::uint64_t maximum_hls_resource_bytes{64ULL * 1024ULL * 1024ULL};
std::uint64_t maximum_hls_total_bytes{1024ULL * 1024ULL * 1024ULL};
```

Validation requires all three to be non-zero and representable by process/container limits. These limits are independent from `maximum_source_bytes`, which continues to bound the primary source object.

Extend `FfmpegVideoIngestReport` with:

```cpp
std::vector<RecordInfo> secondary_descriptors;
std::vector<RecordInfo> secondary_sources;
```

For direct media these vectors remain empty. For HLS they contain accepted child snapshots in open order. `state_exact()` keeps its existing meaning: successful S1 generation with one provenance sidecar per state and no `profile_error`.

The CLI gains:

```text
--maximum-hls-resources N
--maximum-hls-resource-bytes N
--maximum-hls-total-bytes N
```

and successful/error JSON adds:

```json
"secondary_sources": N,
"secondary_source_bytes": N
```

No `--allow-private-network` or cross-origin bypass is added in this slice.

## FFmpeg I/O lifecycle

Create an internal HLS capture session owned for the duration of decode. It contains:

- the primary normalized origin;
- request capture/limit policy;
- `CodaWriter*` for immediate S0 append;
- monotonically increasing resource ordinal;
- aggregate secondary byte count;
- stable heap-owned child-resource byte buffers;
- accepted descriptor/source `RecordInfo` vectors;
- the first CODEC error encountered by an FFmpeg callback.

The HLS `io_open` callback allocates a read-only `AVIOContext` backed by one stable captured child buffer. A matching custom close callback frees only the AVIO/read-cursor allocation; archived bytes remain owned by the session until decode ends.

Any callback failure stores the precise CODEC error in the session. When FFmpeg subsequently returns an I/O/demux error, CODEC prefers that stored error over replacing it with a generic FFmpeg decode error.

## Live HLS termination

A live playlist must not make `ingest_video_ffmpeg()` run indefinitely merely because it lacks `#EXT-X-ENDLIST`.

Extend the decode loop with a requested-duration stop condition. Once timestamps are available, use the first accepted frame timestamp as origin. Do not accept a frame whose relative start is greater than or equal to `end_ns - start_ns`; mark the requested duration reached and stop demuxing. When timestamps are unavailable but a usable nominal frame rate exists, use the same synthetic timeline rule already used by `map_frame_times()`.

The decoder may also terminate through `maximum_frames`, HLS resource limits, capture errors, or ordinary EOF. The stop condition does not claim wall-clock live recording for exactly N seconds; it bounds the decoded media timeline represented in the requested archive interval.

## Provenance contract

The existing single-object canonicalization contract remains valid and unchanged:

```text
operation: codec.video.raw-frame.canonicalize
implementation_id: codec.video
implementation_version: 1
details_type: application/vnd.codec.video.canonicalization.v1
details: 0x01
```

HLS uses a distinct contract:

```text
operation: codec.video.raw-frame.canonicalize.hls
implementation_id: codec.video
implementation_version: 1
details_type: application/vnd.codec.video.hls-canonicalization.v1
details: 0x01
```

Every HLS VFR1 state has direct provenance inputs containing:

1. the same-stream primary manifest `source_bytes` record; and
2. every accepted child `source_bytes` snapshot whose capture completed before that frame was accepted by the decoder.

This is a conservative causal capture frontier. CODEC does not claim FFmpeg exposes segment-level packet lineage precise enough to identify the minimal byte-object subset for each frame. The process contract therefore means: this exact canonical frame was produced during the bounded decode session after exactly this preserved source frontier had been accepted.

## Verified-reader extension

`query_verified_raw_video_frames()` must remain strict for existing direct-media provenance and additionally recognize the HLS contract.

For the direct contract, keep all current same-stream S0 rules unchanged.

For the HLS contract require:

- the subject is canonical VFR1 S1 with `TruthClass::state_exact`;
- input 0 resolves to same-stream `RecordType::source_bytes` and overlaps the state interval;
- each later input resolves to `RecordType::source_bytes` on a different child stream;
- each child stream has exactly one matching `StreamDescriptor` with `type == StreamType::opaque` and `source_id == "codec.video.hls-resource"`;
- no child input equals the subject or primary input;
- all source records overlap the requested ingest interval/state interval by the profile's envelope-time rule;
- the HLS process fields/details match exactly.

A malformed, missing, duplicate, dangling, wrong-process, or structurally unrelated input returns `archive_corrupt` rather than being silently ignored.

## Failure semantics

The preservation-first transaction boundary remains:

- validation/capture failure before the primary S0 archive exists returns an ordinary error and creates no successful archive;
- after primary S0 exists, any HLS detection, encryption, child authorization, child capture, resource limit, FFmpeg demux/decode, canonicalization, or timeline failure finalizes the archive with every exact S0 object already accepted and reports `profile_error`;
- no S1 records are written until the whole bounded decode/timeline mapping succeeds, preserving the existing source-only-on-profile-failure property;
- writer/archive I/O failures remain hard errors rather than profile errors.

## Files and boundaries

Expected production changes:

- `include/codec/profiles/video.hpp` — request/report limit/result additions only;
- `src/video/ffmpeg_ingest.cpp` — HLS detection, capture session, custom secondary AVIO, live termination, HLS provenance;
- `src/video/frame_state_reader.cpp` — strict recognition of the second provenance contract;
- `src/cli/main.cpp` — three HLS limit options and result counters;
- optionally one focused internal helper under `src/video/` if URL/HLS parsing makes `ffmpeg_ingest.cpp` materially harder to review.

Expected test/docs changes:

- new focused HLS integration tests and fixtures;
- existing direct FFmpeg/video tests retained unchanged except where report initialization requires new fields;
- `README.md`, `CHANGELOG.md`, `AI_WORKSHEET.md`, and roadmap issue #10 evidence;
- `tests/ai_contract.cmake` only if it currently encodes the old no-HLS non-claim as a hard prohibition.

No generic archive envelope, generic stream semantics, capture authorization implementation, audio, transport, distributed, telemetry, C ABI, or model subsystem is forked.

## Test contract

### Successful HLS

Use a hermetic local HTTP fixture with `deny_private_network=false` at the C++ API boundary. Serve an HLS media playlist and deterministic tiny media resource(s). Prove:

- manifest is exact parent S0;
- every child response is exact child-stream S0;
- relative segment URLs resolve against the original manifest URI;
- child resource descriptors do not persist raw URLs;
- FFmpeg decodes VFR1 through the existing four canonical layouts where applicable;
- verified reader accepts HLS provenance;
- report resource counts/bytes match archive records.

### Security/failure

Prove:

- file/crypto/non-HTTP child schemes are denied;
- cross-origin child URLs are denied before capture;
- private-network policy remains effective when enabled;
- encrypted manifests are rejected before key fetch;
- per-resource, aggregate-byte, and resource-count limits preserve accepted S0 and produce `resource_exhausted` profile errors;
- malformed child media produces a finalized source-only/resource-only archive;
- no FFmpeg native-network fallback occurs if the custom callback denies a child.

### Live boundedness

Serve a playlist without `#EXT-X-ENDLIST`. Prove the decode stops once the requested media timeline is covered and does not refresh indefinitely. Resource-count bounds remain a second fail-safe.

### Compatibility

Prove:

- direct MP4/BMP behavior and exact S0 remain unchanged;
- FFmpeg-disabled builds still compile/install/package and return `model_incompatible` before archive creation;
- `codec record` remains generic S0-only capture;
- HLS support adds no DASH claim;
- installed public headers contain no FFmpeg/libcurl types.

### CI

The exact final head must pass default-enabled GCC, Clang, sanitizers, full tests, install/package consumer, CLI integration, C ABI, AI contract, and the explicit `CODEC_ENABLE_FFMPEG_VIDEO=OFF` job.

## Non-goals

This slice does not add:

- DASH;
- cross-origin HLS;
- encrypted HLS/key capture or decryption;
- cookies, custom request headers, bearer-token propagation, or browser-session emulation;
- redirects beyond existing capture behavior;
- arbitrary FFmpeg protocol/network access;
- persistent raw requested child URLs outside exact manifest bytes;
- playback, export/transcoding, GPU decode, model inference, codec-quality, performance, or production-scale claims;
- changes to Stage H.2+ roadmap semantics.

## Exit criteria

The HLS follow-on is complete only when:

1. the Nova-Scotia-style relative HTTP/HTTPS media-playlist shape is supported by `codec video ingest` without giving FFmpeg network authority;
2. primary and child source objects are individually preserved as exact S0 records;
3. encrypted, cross-origin, private/forbidden, malformed, and over-limit resource graphs fail closed;
4. live HLS is bounded by requested media duration and resource limits;
5. verified HLS VFR1 requires the exact versioned HLS provenance structure;
6. existing direct-media ingest and FFmpeg-disabled builds remain green;
7. README/CHANGELOG security claims match the proven behavior;
8. the exact PR head and merged `main` pass the full CI matrix before Stage H.2 resumes.

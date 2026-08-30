# Preservation-First HLS Video Ingest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the existing FFmpeg Video Profile ingest path so bounded same-origin HTTP/HTTPS HLS playlists can preserve their manifest/resource graph as exact S0 and emit provenance-verified H.1 VFR1 S1 without giving FFmpeg network authority.

**Architecture:** Keep HLS policy/profile logic under `src/video/`. The parent manifest remains the video stream's exact S0. FFmpeg drives HLS resource demand, but each secondary request is intercepted by a CODEC callback, origin-checked, captured through `PreparedCapture`, committed to a deterministic opaque child stream, then exposed to FFmpeg only through read-only custom AVIO. Existing direct-media decoding keeps its deny-all secondary-I/O behavior unchanged.

**Tech Stack:** C++20, CMake 3.20+, libcurl URL API, existing CODEC capture/archive/provenance APIs, FFmpeg libavformat/libavcodec/libavutil/libswscale, existing lightweight C++ tests, Bash CLI integration, GCC/Clang, ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-30-video-hls-ingest-design.md`

## Global Constraints

- Base implementation target is `main` at `f68515068a022ef4f16eefdc1df0512b94bcec77`; H.2 telemetry work remains untouched on its separate branch.
- Keep FFmpeg enabled by default; preserve the explicit `-DCODEC_ENABLE_FFMPEG_VIDEO=OFF` build and package-consumer path.
- Never let FFmpeg perform native HTTP, HTTPS, file, crypto, data, concat, or arbitrary protocol I/O as a fallback.
- Treat captured bytes as S0 before interpretation. Do not concatenate HLS child resources into the parent stream's source bytes.
- HLS child resources are separate deterministic `StreamType::opaque` streams with `source_id == "codec.video.hls-resource"`; raw requested child URLs are not persisted in descriptors.
- Support only same-origin HTTP/HTTPS child requests. Cross-origin HLS, encrypted HLS, cookies, custom headers, browser-session emulation, redirects beyond current capture behavior, DASH, playback, transcoding, GPU decode, and model inference remain out of scope.
- HLS canonical VFR1 remains S1 only under `TruthClass::state_exact` and the exact HLS provenance contract: `codec.video.raw-frame.canonicalize.hls`, `codec.video`, version `1`, details type `application/vnd.codec.video.hls-canonicalization.v1`, details byte `0x01`.
- Existing direct-media provenance (`codec.video.raw-frame.canonicalize`) and direct MP4/BMP behavior remain byte-for-byte compatible.
- No generic CODA envelope, `RecordType`, stream, capture authorization, audio, distributed, transport, C ABI, or telemetry semantics change.

---

### Task 1: HLS policy helpers and public request/report bounds

**Files:**
- Create: `src/video/hls_policy.hpp`
- Create: `src/video/hls_policy.cpp`
- Create: `tests/test_video_hls_policy.cpp`
- Modify: `include/codec/profiles/video.hpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `codec::Result`, `codec::ErrorCode`, libcurl URL parsing, `codec::StreamId`.
- Produces internal helpers:

```cpp
namespace codec::profiles::video::detail {

struct HlsOrigin {
  std::string scheme;
  std::string host;
  std::uint16_t port{};
  bool operator==(const HlsOrigin&) const = default;
};

bool looks_like_hls_manifest(std::span<const std::byte> bytes) noexcept;
Result<void> validate_hls_manifest_security(std::span<const std::byte> bytes);
Result<HlsOrigin> parse_hls_http_origin(std::string_view uri);
Result<void> require_same_hls_origin(const HlsOrigin& primary,
                                     std::string_view child_uri);
StreamId derive_hls_child_stream_id(const StreamId& parent,
                                    std::size_t ordinal);
std::string hls_child_label(std::string_view parent_label,
                            std::size_t ordinal);

}  // namespace codec::profiles::video::detail
```

- Extends public request/report at the end of existing aggregate members so current designated initializers remain source-compatible:

```cpp
std::size_t maximum_hls_resources{256};
std::uint64_t maximum_hls_resource_bytes{64ULL * 1024ULL * 1024ULL};
std::uint64_t maximum_hls_total_bytes{1024ULL * 1024ULL * 1024ULL};
```

and report members:

```cpp
std::vector<RecordInfo> secondary_descriptors;
std::vector<RecordInfo> secondary_sources;
```

- [ ] **Step 1: Write the policy RED tests**

Create `tests/test_video_hls_policy.cpp` and include `../src/video/hls_policy.hpp`. Add these tests:

```cpp
TEST(video_hls_policy_detects_manifest_after_bom_and_ascii_whitespace)
TEST(video_hls_policy_does_not_treat_extension_only_as_hls)
TEST(video_hls_policy_rejects_encrypted_key_methods_before_uri_use)
TEST(video_hls_policy_allows_method_none)
TEST(video_hls_policy_normalizes_default_ports_and_host_case)
TEST(video_hls_policy_rejects_cross_origin_and_non_http_children)
TEST(video_hls_policy_child_identity_is_deterministic_without_url_material)
TEST(video_hls_request_rejects_zero_hls_limits_before_archive_mutation)
```

Use exact manifest fixtures such as:

```cpp
const auto plain = bytes("#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:1,\nseg0.ts\n");
const auto encrypted = bytes(
    "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"secret.key\"\n"
    "#EXTINF:1,\nseg0.ts\n");
const auto none = bytes(
    "#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:1,\nseg0.ts\n");
```

Assert `parse_hls_http_origin("HTTPS://Example.COM/path.m3u8")` equals scheme `https`, host `example.com`, port `443`; assert `https://example.com:443/a.ts` is same-origin and `https://example.com:444/a.ts`, `http://example.com/a.ts`, `file:///tmp/a.ts`, `crypto:https://example.com/a.ts`, and `https://other.example/a.ts` fail.

For the public request validation test, construct an otherwise valid local MP4 request, set each new bound to zero independently, call `ingest_video_ffmpeg()`, require `invalid_argument`, and require that the target archive does not exist.

- [ ] **Step 2: Wire only declarations/tests and prove RED**

Add `src/video/hls_policy.cpp` to `codec_core` and `tests/test_video_hls_policy.cpp` to `codec_tests`. Initially create `hls_policy.cpp` with only the include and namespace, so the wished-for helpers are unresolved.

Run through CI on the test-only/stub head. Expected: configure succeeds and compilation/link fails only because the HLS policy functions are not implemented. The explicit FFmpeg-disabled job must fail for the same missing helper symbols, not because FFmpeg libraries are absent.

- [ ] **Step 3: Implement strict HLS detection and encryption scanning**

`looks_like_hls_manifest()` must:

1. strip one UTF-8 BOM if present;
2. skip only ASCII space, tab, CR, and LF;
3. require the remaining bytes to start with `#EXTM3U`;
4. require at least one subsequent `#EXT-X-` tag.

`validate_hls_manifest_security()` scans complete logical lines. For each line beginning exactly `#EXT-X-KEY:`, parse comma-separated attributes outside quoted strings, locate `METHOD=`, and allow only `METHOD=NONE`. Missing `METHOD` or any other value returns:

```cpp
fail(ErrorCode::model_incompatible,
     "encrypted HLS is unsupported")
```

Do not interpret or fetch a `URI` attribute in this helper.

- [ ] **Step 4: Implement origin normalization using libcurl URL API**

Use `curl_url()`, `curl_url_set(..., CURLUPART_URL, ...)`, and `curl_url_get()` for scheme, host, and port. Accept only `http` and `https`; lowercase scheme/host; use port 80/443 when absent; reject ports outside 1..65535. `require_same_hls_origin()` parses the child independently and returns `unauthorized_source` for a different normalized origin and `protocol` for non-HTTP/HTTPS syntax.

`derive_hls_child_stream_id()` must derive from this exact identity text:

```text
codec.video.hls-resource.v1\n<PARENT_UUID>\n<ORDINAL>
```

using existing `derive_stream_id()`. `hls_child_label("NS", 7)` returns `NS:hls-resource-0007`; reject/guard ordinals above 999999 in the capture session rather than leaking URL-derived labels.

- [ ] **Step 5: Extend request/report validation minimally**

Append the three public request members and two report vectors. In `validate_request()` reject zero HLS limits, `maximum_hls_resource_bytes > size_t::max`, `maximum_hls_total_bytes > size_t::max`, and `maximum_hls_resources` values that cannot be represented by the child-label/ordinal contract.

Keep the FFmpeg-disabled valid-request behavior unchanged: after validation it returns `model_incompatible` before archive creation.

- [ ] **Step 6: Prove GREEN and commit**

Run the focused test binary with:

```bash
./build/codec_tests --include-prefix video_hls_policy_
./build/codec_tests --include-prefix video_hls_request_
```

and the full CTest suite in both default and `CODEC_ENABLE_FFMPEG_VIDEO=OFF` configurations. Commit as:

```text
Add bounded HLS policy primitives
```

---

### Task 2: Hermetic HLS resource capture and FFmpeg custom secondary AVIO

**Files:**
- Create: `tests/hls_http_fixture.hpp`
- Create: `tests/test_video_hls_ingest.cpp`
- Create: `tests/fixtures/hls_4x4_seg0.ts.b64`
- Create: `tests/fixtures/hls_4x4_seg1.ts.b64`
- Modify: `src/video/ffmpeg_ingest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes Task 1 helpers and existing `PreparedCapture`, `CodaWriter`, `MemoryInput`, frame canonicalization.
- Produces an internal decode-session model in `ffmpeg_ingest.cpp`:

```cpp
struct HlsCapturedResource {
  std::string requested_uri;  // process memory only
  StreamId stream;
  std::vector<std::byte> bytes;
  RecordInfo descriptor_record;
  RecordInfo source_record;
};

struct HlsCaptureSession {
  HlsOrigin primary_origin;
  const FfmpegVideoIngestRequest* request{};
  CodaWriter* writer{};
  std::vector<std::unique_ptr<HlsCapturedResource>> resources;
  std::uint64_t total_bytes{};
  std::optional<Error> callback_error;
  std::vector<RecordInfo>* report_descriptors{};
  std::vector<RecordInfo>* report_sources{};
};
```

and callback signatures compatible with current FFmpeg:

```cpp
int hls_io_open(AVFormatContext*, AVIOContext**, const char*, int,
                AVDictionary**);
int hls_io_close(AVFormatContext*, AVIOContext*);
```

- [ ] **Step 1: Create deterministic committed TS fixtures**

Generate the fixture once during implementation, then commit only base64 text; tests never invoke `ffmpeg` CLI. Use exactly:

```bash
work=$(mktemp -d)
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i 'testsrc2=size=4x4:rate=1' -t 2 \
  -c:v libx264 -threads 1 -g 1 -keyint_min 1 -sc_threshold 0 \
  -pix_fmt yuv420p -f hls -hls_time 1 -hls_list_size 0 \
  -hls_segment_filename "$work/seg%d.ts" "$work/playlist.m3u8"
base64 -w0 "$work/seg0.ts" > tests/fixtures/hls_4x4_seg0.ts.b64
printf '\n' >> tests/fixtures/hls_4x4_seg0.ts.b64
base64 -w0 "$work/seg1.ts" > tests/fixtures/hls_4x4_seg1.ts.b64
printf '\n' >> tests/fixtures/hls_4x4_seg1.ts.b64
```

The expected generated sizes are 1504 bytes for `seg0.ts` and 752 bytes for `seg1.ts`. The media playlist used by the tests is exact text:

```text
#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:1
#EXT-X-MEDIA-SEQUENCE:0
#EXTINF:1.000000,
seg0.ts
#EXTINF:1.000000,
seg1.ts
#EXT-X-ENDLIST
```

- [ ] **Step 2: Add a minimal loopback HTTP fixture**

`tests/hls_http_fixture.hpp` defines:

```cpp
struct HlsHttpResponse {
  int status{200};
  std::string content_type{"application/octet-stream"};
  std::vector<std::byte> body;
};

class HlsHttpFixture {
 public:
  explicit HlsHttpFixture(
      std::map<std::string, HlsHttpResponse> responses);
  ~HlsHttpFixture();
  HlsHttpFixture(const HlsHttpFixture&) = delete;
  HlsHttpFixture& operator=(const HlsHttpFixture&) = delete;
  std::string url(std::string_view path) const;
  std::size_t requests(std::string_view path) const;
};
```

Bind only `127.0.0.1` on an ephemeral port, accept GET requests, strip query only for route lookup when the test explicitly registers the query-free route, send `Connection: close` and a correct `Content-Length`, and count each requested target. The destructor closes the listener and joins its server thread.

- [ ] **Step 3: Write the HLS ingest RED test**

Create `TEST(video_hls_ingest_preserves_manifest_and_segments_before_decode)`. Decode both committed fixture files, serve `/live/playlist.m3u8`, `/live/seg0.ts`, and `/live/seg1.ts`, and construct:

```cpp
video::FfmpegVideoIngestRequest request{
    .source_uri = fixture.url("/live/playlist.m3u8"),
    .archive_path = archive_path,
    .descriptor = codec::StreamDescriptor{
        .id = stream,
        .type = codec::StreamType::video,
        .label = "HLS fixture",
        .source_id = "fixture",
        .payload_type = "application/vnd.apple.mpegurl",
    },
    .start_ns = 0,
    .end_ns = 2'000'000'000,
    .output_layout = video::PixelLayout::yuv420p8,
    .maximum_frames = 4,
    .deny_private_network = false,
};
```

Require `state_exact()`, exactly two secondary source records, exact parent manifest extraction, exact child payload reads matching the two TS fixtures, no raw loopback URL in any child descriptor `source_id`/label, and exactly one HTTP request per segment. Existing code must fail this test because it denies all secondary opens.

- [ ] **Step 4: Prove RED in enabled CI before production changes**

Wire the new test into `codec_tests` and push a RED commit with test fixture/helper only. Expected enabled GCC/Clang/sanitizer failure: the HLS manifest is preserved but `profile_error` is present and no child resources/states exist. The FFmpeg-disabled job may skip the success assertion after confirming backend unavailability; it must still build.

- [ ] **Step 5: Add HLS mode to `decode_video_bytes()`**

Change its signature to receive the writer/report context needed by the session. Detect HLS from already captured primary bytes before `avformat_open_input()`. For HLS:

- run `validate_hls_manifest_security()` before FFmpeg sees the bytes;
- parse and store the primary origin;
- pass `request.source_uri.c_str()` as the filename/context to `avformat_open_input()`;
- pass `av_find_input_format("hls")` as the forced input format after byte-based HLS detection;
- keep `format_raw->pb` as the primary custom memory AVIO and `AVFMT_FLAG_CUSTOM_IO`;
- keep `format_raw->protocol_whitelist = av_strdup("codec-memory-only")` so nested demuxers cannot acquire real protocols;
- install `hls_io_open` and `hls_io_close` only for HLS; direct media keeps `deny_secondary_io_open` exactly as today.

- [ ] **Step 6: Implement capture-before-read secondary opening**

`hls_io_open` must reject writes and malformed/non-HTTP URLs, verify same-origin, check resource count before capture, then call a fresh `PreparedCapture::prepare()` with the existing request capture policy and `maximum_bytes = maximum_hls_resource_bytes`.

Capture into a new heap-owned resource buffer. Before archiving/exposing it:

1. enforce `bytes.size() <= maximum_hls_resource_bytes`;
2. enforce checked `total_bytes + bytes.size() <= maximum_hls_total_bytes`;
3. if `looks_like_hls_manifest(bytes)`, run `validate_hls_manifest_security(bytes)`;
4. derive child stream ID from parent + accepted ordinal;
5. append a `StreamDescriptor` with `type=opaque`, constant `source_id`, generic payload type, and ordinal-only label;
6. append exact child `source_bytes` over the full ingest interval;
7. push both `RecordInfo`s into report vectors;
8. only now allocate/return custom read-only seekable AVIO over the stable bytes.

Store the first CODEC callback failure in `session.callback_error`; return an FFmpeg error such as `AVERROR(EACCES)`/`AVERROR(EIO)` without invoking native protocol openers. When FFmpeg returns an error, prefer `callback_error` as `profile_error`.

- [ ] **Step 7: Prove GREEN and preserve the old nested-open security regression**

Run:

```bash
./build/codec_tests --include-prefix video_hls_ingest_
./build/codec_tests --include-prefix video_ffmpeg_ingest_denies_nested_demuxer_resource_open
```

The HLS test must pass and the existing ffconcat nested-file test must remain source-only. Then run the entire unit suite and commit as:

```text
Capture HLS resources through CODEC AVIO
```

---

### Task 3: HLS failure security and live-duration boundedness

**Files:**
- Modify: `tests/test_video_hls_ingest.cpp`
- Modify: `src/video/ffmpeg_ingest.cpp`

**Interfaces:**
- Consumes Task 2 capture session.
- Produces decode-loop termination when the requested media interval is covered and stable failure behavior for unsupported graphs.

- [ ] **Step 1: Add RED failure/security cases**

Add exact tests:

```cpp
TEST(video_hls_ingest_rejects_encrypted_manifest_before_key_fetch)
TEST(video_hls_ingest_rejects_cross_origin_child_before_capture)
TEST(video_hls_ingest_rejects_file_and_crypto_child_protocols)
TEST(video_hls_ingest_honors_private_network_policy)
TEST(video_hls_ingest_enforces_per_resource_limit_with_preserved_manifest)
TEST(video_hls_ingest_enforces_total_resource_limit_with_accepted_prefix)
TEST(video_hls_ingest_enforces_resource_count_limit_with_accepted_prefix)
TEST(video_hls_ingest_preserves_captured_children_when_segment_decode_fails)
TEST(video_hls_ingest_live_playlist_stops_at_requested_media_duration)
```

For encrypted HLS, register `/secret.key` with a request counter and require zero requests to it. For cross-origin, use the same loopback server but a different effective port in the playlist URL and require zero requests on the second fixture. For forbidden schemes, place `file:///tmp/codec-hls-secret.ts` or `crypto:https://...` directly in the manifest and require `profile_error` with no attempted local file read.

For live boundedness, serve the same two-segment playlist without `#EXT-X-ENDLIST`, request `[0, 2s)`, give `maximum_hls_resources=8`, and require success with no manifest-refresh request beyond the initial parent capture and no third child request.

- [ ] **Step 2: Prove the live-duration RED**

Push/run these tests before changing the decode loop. Expected: security cases that rely on Task 2 policy may already pass; the live case must fail by attempting a refresh/resource beyond the requested 2-second media window or hitting a resource limit. Keep that explicit failing case as the RED evidence for the duration stop.

- [ ] **Step 3: Add media-duration stop state to frame acceptance**

Inside `decode_video_bytes()`, compute:

```cpp
const std::int64_t requested_duration_ns = request.end_ns - request.start_ns;
```

Track the first usable frame timestamp and a synthetic-frame index. Before canonicalizing/pushing a frame, calculate the frame's relative start using the stream time base, or `index * nominal_frame_duration` when timestamps are absent but frame rate is usable. If `relative_start >= requested_duration_ns`, set `duration_reached=true` and do not accept that frame.

Change the receive helper to return both error/success and `duration_reached` (for example `Result<bool>`). Break the `av_read_frame()` loop immediately when duration is reached; do not continue to refresh a live manifest. Keep `maximum_frames`, decoded-byte limits, EOF, and callback limits as independent guards.

- [ ] **Step 4: Prove all HLS security/boundedness tests GREEN**

Run `./build/codec_tests --include-prefix video_hls_ingest_` and the full sanitizer suite. Verify that every failure after primary S0 finalizes the archive, has zero S1/provenance records, and retains every secondary S0 that completed before failure. Commit as:

```text
Bound live HLS decode and resource failures
```

---

### Task 4: Versioned HLS provenance and verified-reader support

**Files:**
- Modify: `tests/test_video_state_reader.cpp`
- Modify: `tests/test_video_hls_ingest.cpp`
- Modify: `src/video/frame_state_reader.cpp`
- Modify: `src/video/ffmpeg_ingest.cpp`

**Interfaces:**
- Produces a second accepted exact process contract while preserving the existing direct contract unchanged.

- [ ] **Step 1: Write manual-archive HLS reader RED tests**

In `tests/test_video_state_reader.cpp`, add helper:

```cpp
codec::ProvenanceProcess hls_video_process() {
  return codec::ProvenanceProcess{
      .operation = "codec.video.raw-frame.canonicalize.hls",
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 500,
      .details_type = "application/vnd.codec.video.hls-canonicalization.v1",
      .details = {std::byte{0x01}},
  };
}
```

Build one archive with parent video stream/source, one opaque child descriptor/source, one canonical VFR1 subject, and ordered provenance `[parent_source, child_source]`. Add:

```cpp
TEST(video_state_reader_accepts_hls_resource_frontier)
TEST(video_state_reader_rejects_hls_missing_primary_input)
TEST(video_state_reader_rejects_hls_child_without_matching_descriptor)
TEST(video_state_reader_rejects_hls_child_wrong_source_id_or_stream_type)
TEST(video_state_reader_rejects_hls_duplicate_or_non_source_child)
TEST(video_state_reader_keeps_direct_provenance_rules_unchanged)
```

The first must fail on current reader because cross-stream input is rejected.

- [ ] **Step 2: Prove reader RED**

Run `./build/codec_tests --include-prefix video_state_reader_accepts_hls_` and confirm `archive_corrupt` due the old same-stream-only lineage rule.

- [ ] **Step 3: Refactor reader process matching without weakening direct validation**

Replace the single boolean process matcher with an internal enum:

```cpp
enum class VideoProvenanceContract { direct, hls };
Result<VideoProvenanceContract> classify_video_process(
    const ProvenanceProcess& process);
```

For `direct`, execute the current validation byte-for-byte: every source record is same-stream `source_bytes` and overlaps the state interval.

For `hls`:

1. require at least two inputs;
2. require input 0 to resolve to same-stream parent `source_bytes` overlapping the state;
3. for inputs 1..N require `source_bytes`, child stream != subject stream, and overlap;
4. load `archive.streams()` once and require exactly one descriptor for each child stream with `type=opaque` and `source_id="codec.video.hls-resource"`;
5. reject duplicate child record links, duplicate child stream snapshots within one provenance frontier, self-reference, dangling links, and wrong record types.

Do not accept any third process contract.

- [ ] **Step 4: Emit HLS provenance from actual ingest**

During HLS decode, each accepted frame candidate must snapshot the current secondary-source frontier as `std::vector<RecordInfo>` at frame acceptance time. Store that frontier with the candidate so later HLS captures are not incorrectly attributed backward.

After complete decode/timeline mapping succeeds, append VFR1 S1 records. For each HLS state create provenance inputs `[primary_source] + candidate.secondary_frontier` and use the HLS process contract. Direct media retains the existing one-input/direct process exactly.

- [ ] **Step 5: Prove actual HLS ingest verifies through the public reader**

Extend the Task 2 success test to call `query_verified_raw_video_frames()` and require the expected frame count; each returned frame must have the parent manifest as `source_records.front()` and at least one opaque HLS resource record after it. Require the final frame frontier to include both segment source records.

Run all video reader, direct FFmpeg, and HLS tests. Commit as:

```text
Verify HLS video provenance frontiers
```

---

### Task 5: CLI surface, package compatibility, documentation, and final gate

**Files:**
- Modify: `src/cli/main.cpp`
- Modify: `tests/video_cli_integration.sh`
- Modify: `tests/package_consumer/video_ffmpeg.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `AI_WORKSHEET.md`
- Modify: `tests/ai_contract.cmake` only to require the new HLS design reference/claim; do not weaken existing forbidden foundation dependencies.
- Modify: roadmap issue #10 after exact-head CI evidence exists.

**Interfaces:**
- CLI adds the three explicit HLS limits and two JSON counters while retaining current required arguments and exit semantics.

- [ ] **Step 1: Write CLI RED checks before parser changes**

In `tests/video_cli_integration.sh`, require help to contain:

```text
--maximum-hls-resources
--maximum-hls-resource-bytes
--maximum-hls-total-bytes
```

For each option, pass zero and require status 2 before archive creation. Existing MP4 probe output must now also include:

```json
"secondary_sources":0,"secondary_source_bytes":0
```

Push/run this CLI-only RED. It must fail only because the new flags/counters are not implemented.

- [ ] **Step 2: Implement CLI parsing and output**

Defaults must exactly match the public request defaults. Reuse `parse_decimal()`. Reject zero, `size_t` overflow for the resource count, and integer overflow for byte limits. Pass parsed values into `FfmpegVideoIngestRequest`.

Compute secondary byte output with checked accumulation over `report->secondary_sources`; an impossible overflow is an `internal` error because production capture has already enforced a smaller aggregate bound. Emit counters before `state_exact` in the existing JSON object. Do not add an allow-private/cross-origin option.

- [ ] **Step 3: Update installed consumer compile contract**

In `tests/package_consumer/video_ffmpeg.cpp`, instantiate `FfmpegVideoIngestRequest` and assert at compile/run time that the defaults are non-zero; construct an empty `FfmpegVideoIngestReport` and access both new vectors. The existing invalid empty request must still return `invalid_argument` in ON and OFF package builds.

- [ ] **Step 4: Update truth-bearing docs and AI contract**

README must state:

- direct media still decodes only captured bytes;
- same-origin unencrypted HTTP/HTTPS HLS is now supported through CODEC-owned secondary capture;
- each child is exact S0 on an opaque child stream before FFmpeg reads it;
- encrypted/cross-origin/private-denied/other-protocol HLS fails closed;
- live duration is a decoded-media-timeline bound, not a wall-clock recording guarantee;
- no DASH or arbitrary FFmpeg networking claim.

CHANGELOG gets a new Unreleased bullet; do not rewrite the historical bridge bullet except to avoid a current-tree contradiction if necessary. AI_WORKSHEET records this branch/base, S0/S1 scope, RED/GREEN evidence, exact HLS security claim, and pending final head. AI contract should require the HLS design link and stable HLS process string while continuing to prohibit FFmpeg types from the dependency-free `video.hpp`/frame-state foundation.

- [ ] **Step 5: Run the complete exact-head verification matrix**

On the final documentation/code head require:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel
ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --test-dir build-san --output-on-failure

cmake -S . -B build-no-ffmpeg -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCODEC_WARNINGS_AS_ERRORS=ON -DCODEC_ENABLE_FFMPEG_VIDEO=OFF
cmake --build build-no-ffmpeg --parallel
ctest --test-dir build-no-ffmpeg --output-on-failure
```

Then require GitHub PR CI on that exact SHA: `build (gcc)`, `build (clang)`, `sanitizers`, and `ffmpeg-disabled`, including install/package-consumer steps.

- [ ] **Step 6: Review, merge with exact-head guard, and verify main**

Audit changed-file patches for: no raw requested child URL persistence, no native FFmpeg protocol fallback, no direct-media regression, no H.2 changes, and no capability overclaim. Resolve all review threads. Merge only with the exact green head SHA. Verify `main` points to the merge commit and require the push-triggered main CI run to pass before recording completion in roadmap issue #10.

- [ ] **Step 7: Manual Nova Scotia smoke test after merge**

After merged-main CI is green, run the user's representative command against:

```text
https://streaming-1.novascotiawebcams.com/live/armdale2/chunklist_w845418048.m3u8
```

with a fresh output archive and a bounded interval. Treat this as interoperability evidence only; do not make CI or completion depend on external availability. If the stream is cross-origin, encrypted, redirected, or otherwise outside the approved slice, report the exact policy failure rather than broadening security automatically.

## Plan self-review

- Spec coverage: tasks cover HLS detection/encryption, origin authorization, child S0 archive model, custom AVIO lifecycle, aggregate bounds, live termination, HLS provenance, reader validation, CLI, package compatibility, documentation, exact-head CI, merge, and external smoke test.
- Placeholder scan: no implementation step depends on an unspecified behavior or unnamed test. The fixture generation command and expected sizes are fixed; CI consumes committed base64 fixtures.
- Type consistency: `maximum_hls_resources`, `maximum_hls_resource_bytes`, `maximum_hls_total_bytes`, `secondary_descriptors`, and `secondary_sources` use the exact public names from the approved spec throughout all tasks.
- Scope: no H.2 telemetry file or branch is modified, and generic capture authorization is reused rather than changed.

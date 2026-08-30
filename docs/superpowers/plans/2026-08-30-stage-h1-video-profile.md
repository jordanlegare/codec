# Stage H.1 Video Stream Profile Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an installed, dependency-free Video Stream Profile that deterministically encodes bounded profile descriptors and raw-frame S1 state and retrieves only frames with exact verified S0 provenance.

**Architecture:** Keep all video-only types, record-code constants, validation, and parsing under `codec::profiles::video`. Store VPD1/VFR1 through existing raw record APIs and validate S1 lineage through existing provenance/query APIs, leaving the generic archive envelope, `RecordType`, CLI, C ABI, audio, transport, and distributed code unchanged.

**Tech Stack:** C++20, CMake 3.20+, existing `codec::Result` and CODA archive APIs, existing lightweight test harness, GCC/Clang, ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-30-stage-h1-video-profile-design.md`

## Global Constraints

- Stage G is deferred, not complete; H.1 activates Stage H.
- Add no FFmpeg/GStreamer/media dependency, codec/container parser, CLI, C ABI, model, inference, export, playback, performance, or scale claim.
- Keep encoded/container/source bytes S0 and require exact `TruthClass::state_exact` provenance before returning a verified VFR1 S1 frame.
- Add no video fields to generic structures and no video values to generic `RecordType`.
- Use profile-owned record codes `0x0100` for VPD1 and `0x0101` for VFR1; never reuse retired codes 20 or 21.
- VPD1 is exactly 36 bytes; VFR1 uses a 32-bit descriptor length and 64-bit pixel length, all big-endian.
- All parsers reject unsupported versions/enums, non-zero reserved bytes, impossible geometry, overflow, truncation, and trailing bytes before unsafe allocation.
- Generic archive verification, repair, unknown-type preservation, CLI behavior, and C ABI must remain unchanged.

---

### Task 1: Versioned video descriptor and raw-frame encodings

**Files:**
- Create: `include/codec/profiles/video.hpp`
- Create: `src/video/frame_state.cpp`
- Create: `tests/test_video_profile.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `codec::RecordTypeCode`, `codec::Result<T>`, and `codec::ErrorCode`.
- Produces:
  - `video_profile_descriptor_record_type == 0x0100`
  - `raw_video_frame_state_record_type == 0x0101`
  - `PixelLayout`, `ColorRange`, `ColorPrimaries`, `TransferCharacteristics`, `MatrixCoefficients`
  - `VideoDecodeLimits`, `VideoProfileDescriptor`, `RawVideoFrameState`
  - `encode_video_profile_descriptor()`, `decode_video_profile_descriptor()`
  - `encode_raw_video_frame_state()`, `decode_raw_video_frame_state()`

- [ ] **Step 1: Add the public type declarations**

Create `include/codec/profiles/video.hpp` with the exact namespace and signatures:

```cpp
#pragma once

#include <codec/archive.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace codec::profiles::video {

inline constexpr RecordTypeCode video_profile_descriptor_record_type = 0x0100;
inline constexpr RecordTypeCode raw_video_frame_state_record_type = 0x0101;

enum class PixelLayout : std::uint8_t {
  gray8 = 1,
  rgb24 = 2,
  rgba32 = 3,
  yuv420p8 = 4,
};

enum class ColorRange : std::uint8_t {
  unspecified = 0,
  limited = 1,
  full = 2,
};

enum class ColorPrimaries : std::uint8_t {
  unspecified = 0,
  bt709 = 1,
  bt2020 = 2,
};

enum class TransferCharacteristics : std::uint8_t {
  unspecified = 0,
  linear = 1,
  srgb = 2,
  bt709 = 3,
  pq = 4,
  hlg = 5,
};

enum class MatrixCoefficients : std::uint8_t {
  unspecified = 0,
  identity = 1,
  bt709 = 2,
  bt2020_ncl = 3,
};

struct VideoDecodeLimits {
  std::uint32_t maximum_width{16384};
  std::uint32_t maximum_height{16384};
  std::uint64_t maximum_pixels{268435456};
  std::uint64_t maximum_payload_bytes{1024ULL * 1024ULL * 1024ULL};
};

struct VideoProfileDescriptor {
  std::uint32_t coded_width{};
  std::uint32_t coded_height{};
  PixelLayout pixel_layout{PixelLayout::gray8};
  std::uint32_t sample_aspect_ratio_numerator{1};
  std::uint32_t sample_aspect_ratio_denominator{1};
  std::uint32_t nominal_frame_rate_numerator{};
  std::uint32_t nominal_frame_rate_denominator{1};
  ColorRange color_range{ColorRange::unspecified};
  ColorPrimaries color_primaries{ColorPrimaries::unspecified};
  TransferCharacteristics transfer{TransferCharacteristics::unspecified};
  MatrixCoefficients matrix{MatrixCoefficients::unspecified};
  auto operator<=>(const VideoProfileDescriptor&) const = default;
};

struct RawVideoFrameState {
  VideoProfileDescriptor descriptor;
  std::vector<std::byte> pixels;
  bool operator==(const RawVideoFrameState&) const = default;
};

Result<std::vector<std::byte>> encode_video_profile_descriptor(
    const VideoProfileDescriptor& descriptor);
Result<VideoProfileDescriptor> decode_video_profile_descriptor(
    std::span<const std::byte> payload, VideoDecodeLimits limits = {});
Result<std::vector<std::byte>> encode_raw_video_frame_state(
    const RawVideoFrameState& frame);
Result<RawVideoFrameState> decode_raw_video_frame_state(
    std::span<const std::byte> payload, VideoDecodeLimits limits = {});

}  // namespace codec::profiles::video
```

Include `<compare>` for the defaulted three-way comparison.

- [ ] **Step 2: Write failing deterministic and exactness tests**

Create `tests/test_video_profile.cpp`. Define a 2x2 RGB24 descriptor and a 12-byte pixel fixture. Add tests named:

```cpp
TEST(video_profile_record_codes_are_profile_owned_and_stable)
TEST(video_descriptor_encoding_matches_vpd1_golden_bytes)
TEST(video_frame_encoding_matches_vfr1_golden_bytes)
TEST(video_frame_round_trips_all_supported_layouts_exactly)
TEST(video_profile_encoding_rejects_invalid_geometry)
TEST(video_profile_decoding_rejects_malformed_and_over_limit_payloads)
```

The VPD1 golden fixture for a 2x2 RGB24, 1:1 sample aspect, 30/1 frame rate, full-range BT.709 descriptor must be exactly 36 bytes and begin:

```cpp
{
  std::byte{'V'}, std::byte{'P'}, std::byte{'D'}, std::byte{'1'},
  std::byte{0x01},  // version
  std::byte{0x02},  // RGB24
  std::byte{0x02},  // full range
  std::byte{0x01},  // BT.709 primaries
  std::byte{0x03},  // BT.709 transfer
  std::byte{0x02},  // BT.709 matrix
  std::byte{0x00}, std::byte{0x00},
  std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x02},
  std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x02},
  std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
  std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
  std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x1e},
  std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
}
```

The VFR1 golden fixture is a 20-byte header, the VPD1 fixture, then the 12 exact pixels. Its header is:

```cpp
{
  std::byte{'V'}, std::byte{'F'}, std::byte{'R'}, std::byte{'1'},
  std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
  std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x24},
  std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
  std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0c},
}
```

For layout round trips, use 2x2 Gray8 (4 bytes), RGB24 (12), RGBA32 (16), and YUV420P8 (6). Verify encode/decode equality and encode-decode-encode byte equality.

Malformed cases must mutate magic, version, enums, reserved bytes, dimensions, denominators, descriptor length, pixel length, truncation, and add trailing data. Verify errors are non-retryable and use `decode` for direct profile decode; the verified reader converts malformed archived payloads to `archive_corrupt` in Task 2.

- [ ] **Step 3: Wire the new test and source into CMake**

Add `src/video/frame_state.cpp` to `codec_core` and `tests/test_video_profile.cpp` to `codec_tests`. Do not change link libraries.

- [ ] **Step 4: Run the focused build to prove RED**

Run:

```bash
cmake -S . -B build-h1 -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build-h1 --target codec_tests --parallel
```

Expected: compilation or link failure only because the declared video encoding functions are not implemented.

- [ ] **Step 5: Implement checked big-endian VPD1/VFR1 encoding**

In `src/video/frame_state.cpp`, define constants:

```cpp
constexpr std::size_t kVpd1Size = 36;
constexpr std::size_t kVfr1HeaderSize = 20;
```

Implement private `put_u32_be`, `put_u64_be`, `get_u32_be`, and `get_u64_be` helpers.

Implement a shared validator that:

- rejects zero dimensions and zero rational denominators;
- accepts only the declared enum values;
- requires even width and height for YUV420P8;
- rejects zero width/height decode limits;
- checks `width <= maximum_width`, `height <= maximum_height`, and checked `width * height <= maximum_pixels`;
- computes canonical bytes using checked multiplication/addition;
- rejects canonical bytes greater than `maximum_payload_bytes`.

Encoding uses the default limits and returns `invalid_argument` for invalid values or `resource_exhausted` for arithmetic/allocation bounds. Decoding returns `decode` for malformed bytes and `resource_exhausted` for configured limits.

Encode VPD1 exactly as the 36-byte golden fixture. Encode VFR1 as its 20-byte header, one canonical VPD1, and exact pixels. Require the caller's pixel count to equal the layout formula before allocating output.

- [ ] **Step 6: Implement exact decoding**

`decode_video_profile_descriptor()` must require exactly 36 bytes, exact magic/version, zero reserved bytes, and valid fields.

`decode_raw_video_frame_state()` must:

1. reject payloads shorter than 20 bytes;
2. verify magic/version/reserved bytes;
3. read descriptor length as u32 and pixel length as u64;
4. require descriptor length 36;
5. check both lengths against `size_t`, caller limits, and the exact total payload size;
6. decode the embedded descriptor with the same limits;
7. require pixel length to equal its canonical layout formula;
8. copy only after all checks pass.

- [ ] **Step 7: Run focused tests to prove GREEN**

Run:

```bash
cmake --build build-h1 --target codec_tests --parallel
./build-h1/codec_tests --include-prefix video_profile_
```

Expected: every `video_profile_` test passes.

- [ ] **Step 8: Commit the self-contained codec**

```bash
git add CMakeLists.txt include/codec/profiles/video.hpp src/video/frame_state.cpp tests/test_video_profile.cpp
git commit -m "feat: add deterministic video frame state"
```

### Task 2: Provenance-verified raw-frame reader

**Files:**
- Create: `include/codec/profiles/video_state_reader.hpp`
- Create: `src/video/frame_state_reader.cpp`
- Create: `tests/test_video_state_reader.cpp`
- Modify: `include/codec/profiles/video.hpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 VFR1 decoder and type code; `CodaArchive::verify`, `records`, `query_provenance`, and `read_payload`.
- Produces: `VideoFrameQuery`, `VerifiedRawVideoFrame`, and `query_verified_raw_video_frames()`.

- [ ] **Step 1: Declare the reader API**

Create `include/codec/profiles/video_state_reader.hpp`:

```cpp
#pragma once

#include <codec/archive.hpp>
#include <codec/profiles/video.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace codec::profiles::video {

struct VideoFrameQuery {
  std::optional<StreamId> stream;
  std::optional<RecordTimeRange> time;
  std::size_t maximum_results{1024};
  std::uint64_t maximum_encoded_bytes{1024ULL * 1024ULL * 1024ULL};
  VideoDecodeLimits decode_limits{};
};

struct VerifiedRawVideoFrame {
  RawVideoFrameState state;
  RecordInfo state_record;
  std::vector<RecordInfo> source_records;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedRawVideoFrame>> query_verified_raw_video_frames(
    const CodaArchive& archive, const VideoFrameQuery& query = {});

}  // namespace codec::profiles::video
```

Include it from `include/codec/profiles/video.hpp` only after moving Task 1 declarations into a dependency-safe base header is evaluated. To avoid a cyclic include, the implementation choice is: keep all Task 1 declarations in `video.hpp`, let `video_state_reader.hpp` include `video.hpp`, and do not include the reader back from `video.hpp`. Installed users include the reader explicitly. This is the one approved refinement from the design's umbrella wording.

- [ ] **Step 2: Write failing archive/provenance tests**

Create `tests/test_video_state_reader.cpp` with helpers that:

1. create a temporary CODA archive;
2. append a `StreamDescriptor{.type = StreamType::video}`;
3. append one `source_bytes` S0 record;
4. append one VFR1 record using `append_raw(raw_video_frame_state_record_type, ...)`;
5. attach provenance with `TruthClass::state_exact`, operation `codec.video.raw-frame.canonicalize`, implementation `codec.video`, version `1`, and details type `application/vnd.codec.video.canonicalization.v1`;
6. finalize and reopen.

Add tests:

```cpp
TEST(video_state_reader_returns_exact_verified_s1)
TEST(video_state_reader_ignores_unprovenanced_vfr1)
TEST(video_state_reader_rejects_wrong_truth_or_process_contract)
TEST(video_state_reader_rejects_dangling_self_or_cross_stream_inputs)
TEST(video_state_reader_accepts_multiple_exact_s0_inputs)
TEST(video_state_reader_rejects_malformed_vfr1_as_archive_corrupt)
TEST(video_state_reader_enforces_result_and_byte_limits)
TEST(video_state_reader_preserves_unknown_future_profile_records)
```

The happy path verifies exact pixels, state record type code `0x0101`, all source links, and provenance. The unknown-future test writes `0x0102`, verifies/extracts it raw, repairs the archive, and verifies/extracts the exact same bytes from the repaired archive.

- [ ] **Step 3: Wire the reader source and tests into CMake**

Add `src/video/frame_state_reader.cpp` and `tests/test_video_state_reader.cpp`; add no dependency.

- [ ] **Step 4: Run the focused build to prove RED**

Run:

```bash
cmake --build build-h1 --target codec_tests --parallel
```

Expected: link failure for `query_verified_raw_video_frames()`.

- [ ] **Step 5: Implement exact link resolution and preflight**

In `src/video/frame_state_reader.cpp`:

- reject zero query limits and invalid zero decode limits with `invalid_argument`;
- require a verified finalized archive;
- call `query_provenance` with subject truth `state_exact`, exact VFR1 raw type, optional stream, and optional time;
- enforce `maximum_results` before reading payloads;
- resolve every subject/input by exact stream, type code, sequence, and SHA-256 against `archive.records()`;
- require subject type code `0x0101`;
- require at least one input;
- reject a self-link;
- require every input to be `RecordType::source_bytes`, share the subject stream, and overlap the subject interval;
- require exact process strings from Step 2;
- preflight aggregate state payload bytes using subtraction-safe arithmetic.

Do not require exactly one source input.

- [ ] **Step 6: Decode after all lineage checks**

Read and decode each state payload only after all candidates and aggregate limits pass. Map any direct VFR1 `decode` error to a new non-retryable `archive_corrupt` error whose message begins `verified video frame payload is invalid:`. Preserve `resource_exhausted` from configured decode limits.

Return candidates in archive/provenance order.

- [ ] **Step 7: Run focused and generic compatibility tests**

Run:

```bash
cmake --build build-h1 --target codec_tests --parallel
./build-h1/codec_tests --include-prefix video_state_reader_
./build-h1/codec_tests --include-prefix archive_
./build-h1/codec_tests --include-prefix audio_
```

Expected: all selected tests pass.

- [ ] **Step 8: Commit the reader boundary**

```bash
git add CMakeLists.txt include/codec/profiles/video_state_reader.hpp src/video/frame_state_reader.cpp tests/test_video_state_reader.cpp
git commit -m "feat: verify archived video frame state"
```

### Task 3: Installed-package proof and truthful manifests

**Files:**
- Modify: `tests/package_consumer/main.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `AI_WORKSHEET.md`
- Modify: `tests/ai_contract.cmake`

**Interfaces:**
- Consumes: installed Task 1/2 headers and library.
- Produces: external-consumer proof plus current Stage H/H.1 documentation.

- [ ] **Step 1: Add the installed-package consumer proof**

In `tests/package_consumer/main.cpp`, include:

```cpp
#include <codec/profiles/video.hpp>
#include <codec/profiles/video_state_reader.hpp>
```

Add one function that:

- constructs a 2x2 RGB24 `RawVideoFrameState`;
- encodes and decodes it and checks exact equality;
- creates an archive in the consumer's temporary path;
- writes a video stream descriptor, one S0 `source_bytes`, one raw VFR1 record, and exact S1 provenance;
- finalizes/reopens;
- calls `query_verified_raw_video_frames()`;
- returns failure unless one exact frame is returned.

Call that function from the existing consumer entry point without removing any existing package composition proof.

- [ ] **Step 2: Run installed-package proof before documentation**

Run:

```bash
cmake --build build-h1 --parallel
cmake --install build-h1 --prefix /tmp/codec-h1-install
cmake -S tests/package_consumer -B /tmp/codec-h1-consumer -G Ninja   -DCMAKE_PREFIX_PATH=/tmp/codec-h1-install
cmake --build /tmp/codec-h1-consumer --parallel
/tmp/codec-h1-consumer/codec_package_consumer
```

Expected: exit 0.

- [ ] **Step 3: Update README current capability truth**

Add a `Video Stream Profile H.1` subsection under C++ library capabilities. State only:

- dependency-free VPD1 descriptor and VFR1 canonical raw-frame state;
- Gray8, RGB24, RGBA32, and YUV420P8 exact layouts;
- exact S1 provenance-verified reader;
- profile-local raw record codes and unknown-version raw preservation;
- no FFmpeg, demux/decode/playback/export/model/inference/CLI capability.

Update the release-scope/current-stage prose so Stage G is explicitly deferred by project direction and Stage H.1 is active on the work branch; do not describe H.1 as merged until the final merge.

- [ ] **Step 4: Add an Unreleased changelog entry**

Under `## Unreleased`, add one bullet describing the exact Video Profile API and its non-claims. Preserve all historical release entries verbatim.

- [ ] **Step 5: Replace the worksheet active record**

Set:

```yaml
task: Add the Stage H.1 dependency-free Video Stream Profile foundation.
base_ref: main
base_head_sha: 2fa8da9fab514d77aa525be0cc6ed940e6569d67
work_branch: codex/stage-h1-video-profile
current_version: 0.3.0
active_roadmap_stage: H.1 — Video Stream Profile foundation; Stage G explicitly deferred and not claimed complete.
scope: other-profile
touched_truth_classes: [S0, S1]
change_class: profile_specific_behavior
new_capability_claim: Deterministic bounded video descriptors and raw-frame S1 records can be encoded, archived under profile-owned raw codes, and returned only with exact verified S0 provenance.
```

Record the exact BEFORE/AFTER and proof contract from the spec. Do not pre-mark verification or CI as passed.

- [ ] **Step 6: Extend the AI contract narrowly**

Update `tests/ai_contract.cmake` only where needed to assert:

- Stage H.1 current docs mention the explicit video non-claims;
- no retired watermark files/symbols are reintroduced;
- no `ffmpeg`, `libavcodec`, or GStreamer dependency appears in `CMakeLists.txt`;
- record tombstones 20/21 and retained numeric errors remain unchanged.

- [ ] **Step 7: Run documentation and contract checks**

Run:

```bash
git diff --check
cmake -DCODEC_SOURCE_DIR="$PWD" -P tests/ai_contract.cmake
./build-h1/codec_tests --include-prefix video_
ctest --test-dir build-h1 --output-on-failure
```

Expected: no whitespace errors; AI contract passes; all video and full tests pass.

- [ ] **Step 8: Commit packaging and manifests**

```bash
git add tests/package_consumer/main.cpp README.md CHANGELOG.md AI_WORKSHEET.md tests/ai_contract.cmake
git commit -m "docs: activate Stage H.1 video profile"
```

### Task 4: Exact-head verification, GitHub evidence, and PR

**Files:**
- Modify only if verification reveals a scoped defect; otherwise no source changes.

**Interfaces:**
- Consumes: complete H.1 branch.
- Produces: exact local evidence, roadmap issue entry, focused PR, and exact-head CI evidence.

- [ ] **Step 1: Run clean Release verification**

Run:

```bash
cmake -S . -B build-h1-release -G Ninja   -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build-h1-release --parallel
ctest --test-dir build-h1-release --output-on-failure
./build-h1-release/codec capabilities
```

Expected: configure/build pass; all CTest targets pass; capability JSON remains truthful and does not claim video decoder/model/CLI support.

- [ ] **Step 2: Run sanitizer verification**

Run:

```bash
cmake -S . -B build-h1-san -G Ninja   -DCMAKE_BUILD_TYPE=Debug -DCODEC_WARNINGS_AS_ERRORS=ON   -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-h1-san --parallel
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1   ctest --test-dir build-h1-san --output-on-failure
```

Expected: configure/build and all tests pass. Record the local LeakSanitizer runner limitation separately; GitHub CI must retain its normal sanitizer policy.

- [ ] **Step 3: Run final diff and claim audit**

Run:

```bash
git diff --check main...HEAD
git diff --stat main...HEAD
git diff --name-only main...HEAD
rg -n "FFmpeg|ffmpeg|GStreamer|decoder|playback|model|inference|Stage G|Stage H\.1"   README.md CHANGELOG.md AI_WORKSHEET.md   docs/superpowers/specs/2026-08-30-stage-h1-video-profile-design.md
```

Expected changed implementation scope: video headers/source/tests, CMake, installed consumer, current docs/worksheet, design, and plan only. Every decoder/model/integration mention is an explicit non-claim.

- [ ] **Step 4: Record planned work on issue 10**

Add one issue comment headed `Planned work — Stage H.1 Video Stream Profile foundation` containing:

- base and branch SHAs;
- explicit Stage G deferral with no completed trust claim;
- exact H.1 scope and non-goals;
- S0/S1 proof contract;
- spec and plan paths;
- verification gates;
- next dependency H.2 telemetry after H.1 merge.

Do not mark H.1 complete before exact-head CI and merge.

- [ ] **Step 5: Publish the branch and open a PR**

Use the GitHub integration to push `codex/stage-h1-video-profile` and open a ready PR against `main` titled:

```text
Add Stage H.1 video profile foundation
```

The PR body must summarize profile-local schemas, verified provenance, compatibility/non-claims, local verification, and `Closes` no issue because issue 10 is a continuing execution log.

- [ ] **Step 6: Verify CI on the exact PR head**

Fetch the PR head SHA and its workflow run. Require GCC, Clang, full tests/install/package consumer, and sanitizer jobs to complete successfully on that exact SHA. If the head changes, discard the older evidence and repeat.

- [ ] **Step 7: Review and merge only the verified exact head**

Audit changed files and review threads. Merge only when all worksheet merge gates are true and the PR remains mergeable at the exact green head.

- [ ] **Step 8: Record completion**

After merge, add a second issue 10 comment headed `Completed — Stage H.1 Video Stream Profile foundation` with:

- base, tested head, CI run, PR, and merge SHAs;
- exact capability delta and non-claims;
- local and CI proof;
- compatibility evidence;
- Stage H.2 telemetry as the next dependency.

Update no source file after merge solely to claim completion.

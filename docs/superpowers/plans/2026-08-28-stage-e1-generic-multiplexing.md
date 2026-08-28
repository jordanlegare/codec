# Stage E.1 Generic Multiplexing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded deterministic `CMX1` byte-stream framing layer that interleaves many logical CODEC streams over one physical stream while preserving each frame's logical metadata and opaque payload exactly.

**Architecture:** `encode_multiplex_frame()` creates one fixed-header versioned frame; concatenating encoded frames is the physical multiplex stream. `MultiplexDecoder` incrementally buffers arbitrary physical chunks, validates sizes and SHA-256 integrity, emits complete logical frames in physical order with bounded output/backpressure, and never interprets sequence gaps or truth semantics.

**Tech Stack:** C++20, existing `codec::StreamId` / `StreamEpoch` / `StreamClock`, existing SHA-256 implementation, CMake/Ninja, custom CODEC test harness, GCC/Clang, ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-e1-generic-multiplexing-design.md`

## Global Constraints

- `CMX1` version 1 has a fixed 164-byte little-endian header.
- SHA-256 covers header bytes `[0, 132)` followed by the payload; the stored digest is excluded.
- Multiplex transport never assigns S0/S1/D truth and never changes CODA layout/version.
- Decoder preserves `StreamId`, sequence, `StreamEpoch`, full `StreamClock`, interval, payload, and physical frame order exactly.
- Decoder performs no gap inference, per-stream continuity enforcement, reordering, retry, recovery, FEC, resynchronization, network I/O, archive write, CLI/C ABI change, or performance claim.
- Global sanitizer leak detection and the existing isolated ONNX Runtime suppression remain unchanged.

---

### Task 1: Establish the E.1 API contract in RED

**Files:**
- Create: `tests/test_transport_multiplex.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing `<codec/stream.hpp>` and `<codec/result.hpp>` concepts.
- Produces: compile-time expectations for `<codec/transport.hpp>`, `MultiplexFrame`, `MultiplexLimits`, `encode_multiplex_frame`, and `MultiplexDecoder`.

- [ ] **Step 1: Create the tests-first source with the absent API**

Start `tests/test_transport_multiplex.cpp` with:

```cpp
#include "test.hpp"

#include <codec/transport.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

codec::MultiplexFrame frame_for(std::string_view name,
                                std::uint64_t sequence,
                                std::byte value) {
  codec::MultiplexFrame frame;
  frame.stream = codec::derive_stream_id(name);
  frame.sequence = sequence;
  frame.epoch = {.connection = 3, .format = 2};
  frame.clock = {
      .monotonic_ns = 1000 + static_cast<std::int64_t>(sequence),
      .observed_utc_ns = 2000 + static_cast<std::int64_t>(sequence),
      .observed_utc_uncertainty_ns = 7,
      .source_timestamp = static_cast<std::int64_t>(sequence * 480),
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 48000,
  };
  frame.start_ns = static_cast<std::int64_t>(sequence * 10);
  frame.end_ns = frame.start_ns + 10;
  frame.payload = {value, std::byte{0}, std::byte{0xff}};
  return frame;
}

}  // namespace

TEST(transport_multiplex_round_trips_one_generic_frame) {
  auto original = frame_for("telemetry/A", 7, std::byte{0x42});
  auto encoded = codec::encode_multiplex_frame(original);
  EXPECT_TRUE(encoded);

  codec::MultiplexDecoder decoder;
  auto decoded = decoder.push(*encoded);
  EXPECT_TRUE(decoded);
  EXPECT_EQ(decoded->size(), std::size_t{1});
  EXPECT_EQ(decoded->front().stream, original.stream);
  EXPECT_EQ(decoded->front().sequence, original.sequence);
  EXPECT_EQ(decoded->front().epoch, original.epoch);
  EXPECT_EQ(decoded->front().clock.monotonic_ns, original.clock.monotonic_ns);
  EXPECT_EQ(decoded->front().clock.observed_utc_ns,
            original.clock.observed_utc_ns);
  EXPECT_EQ(decoded->front().clock.observed_utc_uncertainty_ns,
            original.clock.observed_utc_uncertainty_ns);
  EXPECT_EQ(decoded->front().clock.source_timestamp,
            original.clock.source_timestamp);
  EXPECT_EQ(decoded->front().clock.source_timebase_numerator,
            original.clock.source_timebase_numerator);
  EXPECT_EQ(decoded->front().clock.source_timebase_denominator,
            original.clock.source_timebase_denominator);
  EXPECT_EQ(decoded->front().start_ns, original.start_ns);
  EXPECT_EQ(decoded->front().end_ns, original.end_ns);
  EXPECT_EQ(decoded->front().payload, original.payload);
  EXPECT_TRUE(decoder.finish());
}
```

Include `<string_view>` explicitly.

- [ ] **Step 2: Register the new test translation unit**

Add `tests/test_transport_multiplex.cpp` to the `codec_tests` source list in `CMakeLists.txt`. Do **not** add any production header/source yet.

- [ ] **Step 3: Run the RED build**

Run:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
```

Expected: existing production sources compile; `tests/test_transport_multiplex.cpp` fails because `<codec/transport.hpp>` does not exist. No unrelated failure is acceptable as the RED proof.

- [ ] **Step 4: Commit the accepted RED**

```bash
git add CMakeLists.txt tests/test_transport_multiplex.cpp
git commit -m "Test Stage E.1 multiplexing contract"
```

---

### Task 2: Implement the fixed CMX1 frame codec

**Files:**
- Create: `include/codec/transport.hpp`
- Create: `src/transport/multiplex.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/test_transport_multiplex.cpp`

**Interfaces:**
- Consumes: `StreamId`, `StreamEpoch`, `StreamClock`, `Result`, `sha256`.
- Produces:

```cpp
inline constexpr std::uint16_t multiplex_frame_version = 1;

struct MultiplexFrame {
  StreamId stream{};
  std::uint64_t sequence{};
  StreamEpoch epoch{};
  StreamClock clock{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::vector<std::byte> payload;
};

struct MultiplexLimits {
  std::uint64_t maximum_payload_bytes{16ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_buffered_bytes{32ULL * 1024ULL * 1024ULL};
  std::size_t maximum_frames_per_push{4096};
};

Result<std::vector<std::byte>> encode_multiplex_frame(
    const MultiplexFrame&, MultiplexLimits = {});
```

The header also declares the move-only `MultiplexDecoder` pimpl exactly as the spec.

- [ ] **Step 1: Expand tests for deterministic header and encoder validation**

Add tests that assert:

```cpp
TEST(transport_multiplex_encoding_is_deterministic_and_versioned) {
  const auto frame = frame_for("sensor/A", 9, std::byte{0x11});
  const auto first = codec::encode_multiplex_frame(frame);
  const auto second = codec::encode_multiplex_frame(frame);
  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
  EXPECT_EQ(*first, *second);
  EXPECT_TRUE(first->size() >= std::size_t{164});
  EXPECT_EQ((*first)[0], std::byte{'C'});
  EXPECT_EQ((*first)[1], std::byte{'M'});
  EXPECT_EQ((*first)[2], std::byte{'X'});
  EXPECT_EQ((*first)[3], std::byte{'1'});
}

TEST(transport_multiplex_encoder_rejects_invalid_metadata_and_bounds) {
  auto frame = frame_for("sensor/A", 0, std::byte{1});
  frame.end_ns = frame.start_ns - 1;
  auto inverted = codec::encode_multiplex_frame(frame);
  EXPECT_FALSE(inverted);
  EXPECT_EQ(inverted.error().code, codec::ErrorCode::invalid_argument);

  frame = frame_for("sensor/A", 0, std::byte{1});
  frame.clock.source_timebase_denominator = 0;
  auto bad_timebase = codec::encode_multiplex_frame(frame);
  EXPECT_FALSE(bad_timebase);
  EXPECT_EQ(bad_timebase.error().code, codec::ErrorCode::invalid_argument);

  codec::MultiplexLimits tiny;
  tiny.maximum_payload_bytes = 2;
  tiny.maximum_buffered_bytes = 164 + 2;
  tiny.maximum_frames_per_push = 1;
  auto oversized = codec::encode_multiplex_frame(frame_for(
      "sensor/A", 0, std::byte{1}), tiny);
  EXPECT_FALSE(oversized);
  EXPECT_EQ(oversized.error().code, codec::ErrorCode::resource_exhausted);
}
```

Also cover zero limits and `maximum_buffered_bytes < 164 + maximum_payload_bytes` as `invalid_argument` configuration errors. Add one valid empty-payload frame.

- [ ] **Step 2: Implement `transport.hpp`**

Include `<codec/result.hpp>` and `<codec/stream.hpp>` plus standard memory/span/vector headers. Declare the public structs/functions and pimpl API exactly as the spec. No archive, audio, network, or provider header is permitted.

- [ ] **Step 3: Implement fixed-header little-endian helpers and encoder**

In `src/transport/multiplex.cpp`, use local `put_le` / `get_le` templates consistent with existing CODEC serializers. Define:

```cpp
constexpr std::size_t header_size = 164;
constexpr std::size_t hash_offset = 132;
constexpr std::array<std::byte, 4> magic{
    std::byte{'C'}, std::byte{'M'}, std::byte{'X'}, std::byte{'1'}};
```

Implement `validate_limits()` and `validate_frame_metadata()` so:

```cpp
if (limits.maximum_payload_bytes == 0 ||
    limits.maximum_buffered_bytes == 0 ||
    limits.maximum_frames_per_push == 0) {
  return fail(ErrorCode::invalid_argument,
              "multiplex limits must be non-zero");
}
if (limits.maximum_payload_bytes >
    limits.maximum_buffered_bytes - header_size) {
  return fail(ErrorCode::invalid_argument,
              "multiplex buffer limit cannot hold one permitted frame");
}
```

Guard the subtraction by first requiring `maximum_buffered_bytes >= header_size`.

Serialize fields at the exact offsets in the spec. Compute SHA-256 over a temporary byte vector containing header bytes `[0, 132)` and payload, copy the 32 digest bytes to offset 132, then append the unchanged payload. Catch `std::bad_alloc` and map it to `resource_exhausted`.

- [ ] **Step 4: Register the production source**

Add `src/transport/multiplex.cpp` to `codec_core` in `CMakeLists.txt`.

- [ ] **Step 5: Run targeted tests**

```bash
cmake --build build --parallel
./build/codec_tests --include-prefix transport_multiplex_
```

Expected: encoder tests pass; the basic decoder round-trip may still fail until Task 3 if only a minimal decoder exists. If needed, implement the smallest complete single-frame decoder path now to make the original Task 1 test pass, leaving incremental/backpressure behavior for Task 3.

- [ ] **Step 6: Commit the fixed frame codec**

```bash
git add CMakeLists.txt include/codec/transport.hpp src/transport/multiplex.cpp tests/test_transport_multiplex.cpp
git commit -m "Add Stage E.1 multiplex frame codec"
```

---

### Task 3: Complete incremental demultiplexing, backpressure, and fail-closed behavior

**Files:**
- Modify: `src/transport/multiplex.cpp`
- Modify: `tests/test_transport_multiplex.cpp`

**Interfaces:**
- Consumes: the Task 2 public API and exact CMX1 binary layout.
- Produces: fully functional `MultiplexDecoder::push`, `finish`, and `buffered_bytes`.

- [ ] **Step 1: Add interleaving and arbitrary-chunk tests**

Create a helper that concatenates encoded frame byte vectors. Add:

```cpp
TEST(transport_multiplex_interleaves_independent_logical_streams) {
  const auto a7 = frame_for("A", 7, std::byte{0xa7});
  const auto b100 = frame_for("B", 100, std::byte{0xb1});
  const auto a9 = frame_for("A", 9, std::byte{0xa9});
  const auto c3 = frame_for("C", 3, std::byte{0xc3});
  // Deliberately skip A#8: E.1 must preserve the value, not invent a gap.
  // Encode, concatenate A7/B100/A9/C3, push once, and require four frames in
  // exactly that physical order with exact StreamIds and sequence values.
}
```

Add a one-byte-at-a-time test that pushes an encoded multi-frame stream one byte per call, accumulates returned frames, calls `finish()`, and compares all fields/payloads.

- [ ] **Step 2: Add backpressure tests**

Use `MultiplexLimits{.maximum_payload_bytes = 1024, .maximum_buffered_bytes = 4096, .maximum_frames_per_push = 1}`. Push bytes containing three complete frames. Require the first call returns one frame and leaves `buffered_bytes() > 0`; two successive `push({})` calls return the next frames in order; `finish()` then succeeds.

- [ ] **Step 3: Add corruption and malformed-wire tests**

For fresh decoders, mutate encoded bytes to prove each failure class:

- payload byte flipped after offset 164 -> `protocol` digest mismatch;
- semantic header byte (for example sequence) flipped -> `protocol` digest mismatch;
- magic byte changed -> `protocol`;
- version changed -> `protocol`;
- flags nonzero -> `protocol`;
- header size not 164 -> `protocol`;
- payload size inconsistent with total size -> `protocol`;
- encoded positive timebase changed to zero with digest recomputed using a test-local helper or by constructing a raw header -> `protocol`;
- end time changed below start time with digest recomputed -> `protocol`;
- declared payload/frame larger than configured limits -> `resource_exhausted`;
- a valid prefix followed by a complete corrupt second frame in the same `push()` -> the `Result` is an error and no partial vector is observable;
- half a valid frame followed by `finish()` -> `protocol`;
- `push()` after successful `finish()` -> `invalid_argument`.

- [ ] **Step 4: Implement incremental decoder**

`MultiplexDecoder::Impl` contains:

```cpp
MultiplexLimits limits{};
std::vector<std::byte> buffer;
bool finished{false};
bool failed{false};
```

`push()` must:

1. reject `finished` / `failed` instances;
2. validate limits at construction/use;
3. checked-add incoming bytes to the current buffer and reject growth above `maximum_buffered_bytes` before insertion;
4. append input bytes;
5. parse from offset zero into a local `std::vector<MultiplexFrame> staged` without erasing buffer bytes until the call succeeds;
6. stop successfully on an incomplete header/frame;
7. stop successfully at `maximum_frames_per_push`, leaving remaining complete bytes buffered;
8. validate magic/version/flags/header size/length arithmetic/payload bounds/interval/timebase before constructing the frame;
9. recompute SHA-256 over the exact first 132 header bytes and payload, compare all 32 digest bytes, then decode fields;
10. if any complete frame is malformed, set `failed = true` and return an error without returning `staged`;
11. after successful parsing, erase only consumed bytes and return `staged`.

Catch `std::bad_alloc`, set `failed = true`, and return `resource_exhausted`.

`finish()` returns `protocol` and marks failed if any bytes remain; otherwise it marks `finished = true` and succeeds. `buffered_bytes()` returns zero for a moved-from/null pimpl.

- [ ] **Step 5: Run targeted and full Release tests**

```bash
cmake --build build --parallel
./build/codec_tests --include-prefix transport_multiplex_
ctest --test-dir build --output-on-failure
```

Expected: all E.1 tests and all pre-existing tests pass.

- [ ] **Step 6: Commit incremental demultiplexing**

```bash
git add src/transport/multiplex.cpp tests/test_transport_multiplex.cpp
git commit -m "Complete Stage E.1 incremental demultiplexing"
```

---

### Task 4: Prove installed-package usability and document the capability boundary

**Files:**
- Modify: `tests/package_consumer/main.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: installed `<codec/transport.hpp>` and `codec::codec` target.
- Produces: package-consumer proof and truthful E.1 implementation status.

- [ ] **Step 1: Extend the installed consumer**

Add:

```cpp
#include <codec/transport.hpp>
```

Then create a minimal valid frame:

```cpp
codec::MultiplexFrame multiplex;
multiplex.stream = codec::derive_stream_id("package-consumer/mux");
multiplex.clock.source_timebase_numerator = 1;
multiplex.clock.source_timebase_denominator = 1;
multiplex.payload = {std::byte{0x42}};
auto encoded_multiplex = codec::encode_multiplex_frame(multiplex);
```

Include `!encoded_multiplex` in the consumer's failure expression. Do not add any new consumer link dependency.

- [ ] **Step 2: Update README implementation status**

Record only these new facts:

- Stage E has started with generic CMX1 multiplex framing.
- Many logical `StreamId`s can be interleaved on one physical byte stream.
- sequence/epoch/clock/interval/payload are preserved per frame.
- framing is bounded and SHA-256 corruption-detecting.

Explicitly retain non-claims for sockets/network protocols, cryptographic authentication, ordering/recovery/FEC, scale benchmarks, and Stage E completion.

- [ ] **Step 3: Update CHANGELOG**

Under the current unreleased/development section, add a concise E.1 entry for the additive transport API and its scope. Do not imply a normative frozen wire standard.

- [ ] **Step 4: Verify install/package consumer locally**

```bash
cmake --install build --prefix /tmp/codec-e1-install
cmake -S tests/package_consumer -B /tmp/codec-e1-consumer -G Ninja \
  -DCMAKE_PREFIX_PATH=/tmp/codec-e1-install
cmake --build /tmp/codec-e1-consumer --parallel
/tmp/codec-e1-consumer/codec_package_consumer
```

Expected: exit 0 and no additional transport dependency.

- [ ] **Step 5: Commit package/docs**

```bash
git add tests/package_consumer/main.cpp README.md CHANGELOG.md
git commit -m "Document Stage E.1 multiplexing"
```

---

### Task 5: Exact-head verification and integration

**Files:**
- No production file changes unless verification finds a concrete defect.
- Update PR/roadmap evidence after verification.

**Interfaces:**
- Consumes: complete E.1 branch.
- Produces: exact-head CI evidence, reviewed PR, squash merge, and post-merge `main` proof.

- [ ] **Step 1: Run the complete local verification matrix**

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/codec_tests --include-prefix transport_multiplex_
./build/codec capabilities
```

Then run Debug ASan/UBSan with the repository's existing sanitizer configuration. Do not weaken `detect_leaks=1` in GitHub CI and do not alter the ONNX-only LSan suppression.

- [ ] **Step 2: Review the final diff against the D.9 base**

Require:

- no CODA layout/version delta;
- no archive/audio/inference semantic change;
- no truth-class assignment in transport code;
- no transport provider/network dependency;
- no implicit continuity/gap/recovery behavior;
- all integer size arithmetic checked before allocation;
- digest covers every semantic header field plus payload;
- no partial frame vector on a later complete-frame failure.

- [ ] **Step 3: Require exact-head remote CI**

The exact candidate SHA must pass GCC build/test/install/package-consumer, Clang build/test/install/package-consumer, and sanitizer CTest including existing `codec-onnx-runtime` isolation.

- [ ] **Step 4: Merge only the exact tested head**

Squash-merge PR with GitHub expected-head guard after confirming `main` has not moved incompatibly and no unresolved review thread exists.

- [ ] **Step 5: Require post-merge `main` CI and record roadmap evidence**

Verify the published merge commit is GitHub-verified and its tree equals the tested PR tree. Require push-triggered `main` CI success before recording E.1 complete on roadmap issue #10. The record must list base/head/tree/PR/CI/merge SHAs, RED proof, exact transport claim, preserved non-claims, and the next Stage E dependency: explicit loss/gap/recovery-group semantics before a concrete FEC algorithm.
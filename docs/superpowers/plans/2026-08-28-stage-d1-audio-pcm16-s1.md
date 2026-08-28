# Stage D.1 Deterministic Audio PCM16 S1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a deterministic, self-contained Audio Profile PCM16 canonical-state payload and prove exact CODA S1 storage with direct preserved-S0 provenance.

**Architecture:** Add a distinct profile-specific `Pcm16State` and APS1 codec in a focused audio source file. Generic CODA APIs continue to own physical storage and provenance: callers append the APS1 payload with the existing `pcm16` record type and explicitly declare S1 through the existing state-exact provenance sidecar.

**Tech Stack:** C++20, existing `Result`, CODA writer/query/extraction/provenance APIs, CMake, custom unit harness, CTest, ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-d1-audio-pcm16-s1-design.md`

## Global Constraints

- WAV/RIFF source bytes remain S0; `Pcm16State` is a separate container-independent S1 candidate.
- Canonicalization copies valid signed PCM16 samples/rate/channels exactly and performs no resampling, remixing, dithering, enhancement, inference, or channel-layout interpretation.
- A physical `pcm16` record is S1 only when an explicit valid `state_exact` provenance sidecar links it to exact S0 input records.
- APS1 is a versioned Audio Profile payload inside the existing provisional CODA envelope; add no record type, envelope/layout, C ABI, CLI, or capability change.
- Preserve every root `codec::*` audio API/ABI and expose the new API through exact `codec::profiles::audio` using-declarations.
- Add no FLAC, model/runtime, streaming inference, identity fusion, recovery, transaction, scale, deployment, or Stage D completion claim.

## File Structure

- Modify `include/codec/audio.hpp`: declare `Pcm16State` and canonicalize/encode/decode APIs.
- Create `src/audio/pcm16_state.cpp`: own APS1 validation and deterministic byte encoding/decoding.
- Modify `include/codec/profiles/audio.hpp`: forward the exact new root type/functions.
- Modify `CMakeLists.txt`: compile the focused Audio Profile state codec.
- Modify `tests/test_audio_profile.cpp`: exact fixture, invalid input, decode, archive/provenance, and facade proofs.
- Modify `README.md` and `CHANGELOG.md`: state only the proven D.1 capability.
- Create ignored `build-stage-d1-consumer-src/`: installed-package proof; do not commit it.

---

### Task 1: Add canonical PCM16 state and deterministic APS1 encoding

**Files:**
- Modify: `tests/test_audio_profile.cpp`
- Modify: `include/codec/audio.hpp`
- Create: `src/audio/pcm16_state.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: valid decoded `WavPcm16` values.
- Produces: `Pcm16State`, `canonicalize_pcm16()`, and `encode_pcm16_state()`.

- [ ] **Step 1: Add byte and exact-state test helpers**

Add `<array>`, `<cstddef>`, `<cstdint>`, `<span>`, and `<vector>` to
`tests/test_audio_profile.cpp`. Add:

```cpp
std::vector<std::byte> aps1_fixture() {
  return {
      std::byte{0x41}, std::byte{0x50}, std::byte{0x53}, std::byte{0x31},
      std::byte{0x01}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
      std::byte{0x80}, std::byte{0xbb}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x04}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x80}, std::byte{0xff}, std::byte{0xff},
      std::byte{0x01}, std::byte{0x00}, std::byte{0xff}, std::byte{0x7f},
  };
}

codec::WavPcm16 exact_wav() {
  return codec::WavPcm16{
      .sample_rate = 48000,
      .channels = 2,
      .samples = {-32768, -1, 1, 32767},
  };
}
```

The fixture is hand-derived from the APS1 table and must not call production
encoding helpers.

- [ ] **Step 2: Write the failing canonicalization and encoding tests**

Add:

```cpp
TEST(audio_pcm16_canonicalization_preserves_exact_state) {
  auto state = codec::canonicalize_pcm16(exact_wav());
  EXPECT_TRUE(state);
  if (state) {
    EXPECT_EQ(state->sample_rate, std::uint32_t{48000});
    EXPECT_EQ(state->channels, std::uint16_t{2});
    EXPECT_EQ(state->frames(), std::size_t{2});
    EXPECT_EQ(state->samples,
              (std::vector<std::int16_t>{-32768, -1, 1, 32767}));
  }
}

TEST(audio_pcm16_state_encoding_matches_the_aps1_fixture) {
  const codec::Pcm16State state{
      .sample_rate = 48000,
      .channels = 2,
      .samples = {-32768, -1, 1, 32767},
  };
  auto encoded = codec::encode_pcm16_state(state);
  EXPECT_TRUE(encoded);
  if (encoded) EXPECT_EQ(*encoded, aps1_fixture());
}
```

The breaks caught are sample mutation/reordering, state/container conflation,
wrong header values, native-endian encoding, or signed-sample corruption.

- [ ] **Step 3: Configure and build to verify RED**

```bash
/tmp/codec-d1-tools/cmake/data/bin/cmake -S . \
  -B /tmp/codec-stage-d1-red -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
CMAKE_BUILD_PARALLEL_LEVEL=1 \
  /tmp/codec-d1-tools/cmake/data/bin/cmake --build \
  /tmp/codec-stage-d1-red --target codec_tests
```

Expected: compilation fails only because the new type/functions are absent.

- [ ] **Step 4: Add the public canonical state API**

In `include/codec/audio.hpp`, add `<cstddef>` and `<span>`, then declare:

```cpp
struct Pcm16State {
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  std::vector<std::int16_t> samples;

  std::size_t frames() const noexcept {
    return channels == 0 ? 0 : samples.size() / channels;
  }
  double duration_seconds() const noexcept {
    return sample_rate == 0
               ? 0.0
               : static_cast<double>(frames()) / sample_rate;
  }
};

Result<Pcm16State> canonicalize_pcm16(const WavPcm16& source);
Result<std::vector<std::byte>> encode_pcm16_state(
    const Pcm16State& state);
```

- [ ] **Step 5: Create the minimal canonicalizer and encoder**

Create `src/audio/pcm16_state.cpp`. Use `std::bit_cast<std::uint16_t>` for
sample bit patterns and explicit little-endian helpers:

```cpp
void put16(std::span<std::byte> output, std::size_t offset,
           std::uint16_t value);
void put32(std::span<std::byte> output, std::size_t offset,
           std::uint32_t value);
void put64(std::span<std::byte> output, std::size_t offset,
           std::uint64_t value);
```

Implement validation once in a private helper:

```cpp
bool valid_pcm16(std::uint32_t rate, std::uint16_t channels,
                 std::size_t samples) {
  return rate != 0 && channels != 0 && samples % channels == 0;
}
```

`canonicalize_pcm16()` returns `invalid_argument` for invalid layout and
otherwise copies fields exactly.

`encode_pcm16_state()` validates the same invariant, rejects a sample vector
larger than `(std::numeric_limits<std::size_t>::max() - 24) / 2`, writes
`APS1`, version 1, format 1, rate, channels, zero reserved flags, `uint64_t`
sample count, then every sample's exact 16-bit bit pattern in little-endian
order.

- [ ] **Step 6: Register the focused source in CMake and verify GREEN**

Add `src/audio/pcm16_state.cpp` beside `src/audio/wav.cpp` in `codec_core`.
Build and run `/tmp/codec-stage-d1-red/codec_tests`.

Expected: 86 tests, 0 failures.

- [ ] **Step 7: Commit the canonical state and encoder**

```bash
git add CMakeLists.txt include/codec/audio.hpp src/audio/pcm16_state.cpp \
  tests/test_audio_profile.cpp
git commit -m "feat: define deterministic audio PCM16 state"
```

### Task 2: Decode APS1 and prove malformed-state isolation

**Files:**
- Modify: `tests/test_audio_profile.cpp`
- Modify: `include/codec/audio.hpp`
- Modify: `src/audio/pcm16_state.cpp`

**Interfaces:**
- Consumes: Task 1 APS1 encoder.
- Produces: exact APS1 decode and deterministic invalid-state errors.

- [ ] **Step 1: Add invalid-state and exact decode tests**

Add one test that passes these states through canonicalize/encode and expects
`invalid_argument`:

```cpp
WavPcm16{.sample_rate = 0, .channels = 1, .samples = {1}}
WavPcm16{.sample_rate = 48000, .channels = 0, .samples = {1}}
WavPcm16{.sample_rate = 48000, .channels = 2, .samples = {1}}
Pcm16State{.sample_rate = 0, .channels = 1, .samples = {1}}
Pcm16State{.sample_rate = 48000, .channels = 0, .samples = {1}}
Pcm16State{.sample_rate = 48000, .channels = 2, .samples = {1}}
```

Add the exact decode test:

```cpp
TEST(audio_pcm16_state_decodes_and_reencodes_exactly) {
  auto decoded = codec::decode_pcm16_state(aps1_fixture());
  EXPECT_TRUE(decoded);
  if (decoded) {
    EXPECT_EQ(decoded->sample_rate, std::uint32_t{48000});
    EXPECT_EQ(decoded->channels, std::uint16_t{2});
    EXPECT_EQ(decoded->samples,
              (std::vector<std::int16_t>{-32768, -1, 1, 32767}));
    auto reencoded = codec::encode_pcm16_state(*decoded);
    EXPECT_TRUE(reencoded);
    if (reencoded) EXPECT_EQ(*reencoded, aps1_fixture());
  }
}
```

- [ ] **Step 2: Add one table-driven malformed APS1 test**

Create copies of `aps1_fixture()` for these literal mutations:

- truncate to 23 bytes;
- set magic byte 0 to `0x00`;
- set version byte 4 to `0x02`;
- set format byte 6 to `0x02`;
- set reserved byte 14 to `0x01`;
- zero bytes 8 through 11 (rate);
- zero bytes 12 and 13 (channels);
- set sample-count byte 16 to `0x05` while four samples remain;
- set channels to 3 while four samples remain; and
- append one trailing byte.

Call `decode_pcm16_state()` for each, assert failure and `ErrorCode::decode`.
The expected error code is literal and independent of decoder helpers.

- [ ] **Step 3: Run the unit binary to verify RED**

Expected: compilation fails because `decode_pcm16_state()` is absent.

- [ ] **Step 4: Implement exact decode**

First declare in `include/codec/audio.hpp`:

```cpp
Result<Pcm16State> decode_pcm16_state(
    std::span<const std::byte> payload);
```

Then add private `get16`, `get32`, and `get64` little-endian helpers. Decode in
this order:

1. require at least 24 bytes and exact `APS1` magic;
2. require version 1, format 1, and reserved 0;
3. load non-zero rate/channels and `uint64_t` sample count;
4. require an even remaining byte count;
5. require sample count equals `(payload.size() - 24) / 2` exactly;
6. require sample count is divisible by channels; and
7. only then resize and decode each sample with
   `std::bit_cast<std::int16_t>(get16(...))`.

Every malformed path returns `ErrorCode::decode` without allocation before the
complete header/length/frame validation.

- [ ] **Step 5: Verify GREEN and perform an endian mutation proof**

Build/run the unit binary. Expected: 89 tests, 0 failures.

Temporarily swap the two bytes written for each sample, rebuild, and run. The
hand-derived APS1 fixture test must fail. Restore the correct encoder and
confirm 89 tests, 0 failures.

- [ ] **Step 6: Commit decode and failure isolation**

```bash
git add include/codec/audio.hpp src/audio/pcm16_state.cpp \
  tests/test_audio_profile.cpp
git commit -m "test: prove APS1 decode boundaries"
```

### Task 3: Prove CODA S1 storage, provenance, and facade compatibility

**Files:**
- Modify: `tests/test_audio_profile.cpp`
- Modify: `include/codec/profiles/audio.hpp`

**Interfaces:**
- Consumes: complete APS1 codec plus generic writer/query/extraction/provenance.
- Produces: exact D.1 archive integration and canonical facade exposure.

- [ ] **Step 1: Expose exact root symbols through the profile facade**

Add:

```cpp
using ::codec::Pcm16State;
using ::codec::canonicalize_pcm16;
using ::codec::decode_pcm16_state;
using ::codec::encode_pcm16_state;
```

Add `static_assert(std::is_same_v<audio_profile::Pcm16State,
codec::Pcm16State>);` to the facade test.

- [ ] **Step 2: Write the failing archive/provenance integration test**

In `tests/test_audio_profile.cpp`, include `<codec/archive.hpp>`,
`<filesystem>`, and `<optional>`. Add local deterministic stream-ID and byte
helpers.

The test must:

1. create a temporary absent archive;
2. append literal fake WAV bytes as `source_bytes` under one stream;
3. canonicalize `exact_wav()` through `audio_profile::canonicalize_pcm16()`;
4. encode it through `audio_profile::encode_pcm16_state()`;
5. append the bytes as `RecordType::pcm16` with the same interval;
6. append `state_exact` provenance using the source record as the only input
   and process identity `audio.pcm16.canonicalize` / `codec-audio-profile` /
   version `1`;
7. finalize, reopen, verify, query/extract the `pcm16` record, and decode it;
8. query provenance by `state_exact` plus physical PCM subject;
9. assert subject and input stream/raw type/sequence/hash equal the exact
   records returned by the writer; and
10. independently extract and compare the original S0 bytes.

The break caught is storing undecodable PCM, omitting/misclassifying S1
provenance, linking the wrong subject/input, or making S0 inaccessible.

- [ ] **Step 3: Run a provenance-omission mutation proof**

First run the complete test green. Then temporarily omit the
`append_stream_provenance()` call while leaving the PCM record, finalize, and
run the unit binary. The integration test must fail because its state-exact
query is empty. Restore the sidecar and confirm the full unit binary passes.

Expected final count: 90 tests, 0 failures.

- [ ] **Step 4: Commit archive and facade proof**

```bash
git add include/codec/profiles/audio.hpp tests/test_audio_profile.cpp
git commit -m "test: prove audio PCM16 S1 storage"
```

### Task 4: Publish truthful status and prove installed consumption

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Create ignored: `build-stage-d1-consumer-src/CMakeLists.txt`
- Create ignored: `build-stage-d1-consumer-src/main.cpp`

**Interfaces:**
- Consumes: complete D.1 public profile/storage contract.
- Produces: truthful runtime status and installed-package evidence.

- [ ] **Step 1: Update only proven status**

Add one Audio Profile implemented bullet and one Unreleased changelog entry for
the deterministic self-contained APS1 PCM16 state, exact sample round-trip,
and explicit S0-linked S1 provenance proof. State that FLAC, automatic media
adapters/persistence, models, streaming inference, identity fusion, and Audio
Profile 1.0 completion remain unimplemented.

- [ ] **Step 2: Create an ignored installed consumer**

Use:

```cmake
cmake_minimum_required(VERSION 3.20)
project(codec_audio_s1_consumer LANGUAGES CXX)
find_package(codec 0.1 CONFIG REQUIRED)
add_executable(codec_audio_s1_consumer main.cpp)
target_compile_features(codec_audio_s1_consumer PRIVATE cxx_std_20)
target_link_libraries(codec_audio_s1_consumer PRIVATE codec::codec)
```

The complete consumer includes only installed public headers, creates a valid
`audio_profile::WavPcm16`, canonicalizes/encodes it, appends S0 plus PCM plus
state-exact provenance, finalizes/reopens/extracts/decodes, verifies exact
samples and exact provenance subject/input links, then removes the archive and
returns zero.

- [ ] **Step 3: Run fresh Release, install, consumer, and sanitizer gates**

Use a `mktemp -d` root outside the workspace, CMake 3.31.6 direct binary,
system Make, warnings as errors, and one build job.

Require:

```bash
ctest --test-dir "$proof_root/release" --output-on-failure
"$proof_root/release/codec_tests"
"$proof_root/release/codec" capabilities
cmake --install "$proof_root/release" --prefix "$proof_root/install"
```

Configure/build/run the consumer with the install prefix. Then require:

```bash
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir "$proof_root/san" --output-on-failure
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  "$proof_root/san/codec_tests"
```

Expected: 4/4 Release, 4/4 sanitizer, 90 direct unit tests, installed consumer
success, and unchanged neural/GPU false capability values.

- [ ] **Step 4: Audit and commit status**

```bash
git diff --check
git status --short
git diff --stat 7fcb58a07e002f0ae9b32c5d04acc861b6720008..HEAD
git add README.md CHANGELOG.md
git commit -m "docs: mark deterministic audio S1 implemented"
```

### Task 5: Verify and publish the exact D.1 tree

**Files:**
- No source changes expected.

**Interfaces:**
- Consumes: final committed Stage D.1 tree.
- Produces: verified non-force publication and roadmap evidence.

- [ ] **Step 1: Re-run exact-HEAD verification**

Rerun Release build/CTest/direct unit, sanitizer build/CTest/direct unit,
installed consumer, capability JSON, `git diff --check`, claim audit, and clean
tracked-worktree checks on the final commit.

- [ ] **Step 2: Recheck continuity immediately before publication**

Confirm remote `main` is still
`7fcb58a07e002f0ae9b32c5d04acc861b6720008`, no PR is open, and issue #10
remains the unique roadmap log. Stop rather than force if `main` moved.

- [ ] **Step 3: Publish without force**

Publish the exact tested tree as one Stage D.1 commit whose parent is the
verified base. Verify the remote tree equals the local tested tree, update
`main` with force disabled, and fast-forward the local main checkout.

- [ ] **Step 4: Confirm exact-commit CI and record completion**

Wait for exactly `build (gcc)`, `build (clang)`, and `sanitizers` on the exact
published SHA. All three must complete successfully. Add a completion comment
to issue #10 with base/head/tree, 90-test count, APS1 fixture and archive S1
proof, installed consumer, compatibility/capability/non-claims, CI links, and
the next Audio Profile dependency. Do not claim Stage D complete.

# Stage D.8 Audio Separation ModelBundle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded deterministic AMB1 Audio Profile bundle that binds a strict separation manifest to exact opaque ONNX bytes by SHA-256 without executing a model or changing neural capability claims.

**Architecture:** Add one profile-only public header and one focused implementation source. The encoder emits a canonical big-endian AMB1 byte sequence; the decoder validates all outer bounds and lengths, reconstructs the manifest, verifies the exact model hash, and returns an owned verified bundle plus whole-bundle identity. Existing inference, CODA, CLI, and C ABI surfaces remain unchanged.

**Tech Stack:** C++20, `std::span`, `std::vector<std::byte>`, CODEC `Result`/`ErrorCode`, existing public SHA-256 API, CMake/CTest, GCC/Clang/ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-d8-audio-separation-model-bundle-design.md`

## Global Constraints

- The implementation is profile-scoped under `codec::profiles::audio`; generic core structures gain no audio/model fields.
- AMB1 fixes the task to audio separation, portable format to ONNX, input layout to float32 `[batch, channel, sample]`, output layout to float32 `[batch, source, channel, sample]`, and PCM normalization to signed PCM16 divided by 32768.0.
- The canonical byte order and field order are exactly those in the spec; unknown flags, noncanonical lengths, truncation, trailing bytes, and hash mismatch fail closed.
- The D.7 maximum-source ceiling remains 64; AMB1 channel count is at most 64.
- No filesystem, network, CODA, runtime/provider, ONNX parsing/execution, signature, download, CLI, or C ABI behavior is added.
- `neural_separation:false` and `gpu_inference:false` remain unchanged.
- Use bounded build parallelism (`--parallel 4`) in this managed environment; if a generated executable is observed as a zero-byte artifact, force only that target to relink with `--parallel 1` and record the evidence.

---

## File structure

- Create `include/codec/profiles/audio_model_bundle.hpp`: public AMB1 manifest, bundle, verified bundle, limits, and encode/decode declarations.
- Create `src/audio/separation_model_bundle.cpp`: canonical serialization, strict parsing, semantic validation, checked sizing, and SHA-256 verification.
- Create `tests/test_audio_model_bundle.cpp`: exact fixture, round-trip, invariant, resource, corruption, and facade proofs.
- Modify `include/codec/profiles/audio.hpp`: expose the additive profile header.
- Modify `CMakeLists.txt`: compile the implementation and dedicated test.
- Modify `tests/package_consumer/main.cpp`: prove the installed facade exposes the AMB1 types and function declarations.
- Modify `README.md` and `CHANGELOG.md`: add the exact implemented structural/integrity claim and preserve runtime non-claims.

---

### Task 1: Establish the RED AMB1 contract

**Files:**
- Create: `tests/test_audio_model_bundle.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing `codec::sha256`, `codec::sha256_hex`, `codec::Result`, and the test registry in `tests/test.hpp`.
- Produces: a compile-time and runtime contract for `SeparationModelManifest`, `SeparationModelBundle`, `VerifiedSeparationModelBundle`, `SeparationModelBundleLimits`, `encode_separation_model_bundle`, and `decode_separation_model_bundle`.

- [ ] **Step 1: Add the test file to the unit target before the API exists**

Insert `tests/test_audio_model_bundle.cpp` beside the other Audio Profile tests in `codec_tests`.

```cmake
    tests/test_audio_model_bundle.cpp
```

- [ ] **Step 2: Write the known bundle and exact fixture helpers**

Use the profile facade, not an implementation-private header:

```cpp
#include "test.hpp"

#include <codec/integrity.hpp>
#include <codec/profiles/audio.hpp>

namespace audio_profile = codec::profiles::audio;

audio_profile::SeparationModelBundle known_bundle() {
  return {
      .manifest = {
          .model_id = "codec.test.separator",
          .model_version = "1.2.3",
          .license_id = "Apache-2.0",
          .quality_domain = "test",
          .input_sample_rate = 48000,
          .input_channels = 2,
          .window_samples = 8,
          .hop_samples = 4,
          .lookahead_samples = 0,
          .maximum_sources = 2,
          .causal = true,
          .input_tensor_name = "mixture",
          .output_tensor_name = "sources",
      },
      .onnx_model = {
          std::byte{0x08}, std::byte{0x07}, std::byte{0x12},
          std::byte{0x03}, std::byte{0x6f}, std::byte{0x6e},
          std::byte{0x78},
      },
  };
}
```

Decode this exact lowercase hexadecimal fixture with a small test helper:

```text
414d423100010000005500000000000000070d24ca63c08904d467eb2ff5d79d582125ebd58821a7a58eb534f01b8d736e1a0000bb80000200020000000800000004000000000014636f6465632e746573742e736570617261746f720005312e322e33000a4170616368652d322e3000047465737400076d6978747572650007736f7572636573080712036f6e78
```

The expected model hash is
`0d24ca63c08904d467eb2ff5d79d582125ebd58821a7a58eb534f01b8d736e1a`.
The expected whole-bundle hash is
`3c1cf1aae3f87c83358eb69f59c822e7190be188fd57cc9f01c905d9c9aec90a`.

- [ ] **Step 3: Write success and canonicality tests**

Add tests with these exact assertions:

```cpp
TEST(audio_model_bundle_matches_the_amb1_fixture) {
  const auto bundle = known_bundle();
  const auto encoded = audio_profile::encode_separation_model_bundle(bundle);
  EXPECT_TRUE(encoded);
  if (encoded) EXPECT_EQ(*encoded, amb1_fixture());
}

TEST(audio_model_bundle_verifies_manifest_model_and_bundle_identity) {
  const auto fixture = amb1_fixture();
  const auto decoded = audio_profile::decode_separation_model_bundle(fixture);
  EXPECT_TRUE(decoded);
  if (!decoded) return;
  EXPECT_EQ(decoded->manifest.model_id, std::string{"codec.test.separator"});
  EXPECT_EQ(decoded->manifest.model_version, std::string{"1.2.3"});
  EXPECT_EQ(decoded->manifest.license_id, std::string{"Apache-2.0"});
  EXPECT_EQ(decoded->manifest.quality_domain, std::string{"test"});
  EXPECT_EQ(decoded->manifest.input_sample_rate, std::uint32_t{48000});
  EXPECT_EQ(decoded->manifest.input_channels, std::uint16_t{2});
  EXPECT_EQ(decoded->manifest.window_samples, std::uint32_t{8});
  EXPECT_EQ(decoded->manifest.hop_samples, std::uint32_t{4});
  EXPECT_EQ(decoded->manifest.lookahead_samples, std::uint32_t{0});
  EXPECT_EQ(decoded->manifest.maximum_sources, std::uint16_t{2});
  EXPECT_TRUE(decoded->manifest.causal);
  EXPECT_EQ(decoded->manifest.input_tensor_name, std::string{"mixture"});
  EXPECT_EQ(decoded->manifest.output_tensor_name, std::string{"sources"});
  EXPECT_EQ(decoded->onnx_model, known_bundle().onnx_model);
  EXPECT_EQ(decoded->model_hash, codec::sha256(decoded->onnx_model));
  EXPECT_EQ(decoded->bundle_hash, codec::sha256(fixture));

  const auto reencoded = audio_profile::encode_separation_model_bundle(
      {.manifest = decoded->manifest, .onnx_model = decoded->onnx_model});
  EXPECT_TRUE(reencoded);
  if (reencoded) EXPECT_EQ(*reencoded, fixture);
}
```

- [ ] **Step 4: Write table-driven encoder validation tests**

Use a helper that starts from `known_bundle()`, applies one mutation, expects no
value, and checks the exact error code. Cover:

```text
invalid_argument:
  every empty text field
  every text field containing NUL, newline, DEL, or non-ASCII byte
  zero sample rate
  zero channels and 65 channels
  zero window
  zero hop and hop greater than window
  lookahead greater than window
  causal with non-zero lookahead
  zero maximum sources and 65 maximum sources
  identical input/output tensor names
  empty ONNX bytes
  each zero limit

resource_exhausted:
  text longer than maximum_text_bytes
  model longer than maximum_model_bytes
  exact output longer than maximum_bundle_bytes
```

The helper must assert the input bundle remains unchanged after each failed
encode call.

- [ ] **Step 5: Write strict decoder failure tests**

Start from `amb1_fixture()` and make one mutation per call. Assert
`model_incompatible` for wrong magic, unknown flag bit, manifest length one
smaller/larger, model length one smaller/larger, zero model length, truncated
header, each truncation boundary, trailing byte, model byte corruption, stored
hash corruption, invalid manifest text, zero/invalid geometry, and identical
tensor names. Assert `resource_exhausted` when the supplied encoded bytes,
declared model, or a declared text field exceeds its configured limit.

For length fields use explicit byte offsets from the AMB1 table:

```cpp
constexpr std::size_t flags_offset = 4;
constexpr std::size_t manifest_length_offset = 6;
constexpr std::size_t model_length_offset = 10;
constexpr std::size_t model_hash_offset = 18;
constexpr std::size_t manifest_offset = 50;
```

Verify the function returns no `VerifiedSeparationModelBundle` on every
failure. Decode input must remain byte-identical after the call.

- [ ] **Step 6: Run the clean RED proof**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel 4
```

Expected: existing production sources compile, then the new test fails because
`codec/profiles/audio_model_bundle.hpp` or the declared AMB1 symbols are absent.
Record that exact compiler failure as the accepted RED; do not count unrelated
dependency or source failures.

- [ ] **Step 7: Commit the RED proof**

```bash
git add CMakeLists.txt tests/test_audio_model_bundle.cpp
git commit -m "test: define Stage D.8 ModelBundle contract"
```

---

### Task 2: Implement canonical AMB1 encode and verified decode

**Files:**
- Create: `include/codec/profiles/audio_model_bundle.hpp`
- Create: `src/audio/separation_model_bundle.cpp`
- Modify: `include/codec/profiles/audio.hpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_audio_model_bundle.cpp`

**Interfaces:**
- Consumes: exact Task 1 test contract plus `codec::sha256(std::span<const std::byte>)`.
- Produces: the four public structures and two result-returning functions specified by the design.

- [ ] **Step 1: Add the public profile header exactly as specified**

Include only standard types plus `<codec/integrity.hpp>` and
`<codec/result.hpp>`. Keep all declarations in
`namespace codec::profiles::audio`. Use the exact field order and defaults from
the design so aggregate initialization in the fixture and package consumer is
stable.

- [ ] **Step 2: Add AMB1 to the profile facade and production target**

Add:

```cpp
#include <codec/profiles/audio_model_bundle.hpp>
```

to `include/codec/profiles/audio.hpp`, and add:

```cmake
  src/audio/separation_model_bundle.cpp
```

to `codec_core`.

- [ ] **Step 3: Implement checked canonical writer helpers**

In the private implementation namespace define:

```cpp
constexpr std::array<std::byte, 4> amb1_magic{
    std::byte{'A'}, std::byte{'M'}, std::byte{'B'}, std::byte{'1'}};
constexpr std::uint16_t causal_flag = 0x0001U;
constexpr std::uint16_t known_flags = causal_flag;
constexpr std::uint16_t maximum_profile_channels = 64U;
constexpr std::uint16_t maximum_profile_sources = 64U;
constexpr std::size_t fixed_header_bytes = 50U;
constexpr std::size_t fixed_manifest_bytes = 20U;
```

Implement big-endian `append_u16`, `append_u32`, `append_u64`, bounded
`append_text`, and checked `add_size`. Never cast a length until the validation
proves it fits the destination integer.

- [ ] **Step 4: Implement shared semantic validation with caller/decode error mapping**

Create an internal validator that accepts an error mode:

```cpp
enum class ValidationOrigin { caller, encoded_bundle };
```

Caller semantic defects map to `invalid_argument`; the same semantic defect
decoded from bytes maps to `model_incompatible`. Configured bound exhaustion
maps to `resource_exhausted` in both modes. Printable ASCII means each byte is
between `0x20` and `0x7e` inclusive.

Validate the limits first, then all six strings, geometry, framing, source
count, causal/lookahead relation, distinct tensor names, and non-empty model.

- [ ] **Step 5: Implement deterministic encoding**

The encoder algorithm is:

```text
validate limits and caller bundle
compute manifest size with checked arithmetic
compute total size = 50 + manifest size + model size with checked arithmetic
reject total size over maximum_bundle_bytes
compute model_hash = sha256(onnx_model)
reserve the exact total size
append magic, flags, manifest size, model size, and model hash
append numeric manifest fields and six length-prefixed strings
append exact model bytes
assert the produced size equals the preflight total; otherwise return internal
return encoded bytes
```

Do not build a partial public result on error.

- [ ] **Step 6: Implement bounded strict decoding**

Add a cursor reader that operates on `std::span<const std::byte>` and refuses
to advance unless the full requested width is present. The decoder must:

```text
validate non-zero limits
reject encoded.size() > maximum_bundle_bytes as resource_exhausted
require at least 50 bytes
consume and compare AMB1 magic
read flags and reject any bit outside 0x0001
read manifest and model lengths without narrowing
reject model length over maximum_model_bytes as resource_exhausted
prove 50 + manifest length + model length equals encoded.size() exactly
read the stored 32-byte model hash
create a cursor limited to exactly the manifest body
read all numeric fields and six bounded text fields
require the manifest cursor is exhausted exactly
form a span over exactly the declared model bytes
validate decoded semantics using encoded_bundle mode
recompute SHA-256 and compare all 32 bytes through an XOR accumulator
copy model bytes only after every structural/hash check passes
compute bundle_hash over the original encoded span
return the complete verified bundle
```

Wrong magic, flags, structure, semantic content, or hash returns
`model_incompatible`. Outer/declaration bounds return `resource_exhausted`.

- [ ] **Step 7: Run targeted GREEN and the full Release suite**

Run:

```bash
cmake --build build --parallel 4
./build/codec_tests
ctest --test-dir build --output-on-failure
```

Expected: all D.8 fixture/round-trip/failure tests and all pre-existing tests
pass. If the direct test executable is a zero-byte generated artifact, remove
only `build/codec_tests`, rebuild that target with `--parallel 1`, inspect its
size/mode, and rerun it before counting GREEN.

- [ ] **Step 8: Commit the implementation**

```bash
git add CMakeLists.txt include/codec/profiles/audio.hpp \
  include/codec/profiles/audio_model_bundle.hpp \
  src/audio/separation_model_bundle.cpp
git commit -m "feat: add verified separation ModelBundle"
```

Include any small test correction required to make the written contract and
canonical fixture agree; do not weaken or delete a failure case.

---

### Task 3: Prove downstream packaging and synchronize runtime truth

**Files:**
- Modify: `tests/package_consumer/main.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Test: `tests/test_audio_model_bundle.cpp`

**Interfaces:**
- Consumes: the installed `codec/profiles/audio.hpp` AMB1 API from Task 2.
- Produces: installed-package compile/link proof and truthful user-facing status.

- [ ] **Step 1: Exercise the installed profile API in the package consumer**

Add a small in-memory bundle and require encoding succeeds without invoking a
runtime:

```cpp
const codec::profiles::audio::SeparationModelBundle model_bundle{
    .manifest = {
        .model_id = "consumer.separator",
        .model_version = "1",
        .license_id = "Apache-2.0",
        .quality_domain = "test",
        .input_sample_rate = 48000,
        .input_channels = 2,
        .window_samples = 8,
        .hop_samples = 4,
        .lookahead_samples = 0,
        .maximum_sources = 2,
        .causal = true,
        .input_tensor_name = "mixture",
        .output_tensor_name = "sources",
    },
    .onnx_model = {std::byte{0x08}},
};
const auto encoded_model_bundle =
    codec::profiles::audio::encode_separation_model_bundle(model_bundle);
```

Return non-zero if encoding fails. Include `<cstddef>` for `std::byte`.

- [ ] **Step 2: Update the README manifest with one narrow implemented claim**

Add an Audio Profile bullet stating that CODEC implements bounded deterministic
AMB1 encode/strict decode, exact ONNX-byte SHA-256 verification, canonical
manifest metadata, and whole-bundle identity without persistence or execution.

Change the inference status to say the backend boundary and structural AMB1
loader exist, but no compatible neural runtime or production model is bundled
and the default backend remains `model_incompatible`.

Add one concise Audio Profile paragraph listing the fixed AMB1 tensor and PCM
contracts, error/bounds behavior, and explicit signature/runtime/quality
non-claims. Keep Stage D active.

- [ ] **Step 3: Add the matching changelog entry**

Add one top `Unreleased` bullet with the same exact implemented boundary. It
must retain `neural_separation:false`, `gpu_inference:false`, no runtime/model
execution, no CODA/CLI/C ABI delta, and no Stage D completion claim.

- [ ] **Step 4: Run Release, capability, install, and downstream consumer proof**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/codec_tests
./build/codec capabilities
cmake --install build --prefix build/install
cmake -S tests/package_consumer -B build/package-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/install"
cmake --build build/package-consumer --parallel 4
./build/package-consumer/codec_package_consumer
```

Expected: full suite passes; capabilities still print
`neural_separation:false` and `gpu_inference:false`; installed consumer exits
zero.

- [ ] **Step 5: Commit packaging and documentation**

```bash
git add tests/package_consumer/main.cpp README.md CHANGELOG.md
git commit -m "docs: record Stage D.8 ModelBundle contract"
```

---

### Task 4: Complete mandatory verification and exact-head integration

**Files:**
- Audit: every file changed since `31b77e89cb06cfb149b933934e0d589e7d77369f`
- Record externally: roadmap issue #10 and the D.8 pull request

**Interfaces:**
- Consumes: final committed D.8 branch tree.
- Produces: exact local SHA/tree proof, exact-head CI, squash merge to current main, post-merge CI, and roadmap continuity.

- [ ] **Step 1: Run a fresh Release verification on the exact final head**

Run the worksheet Release configure/build/CTest commands with bounded parallel
4, then run `./build/codec_tests`, `./build/codec capabilities`, and the clean
installed-package consumer proof. Record direct test count and every exit code.

- [ ] **Step 2: Run a fresh sanitizer verification on the same head**

Run:

```bash
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel 4
ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --test-dir build-san --output-on-failure
ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
./build-san/codec_tests
```

Record that local leak scanning is disabled only because the managed container
blocks LeakSanitizer parent `/proc` inspection. Require the normal leak-enabled
sanitizer job in GitHub CI before merge.

- [ ] **Step 3: Perform claim and diff audits**

Run:

```bash
git diff --check 31b77e89cb06cfb149b933934e0d589e7d77369f..HEAD
git diff --stat 31b77e89cb06cfb149b933934e0d589e7d77369f..HEAD
git diff 31b77e89cb06cfb149b933934e0d589e7d77369f..HEAD -- \
  CMakeLists.txt include src tests README.md CHANGELOG.md docs/superpowers
git status --short --branch
git rev-parse HEAD
git rev-parse HEAD^{tree}
```

Confirm no generated files, credentials, CODA changes, generic-core model
semantics, runtime/provider implementation, capability flip, or unrelated
refactor entered the branch.

- [ ] **Step 4: Publish and require exact-head CI**

Publish the branch without force. Open or update one D.8 pull request. Record
the exact remote head SHA and tree in issue #10. Require GCC, Clang,
install/package-consumer, and normal leak-enabled sanitizer jobs to succeed on
that exact SHA. If the head moves, repeat local verification and CI gating.

- [ ] **Step 5: Merge only the tested tree and verify main again**

Squash-merge only when the PR is mergeable and every required job is green on
the exact remote head. Verify the published `main` tree equals the locally
tested feature tree even though the squash commit SHA differs. Require the
push-triggered main CI to pass across the same matrix.

- [ ] **Step 6: Record Stage D.8 completion**

Add one issue #10 completion comment with base, design/plan, RED commit and CI,
final tested local/remote head, tested/published tree, PR, merge SHA, exact-head
and post-merge CI runs, direct/CTest/sanitizer/package proof, implemented AMB1
boundary, preserved non-claims, Stage D still active, and D.9 CPU reference
runtime as the next dependency.

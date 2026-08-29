# Stage E.4 Bounded XOR Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a versioned bounded XRF1 XOR repair symbol that reconstructs and exactly verifies one missing E.1 CMX1 frame from one explicit E.2 recovery group.

**Architecture:** A new generic transport component deterministically encodes complete CMX1 frames, commits each exact length and SHA-256, and XORs zero-padded encoded bytes into one parity vector. A strict XRF1 codec serializes the E.2 group descriptor, slot commitments, and parity. Recovery accepts exactly all but one group member, verifies every observed commitment, reconstructs the missing encoded bytes, verifies them twice through the slot hash and CMX1 decoder, and returns both exact bytes and the decoded frame.

**Tech Stack:** C++20, existing `codec::MultiplexFrame`, `RecoveryGroupDescriptor`, `MultiplexDecoder`, `Sha256`, `Result`, STL spans/vectors, CMake/Ninja, custom CODEC test harness, GCC/Clang, ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-29-stage-e4-xor-recovery-design.md`

## Global Constraints

- Base is exact released `main` SHA `bd8a15ec3e3306b0fbe415064b128fb2e313f6f4`; do not incorporate the stale open draft E.3 PR #26.
- XRF1 version is exactly `1`, flags are exactly `0`, fixed header size is exactly `92`, table entry size is exactly `40`, and the trailing digest is exactly 32 SHA-256 bytes.
- XRF1 integers are canonical unsigned little-endian.
- XOR covers each complete deterministic CMX1 encoding, padded with zero bytes only to the longest encoded member length.
- One symbol covers one exact `RecoveryGroupDescriptor`; groups contain at least two source frames and never span a stream or epoch.
- Recovery supports exactly one known erasure and returns no partial result for zero or multiple erasures.
- Every observed and recovered exact CMX1 byte vector must match its committed length and SHA-256; the recovered vector must decode as exactly one complete member of the missing slot.
- S0/S1/D semantics, CODA bytes, CMX1 bytes/version, E.2 tracker behavior, and E.3 recording/follow behavior remain unchanged.
- No new dependency, network provider, retransmission, authentication, persistence, CLI/C ABI, performance/scale claim, frozen wire-standard claim, or Stage E completion claim.
- Existing global leak detection and isolated ONNX Runtime sanitizer suppression remain unchanged.

---

### Task 1: Establish the absent E.4 API as a clean RED contract

**Files:**
- Create: `tests/test_transport_xor_recovery.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing `<codec/recovery.hpp>`, `<codec/transport.hpp>`, and test harness.
- Produces: compile-time expectations for `<codec/xor_recovery.hpp>`, `XorRepairLimits`, `XorRepairSymbol`, `XorRecoveredFrame`, `create_xor_repair_symbol`, `encode_xor_repair_symbol`, `decode_xor_repair_symbol`, and `recover_xor_single_erasure`.

- [ ] **Step 1: Add the tests-first source with a real desired call path**

Create `tests/test_transport_xor_recovery.cpp` with test helpers that build valid same-stream/same-epoch variable-length frames and compare every field. The first test must contain this API shape:

```cpp
TEST(transport_xor_recovery_round_trips_symbol_and_one_erasure) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);

  auto symbol = codec::create_xor_repair_symbol(descriptor, frames);
  EXPECT_TRUE(symbol);
  auto encoded = codec::encode_xor_repair_symbol(*symbol);
  EXPECT_TRUE(encoded);
  auto decoded = codec::decode_xor_repair_symbol(*encoded);
  EXPECT_TRUE(decoded);

  const std::vector<codec::MultiplexFrame> observed{
      frames[0], frames[2], frames[3]};
  auto recovered = codec::recover_xor_single_erasure(*decoded, observed);
  EXPECT_TRUE(recovered);
  expect_same_frame(recovered->frame, frames[1]);
  EXPECT_EQ(recovered->encoded_frame, exact_encoding(frames[1]));
  EXPECT_EQ(recovered->encoded_frame_hash,
            codec::sha256(recovered->encoded_frame));
}
```

Use four source frames at sequences 40–43 with payload sizes 0, 1, 17, and 257 bytes so parity padding and exact-length truncation are exercised by the first proof.

- [ ] **Step 2: Register only the new test source**

Add `tests/test_transport_xor_recovery.cpp` next to the existing transport recovery tests in `codec_tests`. Do not add the public header or production source yet.

- [ ] **Step 3: Run the Release build and require the expected RED**

Run:

```bash
cmake -S . -B build-red -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build-red --parallel
```

Expected: configuration succeeds and existing production sources compile; compilation of `tests/test_transport_xor_recovery.cpp` fails specifically because `codec/xor_recovery.hpp` does not exist. A dependency/configuration failure is not an accepted RED.

- [ ] **Step 4: Commit and publish the exact RED tree**

```bash
git add CMakeLists.txt tests/test_transport_xor_recovery.cpp
git commit -m "test: define Stage E.4 XOR recovery contract"
git push -u origin automation/stage-e4-xor-recovery
```

Open a draft PR titled `Stage E.4: add bounded XOR frame recovery`, record the exact RED SHA/tree and CI run in PR #30-or-later and roadmap issue #10, and state that no production E.4 API exists at this head.

---

### Task 2: Implement bounded in-memory XOR symbol construction

**Files:**
- Create: `include/codec/xor_recovery.hpp`
- Create: `src/transport/xor_recovery.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/test_transport_xor_recovery.cpp`

**Interfaces:**
- Consumes: `MultiplexLimits`, `encode_multiplex_frame`, `RecoveryGroupDescriptor`, `Sha256`, and `Result`.
- Produces: the public API exactly specified in the design, including default limits of 256 source frames, 64 MiB aggregate encoded source bytes, and 32 MiB complete symbol bytes.

- [ ] **Step 1: Expand RED for canonical source-slot construction and validation**

Add tests that require:

```cpp
TEST(transport_xor_recovery_create_canonicalizes_unordered_frames) {
  auto frames = source_group();
  const auto descriptor = descriptor_for(frames);
  std::swap(frames[0], frames[3]);
  std::swap(frames[1], frames[2]);
  auto symbol = codec::create_xor_repair_symbol(descriptor, frames);
  EXPECT_TRUE(symbol);
  EXPECT_EQ(symbol->encoded_frame_sizes.size(), std::size_t{4});
  EXPECT_EQ(symbol->encoded_frame_hashes.size(), std::size_t{4});
  EXPECT_EQ(symbol->parity.size(), exact_encoding(source_group()[3]).size());
  for (std::size_t i = 0; i < source_group().size(); ++i) {
    const auto bytes = exact_encoding(source_group()[i]);
    EXPECT_EQ(symbol->encoded_frame_sizes[i], bytes.size());
    EXPECT_EQ(symbol->encoded_frame_hashes[i], codec::sha256(bytes));
  }
}
```

Add separate failure tests for source count below 2, descriptor count/input-size mismatch, sequence overflow, duplicate sequence, missing sequence, out-of-range sequence, wrong stream, wrong connection epoch, wrong format epoch, inverted interval/invalid CMX1 timebase, zero E.4 limits, source-frame count exhaustion, one-frame byte exhaustion, and aggregate-byte exhaustion.

- [ ] **Step 2: Add the exact public header**

Create `include/codec/xor_recovery.hpp` with the structures and four functions copied exactly from the design spec. Include only `<codec/integrity.hpp>`, `<codec/recovery.hpp>`, and the standard headers required by the declarations.

- [ ] **Step 3: Implement shared checked validation and source encoding**

In `src/transport/xor_recovery.cpp`:

- define constants for fixed header 92, table entry 40, trailer 32, and `XRF1` magic;
- add checked unsigned addition/multiplication helpers;
- validate all `XorRepairLimits` and the composed `MultiplexLimits` without allocating;
- validate `RecoveryGroupDescriptor` count/range and produce the exclusive end sequence;
- map source frames transactionally to canonical slot indices;
- call `encode_multiplex_frame(frame, limits.multiplex)` for each slot;
- enforce per-frame and aggregate encoded-byte limits with checked arithmetic;
- compute exact `Sha256` per slot and XOR bytes into a parity vector whose size is the longest encoding;
- catch `std::bad_alloc` and return `resource_exhausted` without exposing partial state.

Use `std::vector<std::optional<std::vector<std::byte>>>` or an equivalent explicit occupied-slot representation so duplicates and missing slots cannot be mistaken for empty-payload frames.

- [ ] **Step 4: Register production and verify focused GREEN**

Add `src/transport/xor_recovery.cpp` beside the existing transport sources. Run:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
./build/codec_tests --filter transport_xor_recovery_create
```

Expected: all construction/validation tests pass. If the test harness does not accept `--filter`, run `./build/codec_tests` and require the full suite to pass.

- [ ] **Step 5: Commit the construction boundary**

```bash
git add include/codec/xor_recovery.hpp src/transport/xor_recovery.cpp \
  tests/test_transport_xor_recovery.cpp CMakeLists.txt
git commit -m "feat: build bounded XOR repair symbols"
```

---

### Task 3: Implement strict deterministic XRF1 encoding and decoding

**Files:**
- Modify: `src/transport/xor_recovery.cpp`
- Modify: `tests/test_transport_xor_recovery.cpp`

**Interfaces:**
- Consumes: a structurally valid `XorRepairSymbol` and caller limits.
- Produces: deterministic canonical XRF1 bytes and a verified owned `XorRepairSymbol` decoded from one exact XRF1 byte sequence.

- [ ] **Step 1: Add deterministic wire RED tests**

Require two encodes of the same symbol to compare byte-for-byte equal and assert:

```cpp
EXPECT_EQ((*encoded)[0], std::byte{'X'});
EXPECT_EQ((*encoded)[1], std::byte{'R'});
EXPECT_EQ((*encoded)[2], std::byte{'F'});
EXPECT_EQ((*encoded)[3], std::byte{'1'});
EXPECT_EQ(get_u16(*encoded, 4), codec::xor_repair_symbol_version);
EXPECT_EQ(get_u16(*encoded, 6), std::uint16_t{0});
EXPECT_EQ(get_u32(*encoded, 8), std::uint32_t{92});
EXPECT_EQ(get_u64(*encoded, 12), encoded->size());
EXPECT_EQ(get_u32(*encoded, 84), std::uint32_t{40});
EXPECT_EQ(get_u32(*encoded, 88), std::uint32_t{0});
```

Decode and compare the exact descriptor, length vector, hash vector, and parity vector. Independently compute SHA-256 over every byte except the final 32 and compare it to the trailer.

- [ ] **Step 2: Add wire failure RED tests**

Create one helper that mutates an encoded symbol and optionally recomputes its final digest. Cover bad magic, version, flags, fixed-header size, total size, table-entry size, reserved field, source count below 2, source-range overflow, zero/oversized frame lengths, inconsistent parity size, symbol-size bound exhaustion, truncation at representative header/table/parity/trailer positions, appended trailing byte, and final digest mismatch.

For semantic-invalid cases, recompute the final digest so the test proves invariant validation rather than only the outer hash.

- [ ] **Step 3: Implement canonical encoder**

Preflight `92 + source_count * 40 + parity.size() + 32` with checked arithmetic and all caller limits. Revalidate vector counts, every frame size, aggregate bytes, parity=max(frame sizes), group range, and limits before allocation. Fill a zero-initialized vector using private little-endian helpers, copy the table and parity, compute `sha256(output.first(output.size() - 32))`, and place the digest in the trailer.

- [ ] **Step 4: Implement strict decoder**

Validate the fixed header from the caller span before allocating. Preflight declared source count/table/parity/total sizes and caller bounds, require the exact canonical total to equal `bytes.size()`, verify the trailer digest, then copy owned slot metadata and parity. Reuse the same structural symbol validator so an encoded and decoded symbol obey identical invariants.

- [ ] **Step 5: Run focused and full Release GREEN, then commit**

```bash
cmake --build build --parallel
./build/codec_tests
ctest --test-dir build --output-on-failure
git add src/transport/xor_recovery.cpp tests/test_transport_xor_recovery.cpp
git commit -m "feat: encode and verify XRF1 repair symbols"
```

Expected: every current test passes; no output warning or failure is accepted.

---

### Task 4: Implement exact one-erasure recovery and E.1/E.2 integration

**Files:**
- Modify: `src/transport/xor_recovery.cpp`
- Modify: `tests/test_transport_xor_recovery.cpp`

**Interfaces:**
- Consumes: one verified/in-memory `XorRepairSymbol`, exactly `source_count - 1` observed `MultiplexFrame`s, and caller limits.
- Produces: `XorRecoveredFrame` containing the exact recovered CMX1 bytes, decoded missing frame, and verified encoded-frame SHA-256.

- [ ] **Step 1: Add exhaustive position and ordering RED tests**

For every missing index in the four-member variable-length group, build observed frames from all other indices, reverse their order on alternating cases, recover, and require:

```cpp
const auto expected_bytes = exact_encoding(frames[missing]);
EXPECT_EQ(recovered->encoded_frame, expected_bytes);
EXPECT_EQ(recovered->encoded_frame_hash, codec::sha256(expected_bytes));
expect_same_frame(recovered->frame, frames[missing]);
```

This proves zero-padding is truncated using the exact committed slot length, including when the missing member is shorter than the parity vector.

- [ ] **Step 2: Add fail-closed recovery RED tests**

Cover zero erasures (all members supplied), multiple erasures, duplicate observed slot, wrong stream, wrong connection/format epoch, out-of-range sequence, changed payload under an otherwise correct slot, invalid CMX1 metadata, corrupted in-memory parity, corrupted slot length, corrupted slot hash, aggregate working-byte limit exhaustion, and a reconstructed byte sequence that has a recomputed slot hash but fails CMX1 decode/membership.

- [ ] **Step 3: Add real E.2 integration RED**

Begin a `RecoveryGroupTracker` with the same descriptor, observe all source frames except sequence 41, seal it, and require `sealed_incomplete` with exactly `[41,42)`. Recover through XRF1 and require the returned frame sequence is 41 and its exact encoding equals the original. Do not mutate the sealed E.2 tracker or claim automatic repair persistence.

- [ ] **Step 4: Implement transactional exact recovery**

Revalidate the symbol and exact expected observed count. Map observed frames to unique canonical slots, encode with the configured `MultiplexLimits`, enforce aggregate bytes, and compare each exact size/hash. Copy parity into a local vector, XOR every observed exact byte, identify the single absent slot, truncate to its declared length, and verify its expected hash.

Pass the recovered vector to a new `MultiplexDecoder{limits.multiplex}`. Require `push()` returns exactly one frame, `finish()` succeeds, no buffered bytes remain, and decoded stream/connection epoch/format epoch/sequence match the missing slot. Only then move the owned bytes and frame into `XorRecoveredFrame`.

- [ ] **Step 5: Run full Release GREEN and commit**

```bash
cmake --build build --parallel
./build/codec_tests
ctest --test-dir build --output-on-failure
git add src/transport/xor_recovery.cpp tests/test_transport_xor_recovery.cpp
git commit -m "feat: recover one exact CMX1 erasure"
```

---

### Task 5: Prove installed use and synchronize runtime truth

**Files:**
- Modify: `tests/package_consumer/main.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: installed `codec::codec` target and public `<codec/xor_recovery.hpp>`.
- Produces: an installed-package compile/link/runtime proof and truthful E.4 status wording.

- [ ] **Step 1: Add installed-package use**

Include `<codec/xor_recovery.hpp>` in `tests/package_consumer/main.cpp`. Create two minimal valid same-stream/epoch frames, create an XRF1 symbol, encode/decode it, recover the second frame from the first, and return nonzero if any `Result` fails or the recovered payload/sequence differs. Keep the existing consumer proofs intact.

- [ ] **Step 2: Update README runtime truth**

In the implemented transport/recovery list and Stage E prose, add one bounded statement: CODEC implements deterministic XRF1 XOR symbols and exact one-erasure CMX1 recovery with committed encoded lengths/hashes and strict bounds. State in the same paragraph that this is one known erasure only, integrity is not authentication, and no network, automatic persistence, multi-erasure coding, streaming repair, benchmark, or Stage E completion is implied.

- [ ] **Step 3: Add one Unreleased changelog entry**

Under `## Unreleased`, add one E.4 bullet describing the exact capability and all material non-claims. Do not edit the released `0.2.0` history.

- [ ] **Step 4: Verify installed package and capabilities**

```bash
cmake --install build --prefix "$PWD/build/install"
cmake -S tests/package_consumer -B build/package-consumer -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/build/install"
cmake --build build/package-consumer --parallel
./build/package-consumer/codec_package_consumer
./build/codec capabilities
```

Expected: consumer exits 0. Capability JSON remains byte-for-byte semantically unchanged for neural/GPU and does not invent an unimplemented recovery capability flag.

- [ ] **Step 5: Commit documentation and package proof**

```bash
git add tests/package_consumer/main.cpp README.md CHANGELOG.md
git commit -m "docs: record Stage E.4 recovery scope"
```

---

### Task 6: Run exact-tree verification, publish GREEN, and merge guarded

**Files:**
- Modify only if a verified defect requires a new failing regression test first.

**Interfaces:**
- Consumes: complete E.4 branch.
- Produces: exact local SHA/tree proof, exact-head CI, guarded squash merge, post-merge CI, and roadmap continuity evidence.

- [ ] **Step 1: Run required Release verification from a clean build directory**

```bash
cmake -S . -B build-final -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build-final --parallel
ctest --test-dir build-final --output-on-failure
./build-final/codec_tests
./build-final/codec capabilities
```

- [ ] **Step 2: Run required ASan/UBSan verification**

```bash
cmake -S . -B build-san -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCODEC_WARNINGS_AS_ERRORS=ON \
  -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel
ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --test-dir build-san --output-on-failure
```

Use local `detect_leaks=0` only if the managed container reproduces the documented LeakSanitizer `/proc` restriction. Remote exact-head CI must retain `detect_leaks=1`, keep ordinary tests unsuppressed, and pass the existing isolated ONNX Runtime rule unchanged.

- [ ] **Step 3: Run final package and diff audits**

Repeat the installed-package consumer against `build-final/install`. Then run:

```bash
git diff --check origin/main...HEAD
git status --short
git diff --stat origin/main...HEAD
git diff --name-status origin/main...HEAD
git log --oneline origin/main..HEAD
rg -n 'TBD|TODO|placeholder|Stage E complete|multi-erasure' \
  include/codec/xor_recovery.hpp src/transport/xor_recovery.cpp \
  tests/test_transport_xor_recovery.cpp README.md CHANGELOG.md \
  docs/superpowers/specs/2026-08-29-stage-e4-xor-recovery-design.md
git rev-parse HEAD
git rev-parse HEAD^{tree}
```

Require a clean worktree, only intended files, no generated artifacts, and no claim beyond the tests.

- [ ] **Step 4: Publish the exact tested head and require all CI green**

Push without force, mark the PR ready, and update its body with base SHA, accepted RED SHA/run, final tested SHA/tree, commands/results, public API, truth/CODA/CMX1 boundaries, and non-claims. Require GCC build/test/install/package-consumer, Clang build/test/install/package-consumer, and leak-enabled sanitizer jobs to complete success on that exact head.

- [ ] **Step 5: Squash-merge only with the exact-head guard**

Before merge, re-fetch `main`, ensure it has not moved from the reviewed base or rebase/reverify if it has, and ensure the PR head still equals the exact tested SHA. Squash-merge with the expected head SHA; never merge a changed or partially green head.

- [ ] **Step 6: Verify published main and record roadmap completion**

Require the published merge commit's tree to equal the exact tested PR tree and its GitHub verification to be valid. Require push-triggered main CI to pass the same matrix. Append issue #10 with base, RED, final tested head/tree, exact-head CI, merged main SHA/tree, post-merge CI, delivered E.4 API/claim, preserved boundaries, and the next smallest Stage E dependency. Do not declare Stage E complete.

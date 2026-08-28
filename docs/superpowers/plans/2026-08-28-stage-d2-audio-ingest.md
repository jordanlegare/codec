# Stage D.2 Preservation-First Audio PCM16 WAV Ingest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded Audio Profile ingest operation that persists one exact WAV source snapshot as S0 before conditionally adding D.1 APS1 S1 and exact provenance.

**Architecture:** A new profile-only public header and focused implementation reuse the existing hardened capture path and generic CODA writer. WAV parsing is factored into one internal byte-span decoder so the persisted S0 bytes and interpreted bytes are the same owned snapshot; unsupported media returns a finalized S0-only report rather than discarding preservation.

**Tech Stack:** C++20, existing `PreparedCapture`, CODA writer/query/extraction/provenance APIs, D.1 `Pcm16State`/APS1, CMake, custom unit harness, CTest, ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-d2-audio-ingest-design.md`

## Global Constraints

- Capture and append exact S0 before invoking any WAV/PCM interpretation.
- Decode and persist from one owned byte snapshot; never reopen the source for profile processing.
- APS1 remains the sole PCM16 S1 payload, and S1 exists only with exact `state_exact` provenance.
- Invalid WAV content finalizes an S0-only archive and returns an explicit `profile_error` in a successful report.
- Validation and capture failure create no archive; existing destinations are never replaced.
- Keep the entire API in `codec::profiles::audio`; do not change generic core structures, root audio ABI, CODA bytes, CLI, C ABI, or legacy feed behavior.
- Reuse existing capture schemes and their security policy; persist no source URI automatically.
- Add no FLAC, conversion, resampling, model/runtime, inference, identity, recovery, transaction rollback, scale, deployment, frozen CODA v1, or Stage D completion claim.

## File Structure

- Create `include/codec/profiles/audio_ingest.hpp`: request, report, and profile ingest API.
- Modify `include/codec/profiles/audio.hpp`: include the new canonical profile boundary.
- Create `src/audio/wav_codec.hpp`: non-installed byte-span decoder declaration.
- Modify `src/audio/wav.cpp`: move the existing parser behind the shared internal decoder without behavior change.
- Create `src/audio/pcm16_ingest.cpp`: validation, capture, preservation-first archive workflow, and conditional S1 persistence.
- Modify `CMakeLists.txt`: compile the focused ingest implementation.
- Modify `tests/test_audio_profile.cpp`: exact success, S0-only failure isolation, validation/security, and facade proofs.
- Modify `README.md` and `CHANGELOG.md`: state only the proven D.2 boundary.
- Create ignored `build-stage-d2-consumer-src/`: installed-package proof; do not commit it.

---

### Task 1: Add exact-snapshot success-path ingest

**Files:**
- Modify: `tests/test_audio_profile.cpp`
- Create: `include/codec/profiles/audio_ingest.hpp`
- Modify: `include/codec/profiles/audio.hpp`
- Create: `src/audio/wav_codec.hpp`
- Modify: `src/audio/wav.cpp`
- Create: `src/audio/pcm16_ingest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: hardened generic capture, generic descriptor/S0/PCM/provenance records, and D.1 APS1 APIs.
- Produces: `Pcm16WavIngestRequest`, `Pcm16WavIngestReport`, and `ingest_pcm16_wav()` in `codec::profiles::audio`.

- [ ] **Step 1: Add hand-derived WAV and filesystem test helpers**

In `tests/test_audio_profile.cpp`, add `<fstream>`, `<optional>`, and
`<utility>`. Add a `write_bytes(path, bytes)` helper and a literal
`pcm16_wav_fixture()` containing:

- RIFF/WAVE header with file size 62 and RIFF size 54;
- a 16-byte PCM format chunk: two channels, 48,000 Hz, 192,000 byte rate,
  block alignment 4, and 16 bits;
- an unknown one-byte `JUNK` chunk plus its zero pad byte; and
- a data chunk with signed samples `{-32768, -1, 1, 32767}`.

Write every expected byte literally. Do not generate the fixture with
`WavPcm16::write` or production encoding helpers.

- [ ] **Step 2: Write the failing exact success-path test**

Add `audio_pcm16_wav_ingest_preserves_one_snapshot_and_proves_s1`. It must:

1. write the literal fixture to a temporary source and remove the destination;
2. build a request with `source_uri = source.string()`, an audio descriptor
   whose payload type is exactly `audio/wav`, interval `[100, 200]`, and a
   deterministic stream ID;
3. call `audio_profile::ingest_pcm16_wav(request)`;
4. require outer success, `state_exact()`, both optional records, and no
   `profile_error`;
5. open the archive, require `verify().ok` and `verify().finalized`;
6. list the descriptor and compare every caller field;
7. extract `source_bytes` and compare to the literal fixture byte-for-byte;
8. extract/decode `pcm16` and compare rate/channels/samples to literal values;
9. query `state_exact` provenance and compare subject/input
   stream/type/sequence/hash to the report records;
10. compare process identity to `audio.pcm16.canonicalize`,
    `codec-audio-profile`, version `1`, and created time `200`; and
11. clean up both files.

The production breaks caught are regenerating S0 from decoded PCM, reopening a
different source snapshot, omitting descriptor or APS1 storage, wrong record
ordering/interval, wrong provenance links, or a false S1 report.

- [ ] **Step 3: Run the test build to verify RED**

```bash
/tmp/codec-d2-tools/cmake/data/bin/cmake -S . \
  -B /tmp/codec-stage-d2-red -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
CMAKE_BUILD_PARALLEL_LEVEL=1 \
  /tmp/codec-d2-tools/cmake/data/bin/cmake --build \
  /tmp/codec-stage-d2-red --target codec_tests
```

Expected: compilation fails only because the D.2 request/report/function do
not exist.

- [ ] **Step 4: Add the profile-only public boundary**

Create `include/codec/profiles/audio_ingest.hpp` with the exact request/report
types and defaults from the spec. Include `<codec/archive.hpp>`, `<cstddef>`,
`<cstdint>`, `<filesystem>`, `<optional>`, and `<string>`. Declare:

```cpp
Result<Pcm16WavIngestReport> ingest_pcm16_wav(
    const Pcm16WavIngestRequest& request);
```

Modify `include/codec/profiles/audio.hpp` to include this header. Do not add
root-namespace aliases.

- [ ] **Step 5: Factor the WAV parser over exact bytes**

Create `src/audio/wav_codec.hpp`:

```cpp
#pragma once
#include <codec/audio.hpp>
#include <cstddef>
#include <span>

namespace codec::detail {
Result<WavPcm16> decode_wav_pcm16(
    std::span<const std::byte> encoded);
}
```

In `src/audio/wav.cpp`, retain the existing literal checks and error messages,
but move the parsing body after file reading into that function. Make
`WavPcm16::read` exactly:

```cpp
auto file = detail::read_file(path, 1024ULL * 1024ULL * 1024ULL);
if (!file) return file.error();
return detail::decode_wav_pcm16(*file);
```

Do not broaden accepted WAV formats or add a public bytes decoder.

- [ ] **Step 6: Implement the minimal valid-input workflow**

Create `src/audio/pcm16_ingest.cpp`. For this task, implement enough validation
for the valid request, capture into one owned `std::vector<std::byte>`, create
the archive, append descriptor then exact source, decode the same vector,
canonicalize/encode D.1 state, append `RecordType::pcm16`, append fixed
state-exact provenance, finalize, and return the complete report.

Use these capture options directly from the request:

```cpp
detail::CaptureOptions{
    .chunk_bytes = request.capture_chunk_bytes,
    .maximum_bytes = request.maximum_source_bytes,
    .maximum_redirects = request.maximum_redirects,
    .deny_private_network = request.deny_private_network,
}
```

The capture sink appends the received span unchanged. Use this fixed process:

```cpp
ProvenanceProcess{
    .operation = "audio.pcm16.canonicalize",
    .implementation_id = "codec-audio-profile",
    .implementation_version = "1",
    .implementation_hash = std::nullopt,
    .configuration_hash = std::nullopt,
    .created_utc_ns = request.end_ns,
    .details_type = {},
    .details = {},
}
```

For invalid media in this first minimal slice, return the decoder error; Task 2
adds preservation-first partial success after its failing proof exists.

- [ ] **Step 7: Register the source and verify GREEN**

Add `src/audio/pcm16_ingest.cpp` beside other audio sources in
`CMakeLists.txt`. Build and run `/tmp/codec-stage-d2-red/codec_tests`.

Expected: 91 tests, 0 failures, including all pre-D.2 tests.

- [ ] **Step 8: Commit the successful exact-snapshot workflow**

```bash
git add CMakeLists.txt include/codec/profiles/audio.hpp \
  include/codec/profiles/audio_ingest.hpp src/audio/wav.cpp \
  src/audio/wav_codec.hpp src/audio/pcm16_ingest.cpp \
  tests/test_audio_profile.cpp
git commit -m "feat: ingest exact PCM16 WAV snapshots"
```

### Task 2: Preserve finalized S0 when profile interpretation fails

**Files:**
- Modify: `tests/test_audio_profile.cpp`
- Modify: `src/audio/pcm16_ingest.cpp`

**Interfaces:**
- Consumes: Task 1 exact capture and archive record workflow.
- Produces: explicit S0-only `profile_error` reports for WAV decode/canonicalization/APS1 failures.

- [ ] **Step 1: Write the failing invalid-media preservation test**

Add `audio_pcm16_wav_ingest_finalizes_s0_when_profile_decode_fails` using the
literal bytes `not a PCM16 WAV; preserve exactly`. Require:

- outer ingest success;
- `state_exact() == false`;
- `state` and `provenance` absent;
- `profile_error` present with `ErrorCode::decode`;
- an archive verification report with `ok` and `finalized` true;
- exact independent S0 extraction equal to the invalid bytes; and
- empty PCM record and provenance queries.

The break caught is interpreting before preservation, treating profile failure
as capture failure, leaving an unfinalized archive, or retaining an unproven
PCM record.

- [ ] **Step 2: Run the unit binary to verify RED**

Expected: the ingest outer result fails or the archive is not finalized,
because Task 1 has no source-only completion path.

- [ ] **Step 3: Add source-only finalization**

After descriptor and exact S0 append, route errors from
`decode_wav_pcm16`, `canonicalize_pcm16`, or `encode_pcm16_state` through one
local helper that:

1. finalizes the writer;
2. returns any finalization error as the outer error; and
3. otherwise returns the report with the original profile error, absent state,
   and absent provenance.

Do not append a `pcm16` record before all three profile stages succeed.

- [ ] **Step 4: Verify GREEN**

Build/run the unit binary. Expected: 92 tests, 0 failures.

- [ ] **Step 5: Run the preservation-order mutation proof**

Temporarily return the decoder error before source-only finalization. Rebuild
and run: the invalid-media test must fail because outer success/finalized S0 is
missing. Restore preservation-first handling and confirm 92 tests, 0 failures.

- [ ] **Step 6: Commit failure isolation**

```bash
git add src/audio/pcm16_ingest.cpp tests/test_audio_profile.cpp
git commit -m "test: preserve S0 across audio profile failure"
```

### Task 3: Prove validation, capture isolation, and no replacement

**Files:**
- Modify: `tests/test_audio_profile.cpp`
- Modify: `src/audio/pcm16_ingest.cpp`

**Interfaces:**
- Consumes: Task 2 report semantics.
- Produces: deterministic preflight validation and capture/output security behavior.

- [ ] **Step 1: Write the failing preflight and capture-boundary test**

Add `audio_pcm16_wav_ingest_rejects_invalid_or_unbounded_requests_before_output`.
Use a helper that removes a unique destination, invokes the request, requires
outer failure, and requires the destination to remain absent. Cover these
literal cases:

- empty source URI;
- empty or filename-less archive path;
- inverted interval;
- chunk size 4095 and 16 MiB + 1;
- zero maximum bytes;
- 21 redirects;
- descriptor type `telemetry`;
- descriptor payload type other than `audio/wav`;
- empty descriptor source ID; and
- a valid fixture whose maximum is one byte smaller than the source.

Then pre-create a destination with literal sentinel bytes, invoke a valid
request, require `ErrorCode::archive_io`, and compare the sentinel unchanged.

The break caught is opening/capturing before request validation, publishing a
partial archive on bounded capture failure, accepting an untruthful audio
descriptor, or replacing existing data.

- [ ] **Step 2: Run the unit binary to verify RED**

Expected: at least the Audio Profile type/payload checks or chunk bounds are
accepted by the minimal Task 1 implementation.

- [ ] **Step 3: Implement deterministic preflight**

Add a private `validate_request` that runs before `PreparedCapture::prepare`.
It must implement every request condition in the design spec and call
`detail::encode_stream_descriptor` to reuse generic descriptor bounds.

Use `std::filesystem::symlink_status(path, error)` for an early destination
collision check that treats every existing entry, including symlinks, as a
collision. Retain `CodaWriter::create` as the race-safe final authority.

The capture vector sink must independently guard size arithmetic before
`insert`, even though `PreparedCapture` enforces the configured byte limit.

- [ ] **Step 4: Verify GREEN**

Build/run the unit binary. Expected: 93 tests, 0 failures.

- [ ] **Step 5: Run the missing-provenance mutation proof**

Temporarily omit the `append_stream_provenance` call while leaving the PCM
record. Rebuild/run: the exact success-path test must fail through report or
query assertions. Restore it and confirm 93 tests, 0 failures.

- [ ] **Step 6: Commit validation and security coverage**

```bash
git add src/audio/pcm16_ingest.cpp tests/test_audio_profile.cpp
git commit -m "test: bound audio ingest publication"
```

### Task 4: Synchronize claims and prove installed consumption

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Create ignored: `build-stage-d2-consumer-src/CMakeLists.txt`
- Create ignored: `build-stage-d2-consumer-src/main.cpp`

**Interfaces:**
- Consumes: complete D.2 public profile boundary.
- Produces: truthful status plus independent installed-package evidence.

- [ ] **Step 1: Update current-status documentation**

Add one implemented Audio Profile bullet and one Unreleased changelog entry.
State the bounded single-snapshot, S0-first, conditional-S1 behavior and
explicit finalized S0-only profile-error result.

Keep the explicit nonclaims from the spec. Do not call D.2 filesystem-atomic,
general media ingest, streaming PCM processing, or Stage D completion.

- [ ] **Step 2: Create the ignored installed consumer**

Use `git check-ignore` to prove `build-stage-d2-consumer-src/` is ignored.
Create a CMake consumer that finds installed `codec 0.1`, links
`codec::codec`, writes a valid WAV through installed `WavPcm16`, calls
installed `codec::profiles::audio::ingest_pcm16_wav`, opens the result, and
requires exact state plus provenance and exact independently extractable S0.

Return a distinct non-zero code for every failure. Remove consumer-created
files on success.

- [ ] **Step 3: Run the fresh Release and install proof**

Use fresh directories outside the worktree:

```bash
/tmp/codec-d2-tools/cmake/data/bin/cmake -S . \
  -B /tmp/codec-stage-d2-release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON \
  -DCMAKE_INSTALL_PREFIX=/tmp/codec-stage-d2-install
CMAKE_BUILD_PARALLEL_LEVEL=1 \
  /tmp/codec-d2-tools/cmake/data/bin/cmake --build \
  /tmp/codec-stage-d2-release
/tmp/codec-d2-tools/cmake/data/bin/ctest \
  --test-dir /tmp/codec-stage-d2-release --output-on-failure
/tmp/codec-stage-d2-release/codec_tests
/tmp/codec-stage-d2-release/codec capabilities
/tmp/codec-d2-tools/cmake/data/bin/cmake --install \
  /tmp/codec-stage-d2-release
```

Expected: CTest 4/4, direct unit 93/93, and capabilities retain
`neural_separation:false` and `gpu_inference:false`.

Configure/build/run the ignored consumer using only
`CMAKE_PREFIX_PATH=/tmp/codec-stage-d2-install`; expected exit 0.

- [ ] **Step 4: Run fresh Debug ASan/UBSan proof**

```bash
/tmp/codec-d2-tools/cmake/data/bin/cmake -S . \
  -B /tmp/codec-stage-d2-sanitized -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Debug -DCODEC_WARNINGS_AS_ERRORS=ON \
  -DCODEC_ENABLE_SANITIZERS=ON
CMAKE_BUILD_PARALLEL_LEVEL=1 \
  /tmp/codec-d2-tools/cmake/data/bin/cmake --build \
  /tmp/codec-stage-d2-sanitized
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/codec-d2-tools/cmake/data/bin/ctest \
  --test-dir /tmp/codec-stage-d2-sanitized --output-on-failure
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/codec-stage-d2-sanitized/codec_tests
```

Expected: CTest 4/4 and direct unit 93/93.

- [ ] **Step 5: Commit truthful status**

```bash
git add README.md CHANGELOG.md
git commit -m "docs: mark preservation-first audio ingest implemented"
```

### Task 5: Audit and publish the exact D.2 tree

**Files:**
- Verify only; no intended source changes.

**Interfaces:**
- Consumes: final committed D.2 tree.
- Produces: one non-force Stage D.2 commit on `main`, exact-SHA CI evidence, and roadmap continuity.

- [ ] **Step 1: Re-run exact-HEAD verification and audit**

Require a clean worktree. Re-run Release CTest/direct units and sanitizer
CTest/direct units against exact HEAD. Audit `git diff --check`, the full
base-to-head diff, public namespace placement, truth claims, ignored artifacts,
and absence of credentials/build output.

- [ ] **Step 2: Reconcile publication continuity**

Re-fetch GitHub and require:

- remote `main` still equals
  `2895646df55e9ec4737cc737bad490001d9008d9`;
- no open PR overlaps D.2;
- issue #10 remains the unique exact-title roadmap log; and
- the D.1 exact-head CI evidence remains green.

Stop rather than force if the base moved.

- [ ] **Step 3: Publish the exact tested tree non-force**

Publish the local exact tree as one commit with parent D.1 and message:

```text
Stage D.2: add preservation-first Audio PCM16 ingest
```

Before updating `main`, compare the GitHub-created tree SHA to local
`HEAD^{tree}`. Update `main` with `force:false`, fetch, and fast-forward the
local main checkout.

- [ ] **Step 4: Require exact-SHA CI and record completion**

Poll only the published SHA until exactly these jobs complete successfully:

- `build (gcc)`
- `build (clang)`
- `sanitizers`

Add an issue #10 completion entry with base/head/tree, BEFORE/AFTER contract,
S0/S1 scope, local commands/results, mutation proofs, installed-consumer proof,
CI URL/jobs, nonclaims, remaining Stage D exit evidence, and the next smallest
Audio Profile dependency. Do not claim Stage D complete.

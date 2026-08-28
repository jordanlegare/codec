# Stage D.7 Bounded Offline PCM16 Separation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add bounded offline Audio Profile separation over D.3-verified PCM16 S1 that returns caller-persistable D stems and residual with exact S1 support, independent reconstruction metrics, and model/runtime/configuration identity without claiming a bundled neural runtime.

**Architecture:** A new profile-only wrapper validates an explicit interval and bounded request, queries D.3, reads each exact APS1 state record, and invokes a private one-shot `StreamProcessor` adapter around the existing caller-supplied `SeparationBackend`. The adapter validates the backend result, encodes every derived PCM value as APS1 with D truth and typed process metadata, and the wrapper returns ordered in-memory results without archive mutation.

**Tech Stack:** C++20, existing CODEC `Result`, CODA archive/query/provenance APIs, Audio Profile APS1 codec, generic `StreamProcessor`/`invoke_processor`, CMake/CTest, custom unit harness.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-d7-offline-pcm16-separation-design.md`

## Global Constraints

- Base is exact published D.6 head `6acc73fad9eb6ccf4fd36b1d9914ee49e9e17005`; do not fold unrelated milestones into this branch.
- D.3 remains the sole S1 classification gate; no physical `pcm16` record may be promoted independently.
- S0 and S1 are read-only; every new output is visibly `TruthClass::derived` and retains exact S1 support.
- Do not modify the CODA format, generic processing API, existing separation ABI, CLI, or C ABI.
- Do not add a neural runtime/model dependency or change `neural_separation:false` / `gpu_inference:false`.
- Require a caller-supplied explicit non-empty interval and bounded input/output/source limits.
- Perform no archive, filesystem, or network write in the D.7 function.
- Use the existing APS1 encoding only as a payload representation; it does not make a D artifact S1.
- Preserve caller-visible atomicity: any selected-state/backend/validation failure returns no partial successful vector.

---

## File structure

- Create `include/codec/profiles/audio_offline_separation.hpp`: additive profile API and data contracts only.
- Create `src/audio/offline_pcm16_separation.cpp`: request validation, D.3 orchestration, private processor adapter, backend-result validation, metric/config/detail encoding, and result assembly.
- Create `tests/test_audio_offline_separation.cpp`: actual D.2/D.3 integration plus fake-backend success, bounds, failure, and immutability proofs.
- Modify `include/codec/profiles/audio.hpp`: export the new profile header through the canonical facade.
- Modify `CMakeLists.txt`: compile the new source and test.
- Modify `tests/package_consumer/main.cpp`: prove the installed facade exports D.7 types without new package dependencies.
- Modify `README.md` and `CHANGELOG.md`: state only the bounded offline orchestration capability and preserved non-claims.

### Task 1: Establish the D.7 RED contract

**Files:**
- Create: `tests/test_audio_offline_separation.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `codec::profiles::audio::ingest_pcm16_wav`, `CodaArchive::open`, `SeparationBackend`.
- Produces: a tests-only compile dependency on `OfflinePcm16SeparationRequest` and `separate_verified_pcm16_offline`.

- [ ] **Step 1: Add the new test translation unit to `codec_tests`**

Add `tests/test_audio_offline_separation.cpp` next to the other Audio Profile tests in `CMakeLists.txt`.

- [ ] **Step 2: Write the compile-failing success contract**

Create a D.2 fixture by writing a small PCM16 WAV and calling
`ingest_pcm16_wav`. Add a deterministic fake backend:

```cpp
class DeterministicBackend final : public codec::SeparationBackend {
 public:
  std::string name() const override { return "test-runtime"; }
  bool available() const noexcept override { return true; }
  codec::Result<codec::SeparationResult> separate(
      const codec::SeparationRequest& request) override {
    codec::WavPcm16 stem = request.mixture;
    codec::WavPcm16 residual = request.mixture;
    for (std::size_t i = 0; i < request.mixture.samples.size(); ++i) {
      stem.samples[i] = static_cast<std::int16_t>(
          request.mixture.samples[i] / 2);
      residual.samples[i] = static_cast<std::int16_t>(
          request.mixture.samples[i] - stem.samples[i]);
    }
    return codec::SeparationResult{
        .stems = {std::move(stem)},
        .residual = std::move(residual),
        .mixture_reconstruction_error = 0.25,
        .model_hash =
            "000102030405060708090a0b0c0d0e0f"
            "101112131415161718191a1b1c1d1e1f",
        .provider = "test-cpu",
    };
  }
};
```

Construct `OfflinePcm16SeparationRequest` with stream, interval `[100, 200)`,
one-result/1 MiB input bounds, `maximum_sources = 2`, non-empty model bundle,
execution time, and 1 MiB output bound. Call
`separate_verified_pcm16_offline` and assert one result, one stem, residual
role, and D truth.

- [ ] **Step 3: Run the tests-only build and capture RED**

Run:

```bash
cmake -S . -B build-red -G Ninja -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build-red --parallel
```

Expected: compilation reaches `tests/test_audio_offline_separation.cpp` and
fails because `OfflinePcm16SeparationRequest` and
`separate_verified_pcm16_offline` do not exist. Existing production sources
must compile first; dependency/configure failures do not count as RED.

- [ ] **Step 4: Commit the accepted RED proof**

```bash
git add CMakeLists.txt tests/test_audio_offline_separation.cpp
git commit -m "test: define Stage D.7 offline separation contract"
```

Record the exact RED SHA and compiler error in issue #10 before implementation.

### Task 2: Implement the successful offline D path

**Files:**
- Create: `include/codec/profiles/audio_offline_separation.hpp`
- Create: `src/audio/offline_pcm16_separation.cpp`
- Modify: `include/codec/profiles/audio.hpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_audio_offline_separation.cpp`

**Interfaces:**
- Consumes: `query_verified_pcm16_states`, `CodaArchive::read_payload`, `decode_pcm16_state`, `encode_pcm16_state`, `SeparationBackend::separate`, and `invoke_processor`.
- Produces: the exact public API in the D.7 spec and ordered successful results whose `ProcessorOutput` values are ready for caller persistence.

- [ ] **Step 1: Declare the additive profile API**

Implement the structs and function signature exactly as specified in the
design. The header includes `codec/inference.hpp`, `codec/processing.hpp`, and
`codec/profiles/audio_state_reader.hpp`, plus only standard headers needed by
the value types.

- [ ] **Step 2: Add private deterministic helpers**

In `src/audio/offline_pcm16_separation.cpp`, add helpers that:

```cpp
constexpr std::size_t maximum_source_ceiling = 64;
constexpr std::size_t maximum_identity_bytes = 2048;
constexpr std::size_t maximum_model_bundle_bytes = 4096;

Result<Sha256> parse_model_hash(std::string_view hex);
Sha256 configuration_hash(const OfflinePcm16SeparationRequest& request);
MixtureReconstructionMetrics reconstruction_metrics(
    const WavPcm16& mixture,
    std::span<const WavPcm16> stems,
    const WavPcm16& residual,
    double backend_reported_error);
std::vector<std::byte> process_details(
    OfflinePcm16ArtifactRole role,
    std::size_t stem_index,
    const MixtureReconstructionMetrics& metrics);
ProvenanceRecordLink exact_link(const RecordInfo& record);
```

`parse_model_hash` accepts exactly 64 ASCII hexadecimal characters and
returns `inference` otherwise. `configuration_hash` hashes a versioned binary
encoding of `model_bundle` and `maximum_sources`. `process_details` writes a
fixed `AOS1` binary payload containing role, stem index, max absolute error,
RMS IEEE-754 bits, and backend-reported IEEE-754 bits.

- [ ] **Step 3: Implement the private one-shot processor**

The private `OfflineSeparationProcessor` must:

```cpp
class OfflineSeparationProcessor final : public StreamProcessor {
 public:
  OfflineSeparationProcessor(SeparationBackend& backend,
                             const OfflinePcm16SeparationRequest& request,
                             std::string backend_name,
                             Sha256 configuration_hash);
  std::string name() const override;
  Result<std::vector<ProcessorOutput>> process(
      std::span<const ExtractedRecord> inputs) override;
  const ProcessedMetadata& metadata() const;
};
```

Require exactly one exact APS1 input, decode it, construct the legacy mixture,
and call the backend with the request's `maximum_sources` and `model_bundle`.
Validate at least one stem, the source-count ceiling, exact geometry for every
stem/residual, bounded non-NUL provider identity, finite non-negative backend
metric, and the 64-hex model hash. Compute independent metrics before output
assembly.

For each stem and then the residual, encode `Pcm16State` with APS1 and return:

```cpp
ProcessorOutput{
    .stream = input.record.stream,
    .type = record_type_code(RecordType::pcm16),
    .start_ns = input.record.start_ns,
    .end_ns = input.record.end_ns,
    .truth = TruthClass::derived,
    .payload = encoded_state,
    .process = ProvenanceProcess{
        .operation = "audio.offline-separation",
        .implementation_id = backend_name + "/" + provider,
        .implementation_version = "1",
        .implementation_hash = model_hash,
        .configuration_hash = request_configuration_hash,
        .created_utc_ns = request.created_utc_ns,
        .details_type = "codec.audio.offline-separation-output.v1",
        .details = role_specific_details,
    },
};
```

Store validated model/provider/metrics/state metadata for wrapper assembly.

- [ ] **Step 4: Implement archive orchestration**

`separate_verified_pcm16_offline` validates before archive scanning:

- non-zero D.3 limits;
- present time interval with `begin_ns < end_ns`;
- source count in `[1, 64]`;
- non-zero aggregate output bytes;
- non-empty model bundle of at most 4096 bytes with no embedded NUL;
- available backend; and
- bounded non-empty backend name with no embedded NUL.

Call D.3, then for each verified state read the exact state record payload,
construct one `ExtractedRecord`, and invoke the private processor with:

```cpp
ProcessorRunLimits{
    .maximum_outputs = request.maximum_sources + 1,
    .maximum_output_bytes = remaining_aggregate_bytes,
}
```

Map outputs `[0, stems)` to stem artifacts and the last output to residual.
Decode each returned APS1 payload for the artifact `state`, attach the exact
D.3 state link, and copy the full `VerifiedPcm16State` into the enclosing run.
Accumulate bytes with checked subtraction. Build the return vector locally and
return only after every selected run succeeds.

- [ ] **Step 5: Export through the facade and build graph**

Add the new source to `codec_core`, include the public header from
`codec/profiles/audio.hpp`, and do not add root-level aliases because the new
workflow is profile-only.

- [ ] **Step 6: Run the focused success test**

Run:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
./build/codec_tests
```

Expected: D.7 integration passes and all existing direct unit tests pass.

- [ ] **Step 7: Commit the successful D path**

```bash
git add CMakeLists.txt include/codec/profiles/audio.hpp \
  include/codec/profiles/audio_offline_separation.hpp \
  src/audio/offline_pcm16_separation.cpp \
  tests/test_audio_offline_separation.cpp
git commit -m "feat: orchestrate bounded offline PCM16 separation"
```

### Task 3: Prove fail-closed bounds and backend validation

**Files:**
- Modify: `tests/test_audio_offline_separation.cpp`
- Modify: `src/audio/offline_pcm16_separation.cpp`

**Interfaces:**
- Consumes: the D.7 API from Task 2.
- Produces: explicit pre-invocation validation, resource exhaustion,
  unavailable-model propagation, malformed-backend rejection, and atomic
  multi-state failure behavior.

- [ ] **Step 1: Add request-validation and unavailable-backend tests**

Use a counting backend and assert zero calls for: absent interval, empty or
inverted interval, zero input/result/output bounds, zero or over-ceiling source
count, empty/oversized/NUL model bundle, unavailable backend, and empty/NUL or
oversized backend name. The default backend case must return
`ErrorCode::model_incompatible`.

- [ ] **Step 2: Add malformed-success table coverage**

Add configurable fake results and assert `ErrorCode::inference` for: zero
stems, excessive stems, stem sample-rate/channel/sample-count mismatch,
residual geometry mismatch, empty/NUL/oversized provider, non-hex/wrong-length
model hash, negative reconstruction error, NaN, and infinity.

- [ ] **Step 3: Add bounds and atomicity coverage**

Assert `resource_exhausted` when one successful run's APS1 output exceeds the
aggregate output budget and when later results exhaust the cumulative budget.
Create a finalized archive with two verified states; make the second backend
call fail and assert the public result is an error with no returned partial
vector. Verify backend call order follows D.3 archive order.

- [ ] **Step 4: Add reconstruction and immutability assertions**

Use a fake result where `mixture - (stem + residual)` is exactly `+1/-1` across
samples. Assert maximum absolute error `1`, RMS `1.0`, and a deliberately
different backend-reported value. Snapshot the input archive bytes before and
after success and failure, then assert equality and finalized verification.

- [ ] **Step 5: Implement only validation required by the failing tests**

Keep malformed backend-success errors at `inference`, caller request errors at
`invalid_argument`, caller output budgets at `resource_exhausted`, and backend
errors unchanged. Use checked arithmetic; do not clamp, fabricate residuals,
or silently skip a selected state.

- [ ] **Step 6: Run the complete direct unit suite**

Run `cmake --build build --parallel && ./build/codec_tests`.

Expected: all D.7 failure tests and every existing direct unit test pass.

- [ ] **Step 7: Commit validation proof**

```bash
git add src/audio/offline_pcm16_separation.cpp tests/test_audio_offline_separation.cpp
git commit -m "test: enforce offline separation evidence bounds"
```

### Task 4: Package, document, and verify the milestone

**Files:**
- Modify: `tests/package_consumer/main.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: complete D.7 public API and implementation.
- Produces: installed-package compile evidence and truthful runtime manifest.

- [ ] **Step 1: Extend installed-package consumption**

Instantiate `OfflinePcm16SeparationRequest` and
`OfflinePcm16SeparationLimits` through `<codec/profiles/audio.hpp>` and assert
their default non-zero bounds in the external consumer. Do not require or
claim an available backend.

- [ ] **Step 2: Update runtime truth documentation**

Add one `implemented_v0_1.audio_profile` bullet and one Audio Profile section
paragraph covering: D.3-only selection, explicit intervals/bounds,
caller-supplied backend, D stems/residual, independent reconstruction metrics,
model/runtime/config identity, exact S1 support, and no automatic persistence.
Keep inference status explicitly unavailable. Add the same bounded statement
to `CHANGELOG.md` with the non-claims from the spec.

- [ ] **Step 3: Run fresh Release verification**

```bash
cmake -S . -B build-final -G Ninja -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build-final --parallel
ctest --test-dir build-final --output-on-failure
./build-final/codec_tests
./build-final/codec capabilities
cmake --install build-final --prefix /tmp/codec-d7-install
cmake -S tests/package_consumer -B package-consumer-final -G Ninja \
  -DCMAKE_PREFIX_PATH=/tmp/codec-d7-install
cmake --build package-consumer-final --parallel
./package-consumer-final/codec_package_consumer
```

Expected: CTest 4/4, all direct unit tests, installed consumer, and capability
sanity pass; capabilities remain neural/GPU false.

- [ ] **Step 4: Run fresh sanitizer verification**

```bash
cmake -S . -B build-san-final -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san-final --parallel
ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --test-dir build-san-final --output-on-failure
```

Expected: CTest 4/4 with no sanitizer finding.

- [ ] **Step 5: Audit and commit**

Run `git diff --check`, `git status --short`, inspect the entire branch diff
against D.6, scan for build artifacts/secrets/placeholders, and verify README,
changelog, tests, and capabilities make no production-runtime claim.

```bash
git add README.md CHANGELOG.md tests/package_consumer/main.cpp
git commit -m "docs: record Stage D.7 offline separation"
```

### Task 5: Publish only exact green evidence

**Files:**
- No repository content changes unless verification exposes a defect.

**Interfaces:**
- Consumes: exact final D.7 branch head/tree and repository merge gates.
- Produces: exact-head GitHub CI, fast-forward main publication, post-publish
  CI, and roadmap evidence.

- [ ] **Step 1: Record exact local evidence**

Record branch head SHA, tree SHA, Release/CTest/direct/sanitizer/package results,
capability output, and the at-most-five-bullet diff audit.

- [ ] **Step 2: Publish the feature head and require exact-head CI**

Create/update `automation/stage-d7-offline-separation` without force, open a PR
against the exact D.6 base, and require GCC, Clang, installed-package, and
sanitizer jobs to succeed on the exact head SHA. If the head moves, restart
this gate.

- [ ] **Step 3: Integrate without overwriting concurrent work**

Re-read remote `main`, open PRs, issue #10, and exact-head CI. Merge only if
the expected D.6 base is unchanged and every worksheet gate is true. Require
the published tree to equal the exact tested feature tree; otherwise stop and
reverify.

- [ ] **Step 4: Require post-publish `main` CI**

Wait for the push-triggered run on the exact published `main` SHA and require
all jobs to complete successfully.

- [ ] **Step 5: Record Stage D.7 completion**

Comment on the unique roadmap issue with base, RED SHA/run, exact tested
head/tree/run, published main SHA/tree, post-publish run, touched truth classes
`[S1, D]` (S1 read-only), CODA layout delta `none`, capability output, and all
preserved non-claims. State that Stage D remains active.

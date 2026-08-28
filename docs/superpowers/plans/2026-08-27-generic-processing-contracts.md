# Stage C.3 Generic Processing Contracts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add payload-agnostic pull adapter and batch processor contracts with deterministic validation and resource bounds for non-audio profiles.

**Architecture:** A new public `processing.hpp` owns the extension contracts and values. A focused `processing.cpp` invokes provider implementations, validates returned records against Stage B truth/provenance rules, and enforces caller-supplied bounds without writing archives or changing the compatibility engine.

**Tech Stack:** C++20, existing `Result`, `Stream*`, CODA record/provenance types, CMake, custom unit harness, CTest, ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-27-generic-processing-contracts-design.md`

## Global Constraints

- Adapter records are owned S0 values; they never carry an S1/D truth label.
- Processor outputs are S1 or D only and carry valid generic `ProvenanceProcess` metadata.
- Every processor input is a direct supporting input for every returned output.
- Provider errors propagate unchanged; provider failure and invalid output perform no archive write.
- Preserve the CODA format, C ABI, CLI, `Engine`, `FeedSpec`, audio APIs, and capability JSON.
- Add no registry, dynamic plugin ABI, scheduler, persistence transaction, built-in profile, model, exporter, performance, scale, or deployment claim.

## File Structure

- Create `include/codec/processing.hpp`: owned adapter/processor values, limits, abstract interfaces, and invocation declarations.
- Create `src/core/processing.cpp`: common validation and bounded provider invocation only.
- Create `tests/test_processing.cpp`: non-audio contract, failure, truth, metadata, and resource-bound proofs.
- Modify `CMakeLists.txt`: compile the implementation and unit test file.
- Modify `README.md` and `CHANGELOG.md`: record only the implemented C++ contract boundary.
- Create ignored `build-stage-c3-consumer-src/`: installed-package compatibility proof; do not commit it.

---

### Task 1: Add the pull-based S0 adapter contract

**Files:**
- Create: `include/codec/processing.hpp`
- Create: `src/core/processing.cpp`
- Create: `tests/test_processing.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `StreamId`, `RecordTypeCode`, `Result`, and existing reserved record codes.
- Produces: `AdapterRecord`, `AdapterReadLimits`, `StreamAdapter`, and `pull_adapter_record()`.

- [ ] **Step 1: Write the failing adapter contract test**

Add `tests/test_processing.cpp` to `codec_tests` in `CMakeLists.txt`. Define a
`TelemetryAdapter` with two owned records and assert pull order, exact stream,
raw type, interval, payload, provider name, and successful empty-optional EOF:

```cpp
TEST(generic_adapter_pulls_owned_s0_records_in_order) {
  TelemetryAdapter adapter;
  auto first = codec::pull_adapter_record(adapter);
  auto second = codec::pull_adapter_record(adapter);
  auto eof = codec::pull_adapter_record(adapter);

  EXPECT_TRUE(first && *first);
  EXPECT_TRUE(second && *second);
  EXPECT_TRUE(eof && !*eof);
  EXPECT_EQ((*first)->type, codec::RecordTypeCode{0x7700});
  EXPECT_EQ((*first)->payload, bytes("temperature=21.5"));
  EXPECT_EQ((*second)->payload, bytes("temperature=21.6"));
  EXPECT_EQ(adapter.name(), std::string{"telemetry-test"});
}
```

- [ ] **Step 2: Build the unit target to verify RED**

Run:

```bash
cmake --build build-stage-c3-baseline --target codec_tests --parallel
```

Expected: compilation fails only because `<codec/processing.hpp>`,
`StreamAdapter`, `AdapterRecord`, and `pull_adapter_record()` do not exist.

- [ ] **Step 3: Add the public adapter types and minimal helper**

Create `include/codec/processing.hpp` with:

```cpp
#pragma once

#include <codec/archive.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace codec {

struct AdapterRecord {
  StreamId stream{};
  RecordTypeCode type{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::vector<std::byte> payload;
};

struct AdapterReadLimits {
  std::uint64_t maximum_payload_bytes{16ULL * 1024ULL * 1024ULL};
};

class StreamAdapter {
 public:
  virtual ~StreamAdapter() = default;
  virtual std::string name() const = 0;
  virtual Result<std::optional<AdapterRecord>> next() = 0;
};

Result<std::optional<AdapterRecord>> pull_adapter_record(
    StreamAdapter& adapter, AdapterReadLimits limits = {});

}  // namespace codec
```

Create `src/core/processing.cpp`. Validate a non-zero byte limit before calling
`next()`. Propagate provider errors and EOF. Reject an inverted interval or
`stream_provenance`/`final_index` type with `invalid_argument`; reject an
oversized payload with `resource_exhausted`; otherwise move the optional record
to the caller.

Add `src/core/processing.cpp` to `codec_core` in `CMakeLists.txt`.

- [ ] **Step 4: Build and run the unit binary to verify GREEN**

Run:

```bash
cmake --build build-stage-c3-baseline --target codec_tests --parallel
./build-stage-c3-baseline/codec_tests
```

Expected: every unit case passes, including the new adapter contract case.

- [ ] **Step 5: Commit the adapter boundary**

```bash
git add CMakeLists.txt include/codec/processing.hpp src/core/processing.cpp tests/test_processing.cpp
git commit -m "feat: add generic stream adapter contract"
```

### Task 2: Prove adapter validation and failure propagation

**Files:**
- Modify: `tests/test_processing.cpp`

**Interfaces:**
- Consumes: `pull_adapter_record(StreamAdapter&, AdapterReadLimits)`.
- Produces: deterministic failure/resource evidence for the S0 adapter boundary.

- [ ] **Step 1: Add focused invalid-provider tests**

Use a counting fixed adapter and a failing adapter. Assert:

```cpp
auto zero_limit = codec::pull_adapter_record(
    counting, codec::AdapterReadLimits{.maximum_payload_bytes = 0});
EXPECT_FALSE(zero_limit);
EXPECT_EQ(zero_limit.error().code, codec::ErrorCode::invalid_argument);
EXPECT_EQ(counting.calls, std::size_t{0});

auto failed = codec::pull_adapter_record(failing);
EXPECT_FALSE(failed);
EXPECT_EQ(failed.error().code, codec::ErrorCode::network);
EXPECT_TRUE(failed.error().retryable);
```

Add separate assertions for an inverted interval, each reserved type, and a
two-byte payload under a one-byte limit. Expected codes are `invalid_argument`
for interval/type violations and `resource_exhausted` for the byte bound.

- [ ] **Step 2: Temporarily bypass the interval check and verify RED**

Temporarily allow `end_ns < start_ns`, rebuild, and run the unit binary. Expected:
the inverted-interval assertion fails. Restore the check immediately.

- [ ] **Step 3: Run the unit binary to verify GREEN**

```bash
cmake --build build-stage-c3-baseline --target codec_tests --parallel
./build-stage-c3-baseline/codec_tests
```

Expected: every unit case passes.

- [ ] **Step 4: Commit adapter failure proof**

```bash
git add tests/test_processing.cpp
git commit -m "test: prove adapter contract boundaries"
```

### Task 3: Add the bounded S1/D processor contract

**Files:**
- Modify: `include/codec/processing.hpp`
- Modify: `src/core/processing.cpp`
- Modify: `tests/test_processing.cpp`

**Interfaces:**
- Consumes: exact `ExtractedRecord` batches and Stage B `ProvenanceProcess` validation.
- Produces: `ProcessorOutput`, `ProcessorRunLimits`, `StreamProcessor`, and `invoke_processor()`.

- [ ] **Step 1: Write the failing telemetry normalizer test**

Define a processor that consumes one exact telemetry record and returns one S1
output with a non-audio raw type and valid process identity:

```cpp
TEST(generic_processor_returns_bounded_s1_with_process_identity) {
  const auto input = extracted_record(bytes("temp_c=21.50"));
  TelemetryNormalizer processor;
  const std::array inputs{input};
  auto outputs = codec::invoke_processor(processor, inputs);

  EXPECT_TRUE(outputs);
  EXPECT_EQ(outputs->size(), std::size_t{1});
  EXPECT_EQ(outputs->front().truth, codec::TruthClass::state_exact);
  EXPECT_EQ(outputs->front().type, codec::RecordTypeCode{0x7701});
  EXPECT_EQ(outputs->front().payload, bytes("21.500"));
  EXPECT_EQ(outputs->front().process.operation,
            std::string{"normalize-temperature"});
}
```

- [ ] **Step 2: Build to verify RED**

```bash
cmake --build build-stage-c3-baseline --target codec_tests --parallel
```

Expected: compilation fails only because the processor types and invocation
function are absent.

- [ ] **Step 3: Add the public processor API**

Append to `processing.hpp`:

```cpp
struct ProcessorOutput {
  StreamId stream{};
  RecordTypeCode type{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  TruthClass truth{TruthClass::derived};
  std::vector<std::byte> payload;
  ProvenanceProcess process;
};

struct ProcessorRunLimits {
  std::size_t maximum_outputs{1024};
  std::uint64_t maximum_output_bytes{64ULL * 1024ULL * 1024ULL};
};

class StreamProcessor {
 public:
  virtual ~StreamProcessor() = default;
  virtual std::string name() const = 0;
  virtual Result<std::vector<ProcessorOutput>> process(
      std::span<const ExtractedRecord> inputs) = 0;
};

Result<std::vector<ProcessorOutput>> invoke_processor(
    StreamProcessor& processor,
    std::span<const ExtractedRecord> inputs,
    ProcessorRunLimits limits = {});
```

Implement `invoke_processor()` with these exact gates before provider invocation:

- input count is in `[1, 4096]`;
- both limits are non-zero; and
- each `input.payload.size()` equals `input.record.payload_size`.

After invocation, enforce output count, overflow-safe cumulative bytes, valid
interval, S1/D truth, and reserved-type rejection. Validate process metadata by
calling the existing internal `detail::encode_stream_provenance()` with one
synthetic input link; propagate its `invalid_argument`/`resource_exhausted`
error without archive writes.

- [ ] **Step 4: Build and run to verify GREEN**

```bash
cmake --build build-stage-c3-baseline --target codec_tests --parallel
./build-stage-c3-baseline/codec_tests
```

Expected: every unit case passes, including the telemetry normalizer.

- [ ] **Step 5: Commit the processor boundary**

```bash
git add include/codec/processing.hpp src/core/processing.cpp tests/test_processing.cpp
git commit -m "feat: add generic stream processor contract"
```

### Task 4: Prove processor failures, truth rules, and resource bounds

**Files:**
- Modify: `tests/test_processing.cpp`

**Interfaces:**
- Consumes: `invoke_processor(StreamProcessor&, span, ProcessorRunLimits)`.
- Produces: exhaustive contract/failure proof without persistence side effects.

- [ ] **Step 1: Add pre-invocation and provider-failure assertions**

Use a counting processor to prove empty inputs, 4097 inputs, mismatched payload
size, zero output count, and zero byte limits return `invalid_argument` before
the processor is called. Use a failing processor returning
`ErrorCode::inference` and assert the code/message/retryable flag propagate.

- [ ] **Step 2: Add output validation assertions**

Use a fixed-output processor and assert:

- two outputs under `maximum_outputs = 1` return `resource_exhausted`;
- a two-byte payload under `maximum_output_bytes = 1` returns
  `resource_exhausted`;
- `source_exact` and `static_cast<TruthClass>(0xff)` return `invalid_argument`;
- an inverted interval returns `invalid_argument`;
- provenance/final-index output types return `invalid_argument`;
- an empty process operation and mismatched details type/bytes return
  `invalid_argument`; and
- an empty output vector succeeds.

- [ ] **Step 3: Temporarily bypass the truth check and verify RED**

Temporarily accept `source_exact`, rebuild, and run the unit binary. Expected:
the S0-output assertion fails. Restore the S1/D-only check immediately.

- [ ] **Step 4: Run the unit binary to verify GREEN**

```bash
cmake --build build-stage-c3-baseline --target codec_tests --parallel
./build-stage-c3-baseline/codec_tests
```

Expected: every unit case passes.

- [ ] **Step 5: Commit processor failure proof**

```bash
git add tests/test_processing.cpp
git commit -m "test: prove processor contract boundaries"
```

### Task 5: Publish truthful status and verify the installed boundary

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Create ignored: `build-stage-c3-consumer-src/CMakeLists.txt`
- Create ignored: `build-stage-c3-consumer-src/main.cpp`

**Interfaces:**
- Consumes: the complete adapter/processor contract and existing archive/provenance APIs.
- Produces: truthful implementation status and installed-package compatibility evidence.

- [ ] **Step 1: Update only proven status**

Add one README implemented-generic bullet for pull-based S0 adapters and bounded
batch S1/D processor contracts with validation. Add one Unreleased changelog
entry. Retain registry, runtime orchestration, automatic persistence, engine
migration, built-in non-audio profiles, exporters, and dynamic plugin ABI as
unimplemented.

- [ ] **Step 2: Run the complete Release gate**

```bash
/tmp/codec-stage-tools/cmake/data/bin/cmake -S . -B build-stage-c3 -G Ninja \
  -DCMAKE_MAKE_PROGRAM=/tmp/codec-stage-tools/bin/ninja \
  -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
/tmp/codec-stage-tools/cmake/data/bin/cmake --build build-stage-c3 --parallel
/tmp/codec-stage-tools/cmake/data/bin/ctest --test-dir build-stage-c3 --output-on-failure
./build-stage-c3/codec capabilities
```

Expected: 4/4 CTest targets pass and capability JSON is unchanged.

- [ ] **Step 3: Run the sanitizer gate**

```bash
/tmp/codec-stage-tools/cmake/data/bin/cmake -S . -B build-stage-c3-san -G Ninja \
  -DCMAKE_MAKE_PROGRAM=/tmp/codec-stage-tools/bin/ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCODEC_WARNINGS_AS_ERRORS=ON \
  -DCODEC_ENABLE_SANITIZERS=ON
/tmp/codec-stage-tools/cmake/data/bin/cmake --build build-stage-c3-san --parallel
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/codec-stage-tools/cmake/data/bin/ctest --test-dir build-stage-c3-san --output-on-failure
```

Expected: 4/4 CTest targets pass without ASan/UBSan findings.

- [ ] **Step 4: Build an installed-package telemetry consumer**

Install the Release tree to an ignored prefix. The consumer must use
`find_package(codec 0.1 CONFIG REQUIRED)`, implement both interfaces, pull one
telemetry S0 record, append it with `append_raw()`, invoke the processor, append
the returned S1 subject and exact provenance sidecar, finalize, reopen, and use
`query_provenance()` to assert the exact S1 subject/input hashes. Build and run
the consumer successfully.

- [ ] **Step 5: Audit and commit status**

```bash
git diff --check
git status --short
git diff --stat 086838a7..HEAD
git add README.md CHANGELOG.md
git commit -m "docs: mark processing contracts implemented"
```

### Task 6: Verify and publish the exact tree

**Files:**
- No new files; verification and integration only.

**Interfaces:**
- Consumes: complete Stage C.3 feature branch.
- Produces: exact-tree main commit and roadmap evidence.

- [ ] **Step 1: Re-run exact-HEAD verification**

On the final commit, run a fresh warnings-as-errors Release build/CTest,
ASan/UBSan build/CTest, installed-package consumer, `git diff --check`, claim
audit, and clean tracked-worktree check.

- [ ] **Step 2: Recheck continuity immediately before publication**

Confirm remote `main` is still
`086838a7d54015228e62f1960f5f29608c33b81f`, no PR is open, and issue #10 is
still the single exact-title roadmap log. If `main` moved, rebase the branch and
repeat Step 1.

- [ ] **Step 3: Publish without force**

Create GitHub blobs/tree from only the tracked diff, verify the remote tree SHA
equals local `HEAD^{tree}`, create one commit with parent `086838a7...`, recheck
the remote ref, and update `main` with `force: false`.

- [ ] **Step 4: Confirm exact-commit CI and record completion**

Wait for `build (gcc)`, `build (clang)`, and `sanitizers` on the exact published
SHA. All three must complete successfully. Add a completion comment to issue
#10 with the commit/tree, tests, installed consumer, capability/non-claim
boundary, and CI links.

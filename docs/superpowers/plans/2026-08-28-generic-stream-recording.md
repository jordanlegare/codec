# Stage C.4 Generic Stream Recording Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add typed, stable-identity generic S0 URI capture to `Engine` while preserving the complete compatibility `FeedSpec` recording contract.

**Architecture:** Add `StreamSpec`, `StreamRecordingReport`, and a separately named `Engine::record_streams()` entry point. Refactor only the private prepared-source iteration into a common engine helper; descriptor validation and emission remain distinct so generic calls write `stream_descriptor` and legacy calls continue writing `feed_descriptor` with unchanged ordering.

**Tech Stack:** C++20, existing `Result`, `StreamDescriptor`, `CodaWriter`, hardened `PreparedCapture`, CMake, custom unit harness, CTest, ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-generic-stream-recording-design.md`

## Global Constraints

- Generic recording preserves exact S0 bytes and performs no decoding, canonicalization, inference, or truth reclassification.
- `descriptor.id` is caller-owned and independent of the transport URI.
- Validate the full request and prepare every source before archive creation.
- Generic recording emits `stream_descriptor`; compatibility recording continues to emit `feed_descriptor` with the existing derived ID, redacted URI, validation, ordering, report, CLI, and C ABI behavior.
- Labels never define generic identity and may be empty or duplicated.
- Preserve current file/stdin/HTTP security, redirect, private-network, chunk, and resource policies.
- Add no automatic adapter/processor persistence, registry, scheduler, archive transaction, built-in profile, exporter, format, CLI/C ABI, performance, scale, or deployment claim.

## File Structure

- Modify `include/codec/engine.hpp`: additive generic capture request/report types and `record_streams()` declaration.
- Modify `src/core/engine.cpp`: generic validation plus shared private preparation/recording loop and unchanged compatibility wrapper semantics.
- Modify `tests/test_engine.cpp`: typed telemetry success, exactness, validation, security, and legacy descriptor-layout proofs.
- Modify `README.md` and `CHANGELOG.md`: record only the installed C++ generic recording boundary.
- Create ignored `build-stage-c4-consumer-src/`: installed-package proof; do not commit it.

---

### Task 1: Add the typed generic recording success path

**Files:**
- Modify: `include/codec/engine.hpp`
- Modify: `src/core/engine.cpp`
- Modify: `tests/test_engine.cpp`

**Interfaces:**
- Consumes: `StreamDescriptor`, `PreparedCapture`, `CodaWriter::append_stream_descriptor()`, and S0 `RecordType::source_bytes`.
- Produces: `StreamSpec`, `StreamRecordingReport`, and `Engine::record_streams(const std::vector<StreamSpec>&, const std::filesystem::path&)`.

- [ ] **Step 1: Write the failing telemetry recording test**

Add `<algorithm>`, `<array>`, `<cstdint>`, `<iterator>`, `<utility>`, and
`<vector>` to the test includes, then add a deterministic non-zero ID helper:

```cpp
codec::StreamId stream_id(std::uint8_t seed) {
  codec::StreamId id{};
  for (std::size_t index = 0; index < id.bytes.size(); ++index) {
    id.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return id;
}
```

Add the success case:

```cpp
TEST(engine_records_a_typed_generic_stream_with_stable_identity) {
  const auto directory = std::filesystem::temp_directory_path();
  const auto input = directory / "codec-telemetry-input.bin";
  const auto archive_path = directory / "codec-telemetry.coda";
  const auto payload = std::string{"temp_c=21.500\n"};
  {
    std::ofstream output(input, std::ios::binary);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  }
  std::filesystem::remove(archive_path);
  const codec::StreamDescriptor descriptor{
      .id = stream_id(70),
      .type = codec::StreamType::telemetry,
      .label = "reactor-temperature",
      .source_id = "plant-a/sensor-42",
      .payload_type = "text/vnd.example.telemetry",
  };

  auto engine = codec::Engine::create({});
  EXPECT_TRUE(engine);
  auto report = engine->record_streams(
      {codec::StreamSpec{.uri = input.string(), .descriptor = descriptor}},
      archive_path);

  EXPECT_TRUE(report);
  EXPECT_EQ(report->streams_recorded, std::size_t{1});
  EXPECT_EQ(report->source_bytes,
            static_cast<std::uint64_t>(payload.size()));
  auto archive = std::move(*codec::CodaArchive::open(archive_path));
  auto streams = archive.streams();
  EXPECT_TRUE(streams);
  EXPECT_EQ(streams->size(), std::size_t{1});
  EXPECT_EQ(streams->front().id, descriptor.id);
  EXPECT_EQ(streams->front().type, descriptor.type);
  EXPECT_EQ(streams->front().label, descriptor.label);
  EXPECT_EQ(streams->front().source_id, descriptor.source_id);
  EXPECT_EQ(streams->front().payload_type, descriptor.payload_type);
  auto feeds = archive.feeds();
  EXPECT_TRUE(feeds);
  EXPECT_TRUE(feeds->empty());
  auto extracted = archive.extract_stream(descriptor.id);
  EXPECT_TRUE(extracted);
  EXPECT_EQ(extracted->size(), payload.size());
  EXPECT_TRUE(std::equal(extracted->begin(), extracted->end(),
                         reinterpret_cast<const std::byte*>(payload.data())));
  std::filesystem::remove(input);
  std::filesystem::remove(archive_path);
}
```

- [ ] **Step 2: Build the unit target to verify RED**

Run a fresh temporary Release configuration and build `codec_tests`:

```bash
/tmp/codec-stable-tools/cmake/data/bin/cmake -S . -B /tmp/codec-stage-c4-red \
  -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCODEC_BUILD_TESTS=ON -DCODEC_WARNINGS_AS_ERRORS=ON
CMAKE_BUILD_PARALLEL_LEVEL=1 \
  /tmp/codec-stable-tools/cmake/data/bin/cmake --build \
  /tmp/codec-stage-c4-red --target codec_tests
```

Expected: compilation fails only because `StreamSpec`,
`StreamRecordingReport`, and `Engine::record_streams()` do not exist.

- [ ] **Step 3: Add the public generic recording API**

Add to `include/codec/engine.hpp` before compatibility `FeedSpec`:

```cpp
struct StreamSpec {
  std::string uri;
  StreamDescriptor descriptor;
  bool preserve_source{true};
  std::uint64_t maximum_bytes{16ULL * 1024ULL * 1024ULL * 1024ULL};
};

struct StreamRecordingReport {
  std::filesystem::path archive;
  std::size_t streams_recorded{};
  std::uint64_t source_bytes{};
  std::uint64_t source_records{};
};
```

Add to `Engine`:

```cpp
Result<StreamRecordingReport> record_streams(
    const std::vector<StreamSpec>& streams,
    const std::filesystem::path& archive_path) const;
```

- [ ] **Step 4: Implement private shared preparation and recording helpers**

In the anonymous namespace of `src/core/engine.cpp`, add:

```cpp
struct PreparedSource {
  StreamId stream{};
  detail::PreparedCapture capture;
};

template <typename Spec, typename StreamForSpec>
Result<std::vector<PreparedSource>> prepare_sources(
    const std::vector<Spec>& specs, const EngineConfig& config,
    StreamForSpec&& stream_for_spec) {
  std::vector<PreparedSource> prepared;
  prepared.reserve(specs.size());
  for (const auto& spec : specs) {
    detail::CaptureOptions options;
    options.chunk_bytes = config.capture_chunk_bytes;
    options.maximum_bytes =
        std::min(config.maximum_feed_bytes, spec.maximum_bytes);
    options.maximum_redirects = config.maximum_redirects;
    options.deny_private_network = config.deny_private_network;
    auto source = detail::PreparedCapture::prepare(spec.uri, options);
    if (!source) return source.error();
    prepared.push_back(
        PreparedSource{stream_for_spec(spec), std::move(*source)});
  }
  return prepared;
}

template <typename AppendDescriptor>
Result<StreamRecordingReport> record_prepared_sources(
    std::vector<PreparedSource> prepared,
    const std::filesystem::path& archive_path,
    AppendDescriptor&& append_descriptor) {
  auto writer_result = CodaWriter::create(archive_path);
  if (!writer_result) return writer_result.error();
  auto writer = std::move(*writer_result);
  StreamRecordingReport report;
  report.archive = archive_path;
  for (std::size_t index = 0; index < prepared.size(); ++index) {
    auto descriptor = append_descriptor(writer, index, now_ns());
    if (!descriptor) return descriptor.error();
    auto captured = prepared[index].capture.run(
        [&](std::span<const std::byte> bytes) -> Result<void> {
          const auto observed = now_ns();
          auto appended = writer.append(RecordType::source_bytes,
                                        prepared[index].stream, observed,
                                        observed, bytes);
          if (!appended) return appended.error();
          report.source_bytes += bytes.size();
          report.source_records += 1;
          return {};
        });
    if (!captured) return captured.error();
    report.streams_recorded += 1;
  }
  auto finalized = writer.finalize();
  if (!finalized) return finalized.error();
  return report;
}
```

Move the compatibility method's existing prepared-source and append loop onto
these helpers without changing its validation or descriptor callback:

```cpp
auto prepared = prepare_sources(
    feeds, config_, [](const FeedSpec& feed) {
      return derive_stream_id(feed.label + "\n" + feed.uri);
    });
if (!prepared) return prepared.error();
auto captured = record_prepared_sources(
    std::move(*prepared), archive_path,
    [&feeds](CodaWriter& writer, std::size_t index,
             std::int64_t timestamp_ns) -> Result<RecordInfo> {
      const auto& spec = feeds[index];
      const auto stream = derive_stream_id(spec.label + "\n" + spec.uri);
      const FeedInfo info{stream, spec.label, redacted_uri(spec.uri), true};
      const auto descriptor = detail::encode_feed_descriptor(info);
      if (descriptor.empty()) {
        return fail<RecordInfo>(ErrorCode::invalid_argument,
                                "feed descriptor is too large");
      }
      return writer.append(RecordType::feed_descriptor, stream, timestamp_ns,
                           timestamp_ns, descriptor);
    });
if (!captured) return captured.error();
return RecordingReport{
    .archive = std::move(captured->archive),
    .feeds_recorded = captured->streams_recorded,
    .source_bytes = captured->source_bytes,
    .source_records = captured->source_records,
};
```

- [ ] **Step 5: Implement the minimal generic success path**

Add `Engine::record_streams()` with metadata validation before any preparation
or writer creation:

```cpp
Result<StreamRecordingReport> Engine::record_streams(
    const std::vector<StreamSpec>& streams,
    const std::filesystem::path& archive_path) const {
  if (streams.empty()) {
    return fail<StreamRecordingReport>(ErrorCode::invalid_argument,
                                       "at least one stream is required");
  }
  std::set<StreamId> ids;
  for (const auto& stream : streams) {
    if (stream.uri.empty() || !stream.preserve_source ||
        stream.maximum_bytes == 0) {
      return fail<StreamRecordingReport>(
          ErrorCode::invalid_argument,
          "each stream requires a URI and S0 preservation");
    }
    if (!ids.insert(stream.descriptor.id).second) {
      return fail<StreamRecordingReport>(ErrorCode::invalid_argument,
                                         "stream IDs must be unique");
    }
    auto encoded = detail::encode_stream_descriptor(stream.descriptor);
    if (!encoded) return encoded.error();
  }
  auto prepared = prepare_sources(
      streams, config_,
      [](const StreamSpec& stream) { return stream.descriptor.id; });
  if (!prepared) return prepared.error();
return record_prepared_sources(
    std::move(*prepared), archive_path,
    [&streams](CodaWriter& writer, std::size_t index,
               std::int64_t timestamp_ns) {
      return writer.append_stream_descriptor(streams[index].descriptor,
                                             timestamp_ns);
    });
}
```

- [ ] **Step 6: Build and run the unit binary to verify GREEN**

```bash
CMAKE_BUILD_PARALLEL_LEVEL=1 \
  /tmp/codec-stable-tools/cmake/data/bin/cmake --build \
  /tmp/codec-stage-c4-red --target codec_tests
/tmp/codec-stage-c4-red/codec_tests
```

Expected: 77 tests, 0 failures.

- [ ] **Step 7: Commit the generic recording boundary**

```bash
git add include/codec/engine.hpp src/core/engine.cpp tests/test_engine.cpp
git commit -m "feat: add generic stream recording"
```

### Task 2: Prove validation, security, and failure ordering

**Files:**
- Modify: `tests/test_engine.cpp`
- Modify: `src/core/engine.cpp`

**Interfaces:**
- Consumes: `Engine::record_streams()` from Task 1.
- Produces: deterministic pre-archive validation and inherited capture-security evidence.

- [ ] **Step 1: Add a counting set of invalid request assertions**

Create a valid `StreamSpec` helper:

```cpp
codec::StreamSpec stream_spec(std::string uri, std::uint8_t seed = 80) {
  return codec::StreamSpec{
      .uri = std::move(uri),
      .descriptor = codec::StreamDescriptor{
          .id = stream_id(seed),
          .type = codec::StreamType::telemetry,
          .label = "temperature",
          .source_id = "sensor-42",
          .payload_type = "text/vnd.example.telemetry",
      },
      .preserve_source = true,
      .maximum_bytes = 1024,
  };
}

void expect_invalid_stream_request(
    std::vector<codec::StreamSpec> streams,
    const std::filesystem::path& archive_path) {
  std::filesystem::remove(archive_path);
  auto engine = codec::Engine::create({});
  EXPECT_TRUE(engine);
  auto result = engine->record_streams(streams, archive_path);
  EXPECT_FALSE(result);
  if (!result) {
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  }
  EXPECT_FALSE(std::filesystem::exists(archive_path));
}
```

Add one test that constructs these cases and calls the helper with a unique
absent archive path for each:

```cpp
expect_invalid_stream_request({}, directory / "codec-stream-empty.coda");

auto invalid = stream_spec(input.string());
invalid.uri.clear();
expect_invalid_stream_request({invalid}, directory / "codec-stream-uri.coda");

invalid = stream_spec(input.string());
invalid.preserve_source = false;
expect_invalid_stream_request({invalid},
                              directory / "codec-stream-preserve.coda");

invalid = stream_spec(input.string());
invalid.maximum_bytes = 0;
expect_invalid_stream_request({invalid},
                              directory / "codec-stream-limit.coda");

invalid = stream_spec(input.string());
invalid.descriptor.source_id.clear();
expect_invalid_stream_request({invalid},
                              directory / "codec-stream-source.coda");

invalid = stream_spec(input.string());
invalid.descriptor.payload_type.clear();
expect_invalid_stream_request({invalid},
                              directory / "codec-stream-payload.coda");

auto first = stream_spec(first_input.string());
auto second = stream_spec(second_input.string());
second.descriptor.id = first.descriptor.id;
second.descriptor.label = "different-label";
const std::vector<codec::StreamSpec> duplicate_ids{first, second};
expect_invalid_stream_request(duplicate_ids,
                              directory / "codec-stream-duplicate.coda");
```

Give the two duplicate-ID specs different labels and URIs but the same
`descriptor.id` to prove labels and transports do not define identity.

- [ ] **Step 2: Add generic alias and private-network security tests**

Mirror the existing compatibility tests through `record_streams()`:

```cpp
std::string read_text(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

auto alias = engine->record_streams(
    {stream_spec(path.string())}, path);
EXPECT_FALSE(alias);
EXPECT_EQ(read_text(path), sentinel);

auto denied = engine->record_streams(
    {stream_spec("http://2130706433:9/private")}, archive_path);
EXPECT_FALSE(denied);
EXPECT_EQ(denied.error().code, codec::ErrorCode::unauthorized_source);
EXPECT_TRUE(std::filesystem::exists(archive_path));
auto denied_archive = codec::CodaArchive::open(archive_path);
EXPECT_TRUE(denied_archive);
const auto verification = denied_archive->verify();
EXPECT_TRUE(verification.ok);
EXPECT_FALSE(verification.finalized);
auto prefix = denied_archive->records(
    codec::ArchiveReadPolicy::verified_prefix);
EXPECT_TRUE(prefix);
EXPECT_EQ(prefix->size(), std::size_t{1});
EXPECT_EQ(prefix->front().type, codec::RecordType::stream_descriptor);
```

This intentionally matches the compatibility lifecycle: numeric/DNS denial is
enforced when the prepared HTTP source starts, after descriptor creation, and
must leave no denied `source_bytes` record. Do not add buffering or transaction
semantics to make the archive disappear.

- [ ] **Step 3: Temporarily bypass duplicate-ID validation and verify RED**

Temporarily remove the unique-ID check, rebuild, and run the unit binary.
Expected: the duplicate-ID pre-archive assertion fails. Restore the check
immediately.

- [ ] **Step 4: Run the unit binary to verify GREEN**

```bash
CMAKE_BUILD_PARALLEL_LEVEL=1 \
  /tmp/codec-stable-tools/cmake/data/bin/cmake --build \
  /tmp/codec-stage-c4-red --target codec_tests
/tmp/codec-stage-c4-red/codec_tests
```

Expected: every unit case passes.

- [ ] **Step 5: Commit validation and security proofs**

```bash
git add src/core/engine.cpp tests/test_engine.cpp
git commit -m "test: prove generic recording boundaries"
```

### Task 3: Freeze legacy descriptor and report compatibility

**Files:**
- Modify: `tests/test_engine.cpp`

**Interfaces:**
- Consumes: compatibility `Engine::record(FeedSpec)` and generic
  `Engine::record_streams(StreamSpec)`.
- Produces: explicit evidence that the shared loop does not merge their public
  descriptor contracts.

- [ ] **Step 1: Extend the existing feed regression**

After opening the compatibility archive, inspect `records()` and assert:

```cpp
EXPECT_EQ(report->feeds_recorded, std::size_t{1});
EXPECT_EQ(report->source_records, std::uint64_t{1});
auto records = archive->records();
EXPECT_TRUE(records);
EXPECT_EQ(records->front().type, codec::RecordType::feed_descriptor);
EXPECT_TRUE(std::none_of(records->begin(), records->end(),
                         [](const codec::RecordInfo& record) {
                           return record.type ==
                                  codec::RecordType::stream_descriptor;
                         }));
```

In the generic test, assert the first record is `stream_descriptor` and no
record is `feed_descriptor`.

- [ ] **Step 2: Build and run the complete unit binary**

```bash
CMAKE_BUILD_PARALLEL_LEVEL=1 \
  /tmp/codec-stable-tools/cmake/data/bin/cmake --build \
  /tmp/codec-stage-c4-red --target codec_tests
/tmp/codec-stage-c4-red/codec_tests
```

Expected: all generic and legacy cases pass.

- [ ] **Step 3: Commit the compatibility proof**

```bash
git add tests/test_engine.cpp
git commit -m "test: preserve feed recording compatibility"
```

### Task 4: Publish truthful status and verify the installed API

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Create ignored: `build-stage-c4-consumer-src/CMakeLists.txt`
- Create ignored: `build-stage-c4-consumer-src/main.cpp`

**Interfaces:**
- Consumes: complete generic recording boundary and installed `codec::codec`
  target.
- Produces: truthful status plus downstream compile/link/runtime evidence.

- [ ] **Step 1: Update only proven status**

Add one README implemented-generic bullet for typed, stable-ID C++ stream
recording through the hardened URI capture path. Add one Unreleased changelog
entry. Retain adapter/processor automatic persistence, profile exporters,
generic CLI/C ABI, built-in profiles, audio API migration, and Stage C
completion as planned or unimplemented.

- [ ] **Step 2: Create an ignored installed-package telemetry consumer**

Use this CMake contract:

```cmake
cmake_minimum_required(VERSION 3.20)
project(codec_stream_recording_consumer LANGUAGES CXX)
find_package(codec 0.1 CONFIG REQUIRED)
add_executable(codec_stream_recording_consumer main.cpp)
target_compile_features(codec_stream_recording_consumer PRIVATE cxx_std_20)
target_link_libraries(codec_stream_recording_consumer PRIVATE codec::codec)
```

Use this complete `main.cpp` contract:

```cpp
#include <codec/engine.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
  const auto input = std::filesystem::path{"installed-telemetry.bin"};
  const auto archive_path =
      std::filesystem::path{"installed-telemetry.coda"};
  const std::string payload = "temp_c=21.500\n";
  {
    std::ofstream output(input, std::ios::binary);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  }
  std::filesystem::remove(archive_path);
  codec::StreamId id{};
  for (std::size_t index = 0; index < id.bytes.size(); ++index) {
    id.bytes[index] = static_cast<std::uint8_t>(90 + index);
  }
  const codec::StreamDescriptor descriptor{
      .id = id,
      .type = codec::StreamType::telemetry,
      .label = "installed-temperature",
      .source_id = "installed/sensor-42",
      .payload_type = "text/vnd.example.telemetry",
  };
  auto engine = codec::Engine::create({});
  if (!engine) return 1;
  auto report = engine->record_streams(
      {codec::StreamSpec{.uri = input.string(), .descriptor = descriptor}},
      archive_path);
  if (!report || report->streams_recorded != 1) return 2;
  auto archive = codec::CodaArchive::open(archive_path);
  if (!archive) return 3;
  auto streams = archive->streams();
  if (!streams || streams->size() != 1 ||
      streams->front().id != descriptor.id ||
      streams->front().type != descriptor.type ||
      streams->front().source_id != descriptor.source_id ||
      streams->front().payload_type != descriptor.payload_type) {
    return 4;
  }
  auto extracted = archive->extract_stream(id);
  const auto exact =
      extracted && extracted->size() == payload.size() &&
      std::equal(extracted->begin(), extracted->end(),
                 reinterpret_cast<const std::byte*>(payload.data()));
  std::filesystem::remove(input);
  std::filesystem::remove(archive_path);
  return exact ? 0 : 5;
}
```

- [ ] **Step 3: Run a fresh Release gate and install**

Use a `mktemp -d` root outside the watched workspace, CMake 3.31.6, system Make,
warnings as errors, and one build job. Verify generated binaries remain valid
after a four-second delay, then run:

```bash
ctest --test-dir "$proof_root/release" --output-on-failure
"$proof_root/release/codec" capabilities
cmake --install "$proof_root/release" --prefix "$proof_root/install"
```

Expected: 4/4 CTest targets pass, `processing.hpp` and the updated `engine.hpp`
install, and capability JSON remains unchanged.

- [ ] **Step 4: Build and run the installed consumer**

Configure it with `-DCMAKE_PREFIX_PATH="$proof_root/install"`, build with one
job, wait four seconds, verify the executable remains an ELF executable, and
run it successfully.

- [ ] **Step 5: Run a fresh sanitizer gate**

Configure a separate Debug build under the same temporary proof root with
`CODEC_ENABLE_SANITIZERS=ON`; build with one job and run:

```bash
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir "$proof_root/san" --output-on-failure
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  "$proof_root/san/codec_tests"
```

Expected: 4/4 CTest targets and every direct unit case pass without sanitizer
findings.

- [ ] **Step 6: Audit and commit status**

```bash
git diff --check
git status --short
git diff --stat 20ab862a8409290e9c9287af86cd5fa8a3fcad4f..HEAD
git add README.md CHANGELOG.md
git commit -m "docs: mark generic stream recording implemented"
```

### Task 5: Verify and publish the exact tree

**Files:**
- No new files; verification and integration only.

**Interfaces:**
- Consumes: complete Stage C.4 feature branch.
- Produces: exact-tree `main` commit and roadmap evidence.

- [ ] **Step 1: Re-run exact-HEAD verification**

On the final commit, run fresh warnings-as-errors Release/CTest,
ASan/UBSan/CTest, direct unit cases, installed-package consumer, capability
JSON, `git diff --check`, claim audit, and clean tracked-worktree checks.

- [ ] **Step 2: Recheck continuity immediately before publication**

Confirm remote `main` is still
`20ab862a8409290e9c9287af86cd5fa8a3fcad4f`, no PR is open, and issue #10 is
still the single exact-title roadmap log. If `main` moved, rebase the branch and
repeat Step 1.

- [ ] **Step 3: Publish without force**

Create GitHub blobs/tree from only the tracked diff, verify the remote tree SHA
equals local `HEAD^{tree}`, create one commit with parent `20ab862a...`, recheck
the remote ref, and update `main` with `force: false`.

- [ ] **Step 4: Confirm exact-commit CI and record completion**

Wait for `build (gcc)`, `build (clang)`, and `sanitizers` on the exact published
SHA. All three must complete successfully. Add a completion comment to issue
#10 with the commit/tree, test counts, installed consumer, generic/legacy
descriptor proof, capability/non-claim boundary, CI links, and the next unmet
Stage C dependency.

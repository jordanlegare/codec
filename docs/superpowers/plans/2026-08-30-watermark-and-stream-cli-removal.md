# Watermark and Generic-Stream CLI Removal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove CODEC's complete watermark feature and generic-stream CLI selection while retaining generic stream C++ APIs and making source-exact the default feed-extraction fidelity.

**Architecture:** Remove profile-specific watermark code at its public, build, CLI, error, and documentation boundaries. Narrow only the list/extract CLI branches, preserve numeric gaps as compatibility tombstones, and rely on existing opaque-record support for old archive codes 20 and 21.

**Tech Stack:** C++20, C17 ABI header/tests, CMake 3.20+, Bash CLI integration tests, OpenSSL 3 Crypto, libcurl, libFLAC.

**Spec:** `docs/superpowers/specs/2026-08-30-watermark-and-stream-cli-removal-design.md`

## Global Constraints

- Base the implementation on `a73b15c474b6974bf74b2c622b81bdcafee7890c` through the committed design checkpoint `89bb674608169e18d2735621445c796c12c13dd3`.
- Do not remove generic `StreamId`, descriptors, recording, query, extraction, follow, processing, transport, recovery, or distributed C++ APIs.
- Do not change CODA envelope bytes, format versions, hashing, commit chains, S0/S1/D semantics, or unknown-type preservation.
- Do not modify the command bodies for `record`, `verify`, `inspect`, or `repair`.
- Keep `codec list feeds` and regular/live `codec extract --feed` output compatible.
- Source-exact is the default when `--fidelity` is absent; explicit `--fidelity source-exact` remains valid and every other value remains invalid.
- Do not reuse archive type codes 20/21, C++ error values 10-14, or distributed wire slots 10-14.
- Keep OpenSSL because `src/archive/archive.cpp` uses `RAND_bytes()` independently of watermark signing.
- Record the breaking removal under Unreleased; do not change `project(CODEC VERSION 0.3.0)`.
- Use `apply_patch` for every file edit or deletion. Preserve historical released changelog entries and historical design documents.
- CMake is a hard verification prerequisite. If it is unavailable, stop at the verification gate and report the blocker rather than claiming completion.

## File Structure Map

| Responsibility | Files |
|---|---|
| CLI behavior and capability JSON | `src/cli/main.cpp`, `include/codec/engine.hpp`, `tests/cli_integration.sh`, `tests/test_engine.cpp` |
| Watermark public/profile surface | `include/codec/watermark.hpp`, `include/codec/statement.hpp`, `include/codec/profiles/audio.hpp`, `tests/test_audio_profile.cpp` |
| Watermark implementation/build | `src/watermark/carrier.cpp`, `src/watermark/statement.cpp`, `tests/test_watermark.cpp`, `tests/test_statement.cpp`, `CMakeLists.txt` |
| Compatibility tombstones | `include/codec/archive.hpp`, `include/codec/result.hpp`, `include/codec/codec_c.h`, `src/archive/archive.cpp`, `src/core/sha256.cpp`, `src/capi/codec_c.cpp`, `src/distributed/wire.cpp` |
| Tombstone proof | `tests/test_archive.cpp`, `tests/test_distributed_wire.cpp`, `tests/test_distributed_wire_strict.cpp`, `tests/ai_contract.cmake` |
| Current documentation | `README.md`, `AGENTS.md`, `CONTRIBUTING.md`, `CHANGELOG.md`, `AI_WORKSHEET.md` |

---

### Task 1: Lock the new CLI contract with failing integration tests

**Files:**
- Modify: `tests/cli_integration.sh:20-64`
- Modify: `tests/cli_integration.sh:108-171`

**Interfaces:**
- Consumes: current `codec` command-line executable.
- Produces: executable proof that retired commands return status 2, retained feed extraction defaults to source-exact, and removed commands do not write outputs.

- [ ] **Step 1: Add a reusable status-2 assertion**

Insert after the cleanup trap:

```bash
expect_status_2() {
  local stdout_path=$1
  local stderr_path=$2
  shift 2
  set +e
  "$@" > "$stdout_path" 2> "$stderr_path"
  local status=$?
  set -e
  if [ "$status" -ne 2 ]; then
    echo "expected status 2, got $status: $*" >&2
    return 1
  fi
}
```

- [ ] **Step 2: Prove source-exact defaults for regular extraction**

Replace the first feed extraction with an invocation that omits `--fidelity`, capture its JSON, and retain one explicit compatibility invocation:

```bash
"$codec_bin" extract "$test_dir/session.coda" --feed news \
  --output "$test_dir/extracted.bin" > "$test_dir/extract.json"
grep -q '"fidelity":"source_exact"' "$test_dir/extract.json"
cmp "$test_dir/input.bin" "$test_dir/extracted.bin"

"$codec_bin" extract "$test_dir/session.coda" --feed news \
  --fidelity source-exact --output "$test_dir/explicit-extracted.bin"
cmp "$test_dir/input.bin" "$test_dir/explicit-extracted.bin"
```

- [ ] **Step 3: Replace generic-stream CLI success coverage with removal coverage**

Delete the current `list streams`, stream-ID extraction, missing-stream, and malformed-stream blocks. Add:

```bash
expect_status_2 "$test_dir/list-streams.stdout" \
  "$test_dir/list-streams.stderr" \
  "$codec_bin" list streams "$test_dir/session.coda"
grep -q 'list supports: list feeds' "$test_dir/list-streams.stderr"

printf 'sentinel output\n' > "$test_dir/stream-output.bin"
cp "$test_dir/stream-output.bin" "$test_dir/expected-sentinel.bin"
expect_status_2 "$test_dir/stream.stdout" "$test_dir/stream.stderr" \
  "$codec_bin" extract "$test_dir/session.coda" \
  --stream 00000000-0000-0000-0000-000000000001 \
  --output "$test_dir/stream-output.bin"
cmp "$test_dir/expected-sentinel.bin" "$test_dir/stream-output.bin"
```

- [ ] **Step 4: Prove follow extraction also defaults to source-exact**

Remove `--fidelity source-exact` from the live `extract --follow` invocation. After `wait "$follow_pid"`, add:

```bash
grep -q '"fidelity":"source_exact"' "$test_dir/live-follow.json"
grep -q '"follow":true' "$test_dir/live-follow.json"
```

- [ ] **Step 5: Replace the watermark lifecycle with safe-removal checks**

Delete WAV generation, key generation, issue, and detection. Add:

```bash
expect_status_2 "$test_dir/watermark.stdout" "$test_dir/watermark.stderr" \
  "$codec_bin" watermark keygen --private "$test_dir/issuer.key" \
  --public "$test_dir/issuer.pub"
[ ! -e "$test_dir/issuer.key" ]
[ ! -e "$test_dir/issuer.pub" ]

"$codec_bin" --help > "$test_dir/help.txt"
if grep -Eq 'codec watermark|codec list streams|extract .*--stream' \
    "$test_dir/help.txt"; then
  echo "help still advertises a retired CLI surface" >&2
  exit 1
fi
if grep -Eq 'w0_ed25519|w1_reference|w2_reference|w2_policy' \
    "$test_dir/capabilities.json"; then
  echo "capabilities still advertise watermarking" >&2
  exit 1
fi
```

- [ ] **Step 6: Run the CLI integration proof and observe RED**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build -R '^codec-cli-integration$' --output-on-failure
```

Expected: FAIL because `codec list streams` and `codec watermark keygen` still succeed or because help/capabilities still advertise retired surfaces. The default-fidelity checks should already pass, confirming the parser currently implements that requested default.

- [ ] **Step 7: Commit the failing contract**

```bash
git add tests/cli_integration.sh
git commit -m "test: specify reduced feed-only CLI"
```

---

### Task 2: Remove the retired CLI paths and capability claims

**Files:**
- Modify: `src/cli/main.cpp:1-653`
- Modify: `include/codec/engine.hpp:51-61`
- Modify: `tests/test_engine.cpp:282-290`

**Interfaces:**
- Consumes: Task 1 CLI contract.
- Produces: feed-only `list`/`extract`, unknown-command handling for `watermark`, and a watermark-free `codec::Capabilities`/JSON shape.

- [ ] **Step 1: Remove watermark-only includes and parser helpers**

From `src/cli/main.cpp`, remove `<codec/audio.hpp>`, `<codec/statement.hpp>`, `<codec/watermark.hpp>`, and standard headers used only by the deleted code. Delete these helpers after confirming their call sites are watermark/stream-only with `rg`:

```text
now_seconds
integer
code_value
hex_nibble
stream_id
read_bytes
watermark_keygen
watermark_issue
watermark_detect
watermark_command
```

Keep `write_bytes`, `flag`, `option`, `std::chrono_literals`, and `std::thread` because feed extraction/follow still use them.

- [ ] **Step 2: Narrow usage and capability output**

Use these usage lines:

```cpp
      << "  codec list feeds ARCHIVE\n"
      << "  codec extract ARCHIVE --feed LABEL [--fidelity source-exact] [--follow] --output FILE\n"
```

Remove all watermark usage lines. In `capabilities_command()`, emit only the retained fields:

```cpp
  std::cout << "{\"version\":\"" << CODEC_VERSION_STRING << "\","
            << "\"coda_archive\":" << (value.coda_archive ? "true" : "false")
            << ",\"file_capture\":" << (value.file_capture ? "true" : "false")
            << ",\"http_capture\":" << (value.http_capture ? "true" : "false")
            << ",\"pcm16_wav\":" << (value.pcm16_wav ? "true" : "false")
            << ",\"neural_separation\":"
            << (value.neural_separation ? "true" : "false")
            << ",\"gpu_inference\":"
            << (value.gpu_inference ? "true" : "false") << "}\n";
```

- [ ] **Step 3: Make `list_command` feed-only**

Validate `arguments.front() == "feeds"` and remove the stream branch:

```cpp
int list_command(const Strings& arguments) {
  if (arguments.size() < 2 || arguments.front() != "feeds") {
    std::cerr << "codec: list supports: list feeds ARCHIVE\n";
    return 2;
  }
  auto archive = codec::CodaArchive::open(std::string{arguments[1]});
  if (!archive) return print_error(archive.error());
  auto feeds = archive->feeds();
  if (!feeds) return print_error(feeds.error());
  for (const auto& feed : *feeds) {
    std::cout << "{\"label\":\"" << codec::detail::json_escape(feed.label)
              << "\",\"stream_id\":\"" << codec::to_string(feed.stream)
              << "\",\"uri\":\"" << codec::detail::json_escape(feed.uri)
              << "\",\"fidelity\":\"S0\"}\n";
  }
  return 0;
}
```

- [ ] **Step 4: Make follow extraction resolve only a feed**

Replace the function with this feed-only form:

```cpp
int follow_extract(const codec::CodaArchive& archive, std::string_view label,
                   const std::filesystem::path& output_path) {
  using namespace std::chrono_literals;

  std::optional<codec::StreamId> selected_stream;
  for (;;) {
    auto feeds = archive.feeds(codec::ArchiveReadPolicy::verified_prefix);
    if (!feeds) return print_error(feeds.error());
    std::optional<codec::StreamId> match;
    for (const auto& feed : *feeds) {
      if (feed.label != label) continue;
      if (match) {
        std::cerr << "codec: archive_corrupt: duplicate feed label: "
                  << label << '\n';
        return 1;
      }
      match = feed.stream;
    }
    if (match) {
      selected_stream = *match;
      break;
    }
    auto records = archive.records(codec::ArchiveReadPolicy::verified_prefix);
    if (!records) return print_error(records.error());
    const bool finalized = std::any_of(
        records->begin(), records->end(), [](const codec::RecordInfo& record) {
          return record.type == codec::RecordType::final_index;
        });
    if (finalized) {
      std::cerr << "codec: feed label not found: " << label << '\n';
      return 1;
    }
    std::this_thread::sleep_for(50ms);
  }

  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    std::cerr << "codec: archive_io: cannot open output: "
              << output_path.string() << '\n';
    return 1;
  }

  codec::SourceExactCursor cursor{};
  std::uint64_t total_bytes = 0;
  bool observed_stream = false;
  for (;;) {
    auto batch = codec::extract_stream_source_exact_prefix(
        archive, *selected_stream, cursor);
    if (!batch) return print_error(batch.error());
    cursor = batch->cursor;

    for (const auto& extracted : batch->records) {
      observed_stream = true;
      if (!extracted.payload.empty()) {
        output.write(reinterpret_cast<const char*>(extracted.payload.data()),
                     static_cast<std::streamsize>(extracted.payload.size()));
        if (!output) {
          std::cerr << "codec: archive_io: cannot write follow output\n";
          return 1;
        }
      }
      total_bytes += extracted.payload.size();
    }
    if (!batch->records.empty()) {
      output.flush();
      if (!output) {
        std::cerr << "codec: archive_io: cannot flush follow output\n";
        return 1;
      }
    }

    if (batch->finalized) {
      if (!observed_stream) {
        auto streams =
            archive.streams(codec::ArchiveReadPolicy::verified_prefix);
        if (!streams) return print_error(streams.error());
        const bool declared = std::any_of(
            streams->begin(), streams->end(), [&](const auto& descriptor) {
              return descriptor.id == *selected_stream;
            });
        if (!declared) {
          std::cerr << "codec: stream ID not found: "
                    << codec::to_string(*selected_stream) << '\n';
          return 1;
        }
      }
      std::cout << "{\"feed\":\"" << codec::detail::json_escape(label)
                << "\",\"fidelity\":\"source_exact\",\"bytes\":"
                << total_bytes << ",\"follow\":true}\n";
      return 0;
    }

    if (batch->records.empty()) std::this_thread::sleep_for(50ms);
  }
}
```

- [ ] **Step 5: Make regular extraction feed-only without changing the default**

Replace `extract_command` with the feed-only target below. `flag(tail, "--stream")` ensures a trailing or valueless retired option cannot be ignored:

```cpp
int extract_command(const Strings& arguments) {
  if (arguments.empty()) {
    std::cerr << "codec: extract requires an archive\n";
    return 2;
  }
  const Strings tail(arguments.begin() + 1, arguments.end());
  const auto label = option(tail, "--feed");
  const auto output = option(tail, "--output");
  const auto fidelity = option(tail, "--fidelity");
  const bool follow = flag(tail, "--follow");
  if (!label || !output || flag(tail, "--stream") ||
      (fidelity && *fidelity != "source-exact")) {
    std::cerr << "codec: extract requires --feed, --output, and source-exact fidelity\n";
    return 2;
  }
  auto archive = codec::CodaArchive::open(std::string{arguments.front()});
  if (!archive) return print_error(archive.error());

  if (follow) {
    return follow_extract(*archive, *label,
                          std::filesystem::path{std::string{*output}});
  }

  auto extracted = archive->extract_feed(*label);
  if (!extracted) return print_error(extracted.error());
  auto written = write_bytes(std::string{*output}, *extracted);
  if (!written) return print_error(written.error());
  std::cout << "{\"feed\":\"" << codec::detail::json_escape(*label)
            << "\",\"fidelity\":\"source_exact\",\"bytes\":"
            << extracted->size() << "}\n";
  return 0;
}
```

- [ ] **Step 6: Remove capability members and watermark dispatch**

Delete `w0_ed25519`, `w1_reference`, and `w2_reference` from `codec::Capabilities`. Remove their assertions from `capabilities_never_claim_an_unloaded_neural_backend`. Delete this line from `main()`:

```cpp
if (command == "watermark") return watermark_command(arguments);
```

- [ ] **Step 7: Run targeted CLI and unit tests**

```bash
cmake --build build --parallel
ctest --test-dir build -R '^(codec-unit|codec-cli-integration)$' --output-on-failure
```

Expected: PASS. Confirm `codec extract ... --feed ... --output ...` and the explicit fidelity spelling both pass.

- [ ] **Step 8: Commit the CLI removal**

```bash
git add src/cli/main.cpp include/codec/engine.hpp tests/test_engine.cpp tests/cli_integration.sh
git commit -m "refactor: reduce CLI to feed-based extraction"
```

---

### Task 3: Remove watermark public APIs, implementation, tests, and build entries

**Files:**
- Modify: `tests/ai_contract.cmake`
- Modify: `CMakeLists.txt`
- Modify: `include/codec/profiles/audio.hpp`
- Modify: `tests/test_audio_profile.cpp`
- Delete: `include/codec/watermark.hpp`
- Delete: `include/codec/statement.hpp`
- Delete: `src/watermark/carrier.cpp`
- Delete: `src/watermark/statement.cpp`
- Delete: `tests/test_watermark.cpp`
- Delete: `tests/test_statement.cpp`

**Interfaces:**
- Consumes: watermark-free CLI from Task 2.
- Produces: installed C++ package and Audio Profile facade with no watermark or statement surface.

- [ ] **Step 1: Add repository-contract checks for retired files and build references**

Add to `tests/ai_contract.cmake`:

```cmake
foreach(retired_file IN ITEMS
    "include/codec/watermark.hpp"
    "include/codec/statement.hpp"
    "src/watermark/carrier.cpp"
    "src/watermark/statement.cpp"
    "tests/test_watermark.cpp"
    "tests/test_statement.cpp")
  if(EXISTS "${CODEC_SOURCE_DIR}/${retired_file}")
    message(FATAL_ERROR "Retired watermark file remains: ${retired_file}")
  endif()
endforeach()

foreach(retired_build_text IN ITEMS
    "src/watermark/carrier.cpp"
    "src/watermark/statement.cpp"
    "tests/test_watermark.cpp"
    "tests/test_statement.cpp")
  string(FIND "${cmake_contents}" "${retired_build_text}" retired_offset)
  if(NOT retired_offset EQUAL -1)
    message(FATAL_ERROR
      "CMakeLists.txt still references ${retired_build_text}")
  endif()
endforeach()
```

- [ ] **Step 2: Run the contract test and observe RED**

```bash
cmake -DCODEC_SOURCE_DIR="$PWD" -P tests/ai_contract.cmake
```

Expected: FAIL with `Retired watermark file remains: include/codec/watermark.hpp`.

- [ ] **Step 3: Remove build entries and facade aliases**

Remove the two watermark sources and two watermark test sources from `CMakeLists.txt`. From `include/codec/profiles/audio.hpp`, remove both retired includes plus these names:

```text
CarrierBand
FeedStatement
StatementState
StatementVerification
WatermarkEmbedReport
WatermarkObservation
WatermarkPolicy
carrier_band_name
detect_watermarks
embed_watermark
generate_ed25519_keypair
issue_statement
statement_state_name
verify_statement
```

Remove the matching static assertions and the carrier/statement checks from `tests/test_audio_profile.cpp`. Keep PCM16, ingest/export, separation, model-bundle, and ONNX facade coverage intact.

- [ ] **Step 4: Delete the retired files with `apply_patch`**

Use one `apply_patch` deletion patch for the six files listed in this task. Do not use a recursive shell deletion and do not touch `.gitignore` credential patterns.

- [ ] **Step 5: Reconfigure and run contract, unit, and package-consumer tests**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
cmake -DCODEC_SOURCE_DIR="$PWD" -P tests/ai_contract.cmake
ctest --test-dir build -R '^(codec-unit|codec-ai-contract)$' --output-on-failure
```

Expected: PASS. The installed-package consumer continues compiling because it uses retained Audio Profile APIs only.

- [ ] **Step 6: Commit the feature-source removal**

```bash
git add CMakeLists.txt include/codec/profiles/audio.hpp tests/test_audio_profile.cpp tests/ai_contract.cmake
git add -u include/codec src/watermark tests
git commit -m "refactor: remove watermark implementation and APIs"
```

---

### Task 4: Convert archive and error identifiers into compatibility tombstones

**Files:**
- Modify: `tests/test_archive.cpp`
- Modify: `tests/test_distributed_wire.cpp`
- Modify: `tests/test_distributed_wire_strict.cpp`
- Modify: `tests/ai_contract.cmake`
- Modify: `include/codec/archive.hpp`
- Modify: `include/codec/result.hpp`
- Modify: `include/codec/codec_c.h`
- Modify: `src/archive/archive.cpp`
- Modify: `src/core/sha256.cpp`
- Modify: `src/capi/codec_c.cpp`
- Modify: `src/distributed/wire.cpp`

**Interfaces:**
- Consumes: opaque `RecordTypeCode`, `append_raw`, `extract_stream_raw`, repair, and DRS1 strict-parser helpers.
- Produces: unassigned archive codes 20/21, unassigned error/wire values 10-14, and stable retained numeric mappings.

- [ ] **Step 1: Add archive proof for retired raw codes**

Add a focused test near the existing unknown-record tests:

```cpp
TEST(retired_watermark_record_codes_remain_opaque_and_repairable) {
  const auto source = test_path("retired-record-source.coda");
  const auto repaired = test_path("retired-record-output.coda");
  std::filesystem::remove(source);
  std::filesystem::remove(repaired);
  const auto stream = stream_id(7);
  const std::array<codec::RecordTypeCode, 2> retired{20, 21};
  const std::array payloads{bytes("retired-20"), bytes("retired-21")};

  auto writer = std::move(*codec::CodaWriter::create(source));
  for (std::size_t index = 0; index < retired.size(); ++index) {
    EXPECT_TRUE(writer.append_raw(retired[index], stream,
                                  static_cast<std::int64_t>(index),
                                  static_cast<std::int64_t>(index + 1),
                                  payloads[index]));
  }
  EXPECT_TRUE(writer.finalize());

  auto repair = codec::CodaArchive::repair(source, repaired);
  EXPECT_TRUE(repair);
  auto archive = std::move(*codec::CodaArchive::open(repaired));
  EXPECT_TRUE(archive.verify().ok);
  for (std::size_t index = 0; index < retired.size(); ++index) {
    auto extracted = archive.extract_stream_raw(stream, retired[index]);
    EXPECT_TRUE(extracted);
    EXPECT_EQ(*extracted, payloads[index]);
  }
  std::filesystem::remove(source);
  std::filesystem::remove(repaired);
}
```

- [ ] **Step 2: Make retired distributed slots a failing strict-parser proof**

In `distributed_wire_error_rejects_unknown_code_and_flags_with_valid_digest`, add:

```cpp
for (std::uint16_t retired = 10; retired <= 14; ++retired) {
  auto encoded = *base;
  encoded[64] = static_cast<std::byte>(retired & 0xffU);
  encoded[65] = static_cast<std::byte>(retired >> 8U);
  refresh_digest(encoded);
  expect_reply_protocol(encoded);
}
```

Remove the five watermark errors from `distributed_wire_error_reply_round_trips_all_current_error_codes`. Add stable-value assertions near that test:

```cpp
static_assert(static_cast<int>(codec::ErrorCode::identity_not_enrolled) == 15);
static_assert(static_cast<int>(codec::ErrorCode::identity_uncalibrated) == 16);
static_assert(static_cast<int>(codec::ErrorCode::cancelled) == 17);
static_assert(static_cast<int>(codec::ErrorCode::resource_exhausted) == 18);
static_assert(static_cast<int>(codec::ErrorCode::internal) == 19);
```

- [ ] **Step 3: Add source-contract exclusions for retired symbolic names**

Read the affected files in `tests/ai_contract.cmake` and reject these tokens in current runtime/public sources:

```cmake
function(reject_runtime_symbol source_name source_contents symbol)
  string(FIND "${source_contents}" "${symbol}" retired_offset)
  if(NOT retired_offset EQUAL -1)
    message(FATAL_ERROR
      "${source_name} still contains retired runtime symbol: ${symbol}")
  endif()
endfunction()

foreach(retired_symbol IN ITEMS
    "watermark_statement"
    "watermark_observation"
    "watermark_model_missing"
    "watermark_code_ambiguous"
    "watermark_signature_invalid"
    "watermark_replay_suspected"
    "watermark_path_unqualified"
    "CODEC_STATUS_WATERMARK")
  reject_runtime_symbol("include/codec/archive.hpp"
    "${archive_header_contents}" "${retired_symbol}")
  reject_runtime_symbol("include/codec/result.hpp"
    "${result_header_contents}" "${retired_symbol}")
  reject_runtime_symbol("include/codec/codec_c.h"
    "${c_header_contents}" "${retired_symbol}")
  reject_runtime_symbol("src/archive/archive.cpp"
    "${archive_source_contents}" "${retired_symbol}")
  reject_runtime_symbol("src/core/sha256.cpp"
    "${core_source_contents}" "${retired_symbol}")
  reject_runtime_symbol("src/capi/codec_c.cpp"
    "${capi_source_contents}" "${retired_symbol}")
  reject_runtime_symbol("src/distributed/wire.cpp"
    "${wire_source_contents}" "${retired_symbol}")
endforeach()
```

Define each `*_contents` variable with `read_required()` and its exact repository path before the loops.

- [ ] **Step 4: Run targeted tests and observe RED**

```bash
cmake --build build --parallel
ctest --test-dir build -R '^codec-unit$' --output-on-failure
cmake -DCODEC_SOURCE_DIR="$PWD" -P tests/ai_contract.cmake
```

Expected: FAIL because DRS1 slots 10-14 still decode successfully and the retired symbolic names remain.

- [ ] **Step 5: Remove registered archive names without reusing their codes**

Delete these enumerators from `RecordType` and their `record_type_name()` cases:

```cpp
watermark_statement = 20,
watermark_observation = 21,
```

Do not insert another enumerator at 20 or 21. Keep `feed_identity_event = 22` and all other explicit values unchanged.

- [ ] **Step 6: Remove watermark errors and preserve retained C++ values explicitly**

Replace `ErrorCode` with explicit retained values:

```cpp
enum class ErrorCode {
  ok = 0,
  invalid_argument = 1,
  unauthorized_source = 2,
  network = 3,
  protocol = 4,
  decode = 5,
  archive_io = 6,
  archive_corrupt = 7,
  model_incompatible = 8,
  inference = 9,
  identity_not_enrolled = 15,
  identity_uncalibrated = 16,
  cancelled = 17,
  resource_exhausted = 18,
  internal = 19,
};
```

Remove retired cases from `error_code_name()` and `status_for()`. Remove `CODEC_STATUS_WATERMARK = 9` from `codec_status_t`; do not reassign value 9 to another C status.

- [ ] **Step 7: Reserve distributed wire slots 10-14**

Remove the watermark cases from `error_code_to_wire()` and cases 10-14 from `error_code_from_wire()`. Keep the existing default decoder failure:

```cpp
return fail<ErrorCode>(ErrorCode::protocol,
                       "unknown distributed remote wire error code");
```

Do not change mappings 1-9 or 15-19.

- [ ] **Step 8: Run tombstone, contract, C ABI, and wire proof**

```bash
cmake --build build --parallel
ctest --test-dir build -R '^(codec-unit|codec-c-api|codec-ai-contract)$' \
  --output-on-failure
```

Expected: PASS, including byte-exact extraction for raw codes 20/21 and protocol rejection for wire slots 10-14.

- [ ] **Step 9: Commit compatibility tombstones**

```bash
git add include/codec/archive.hpp include/codec/result.hpp \
  include/codec/codec_c.h src/archive/archive.cpp src/core/sha256.cpp \
  src/capi/codec_c.cpp src/distributed/wire.cpp tests/test_archive.cpp \
  tests/test_distributed_wire.cpp tests/test_distributed_wire_strict.cpp \
  tests/ai_contract.cmake
git commit -m "refactor: reserve retired watermark identifiers"
```

---

### Task 5: Align current documentation and repository controls

**Files:**
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CONTRIBUTING.md`
- Modify: `CHANGELOG.md`
- Modify: `tests/ai_contract.cmake`

**Interfaces:**
- Consumes: final runtime and CLI surface from Tasks 2-4.
- Produces: current user/agent documentation that describes only implemented behavior while preserving historical release truth.

- [ ] **Step 1: Add current-documentation contract checks**

Extend `tests/ai_contract.cmake` with precise current-surface exclusions:

```cmake
foreach(retired_cli_text IN ITEMS
    "codec watermark"
    "codec list streams"
    "--stream STREAM_ID"
    "w0_ed25519"
    "w1_reference"
    "w2_reference"
    "w2_policy")
  string(FIND "${readme_contents}" "${retired_cli_text}" retired_offset)
  if(NOT retired_offset EQUAL -1)
    message(FATAL_ERROR
      "README.md still advertises retired surface: ${retired_cli_text}")
  endif()
endforeach()

string(FIND "${agents_contents}" "watermark" agents_watermark_offset)
if(NOT agents_watermark_offset EQUAL -1)
  message(FATAL_ERROR "AGENTS.md still requires watermarking")
endif()
string(FIND "${contributing_contents}" "watermark"
  contributing_watermark_offset)
if(NOT contributing_watermark_offset EQUAL -1)
  message(FATAL_ERROR "CONTRIBUTING.md still requires watermarking")
endif()
```

- [ ] **Step 2: Run the contract and observe RED**

```bash
cmake -DCODEC_SOURCE_DIR="$PWD" -P tests/ai_contract.cmake
```

Expected: FAIL because README and current agent guidance still advertise watermarking and generic-stream CLI commands.

- [ ] **Step 3: Rewrite the README's current behavior**

Make these exact content changes without editing historical documents:

- opening definition: describe record/verify/inspect/list-feeds/extract-feeds/repair only;
- CLI task table: remove generic-stream listing and all watermark rows;
- exit status: remove status 4;
- capabilities JSON/table: remove all four watermark fields;
- delete the `codec list streams` section;
- make the extract syntax `codec extract ARCHIVE --feed LABEL [--fidelity source-exact] [--follow] --output FILE`;
- state that omitted fidelity defaults to source-exact and explicit source-exact remains accepted;
- delete `--stream` argument/output documentation;
- remove stream-ID and watermark workflows;
- remove watermark metrics, Audio Profile claims, and security statements;
- retain the generic C++ stream API section and clarify it is library-only;
- add the approved removal spec to Developer and repository documentation.

Examples for both regular and live extraction must omit `--fidelity`:

```bash
codec extract session.coda --feed news --output recovered.bin
codec extract live.coda --feed live --follow --output live-copy.bin
```

- [ ] **Step 4: Align repository guidance and changelog**

In `AGENTS.md`, remove `watermark` from the Audio Profile boundary sentence while retaining the generic/profile separation rule. In `CONTRIBUTING.md`, remove watermark-specific candidate language and retain S0/S1/D plus Audio Stream Profile rules.

Add under `## Unreleased` in `CHANGELOG.md`:

```markdown
- Remove the complete reference watermark feature from the CLI and installed C++/C surfaces, including W0 statements, W1/W2 carriers, capability fields, profile aliases, implementation, and tests. Remove generic `list streams` and `extract --stream` CLI selection while retaining generic stream C++ APIs and unchanged feed listing/extraction, with source-exact fidelity documented as the default. Preserve former archive codes 20/21 as opaque unknown record tags and former distributed error slots 10-14 as unassigned compatibility tombstones; retained numeric mappings and CODA bytes are unchanged.
```

Do not modify the 0.1.0, 0.2.0, or 0.3.0 release sections.

- [ ] **Step 5: Run documentation and CLI contract tests**

```bash
cmake -DCODEC_SOURCE_DIR="$PWD" -P tests/ai_contract.cmake
cmake --build build --parallel
ctest --test-dir build -R '^(codec-ai-contract|codec-cli-integration)$' \
  --output-on-failure
```

Expected: PASS.

- [ ] **Step 6: Audit runtime remnants**

```bash
rg -n "watermark|w0_ed25519|w1_reference|w2_reference|w2_policy" \
  CMakeLists.txt include src tests README.md AGENTS.md CONTRIBUTING.md
```

Expected: no matches. Historical matches are permitted only in `CHANGELOG.md`, superseded design/spec/plan documents, and worksheet evidence.

- [ ] **Step 7: Commit documentation and controls**

```bash
git add README.md AGENTS.md CONTRIBUTING.md CHANGELOG.md tests/ai_contract.cmake
git commit -m "docs: describe reduced CODEC surface"
```

---

### Task 6: Run mandatory verification and record exact evidence

**Files:**
- Modify: `AI_WORKSHEET.md`

**Interfaces:**
- Consumes: completed implementation from Tasks 1-5.
- Produces: exact-head verification record and merge-readiness evidence; no PR, push, or merge.

- [ ] **Step 1: Confirm scope and cleanliness before verification**

```bash
git status --short
git diff --check 89bb674608169e18d2735621445c796c12c13dd3..HEAD
git diff --stat 89bb674608169e18d2735621445c796c12c13dd3..HEAD
```

Expected: clean worktree before generated build directories; no whitespace errors; only files named in this plan changed.

- [ ] **Step 2: Run the strict Release suite**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Expected: all configured tests pass, including unit, C ABI, CLI integration, installed-package consumers, and AI contract.

- [ ] **Step 3: Run the sanitizer suite**

```bash
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel
ctest --test-dir build-san --output-on-failure
```

Expected: all sanitizer tests pass with no AddressSanitizer or UndefinedBehaviorSanitizer report.

- [ ] **Step 4: Run CLI sanity and targeted negative checks**

```bash
./build/codec capabilities
./build/codec --help
set +e
./build/codec watermark > /tmp/codec-watermark.out 2>/tmp/codec-watermark.err
watermark_status=$?
./build/codec list streams missing.coda \
  > /tmp/codec-streams.out 2>/tmp/codec-streams.err
streams_status=$?
./build/codec extract missing.coda \
  --stream 00000000-0000-0000-0000-000000000001 \
  --output /tmp/codec-retired-output \
  > /tmp/codec-extract-stream.out 2>/tmp/codec-extract-stream.err
extract_status=$?
set -e
test "$watermark_status" -eq 2
test "$streams_status" -eq 2
test "$extract_status" -eq 2
test ! -e /tmp/codec-retired-output
```

Expected: capability/help output contains no retired fields or commands; all three retired invocations return 2 without output creation.

- [ ] **Step 5: Verify the installed package consumers**

```bash
codec_install_prefix="$PWD/build/install"
cmake --install build --prefix "$codec_install_prefix"
cmake -S tests/package_consumer -B build/package-consumer \
  -DCMAKE_PREFIX_PATH="$codec_install_prefix"
cmake --build build/package-consumer --parallel
./build/package-consumer/codec_package_consumer
```

Expected: configure, build, post-build remote consumer executions, and the main installed-package consumer all succeed without either retired header.

- [ ] **Step 6: Record verification in the worksheet**

Add the exact results and tested pre-record SHA to the active work record:

```yaml
verification:
  release_configure: pass
  release_build: pass
  tests: pass
  sanitizer_build: pass
  sanitizer_tests: pass
  installed_package_consumers: pass
  cli_capabilities: pass
  targeted_proof: pass
```

If any command cannot run, record `fail` plus the exact blocker; do not write `pass`.

- [ ] **Step 7: Commit the verification record and rerun the non-build checks**

```bash
git add AI_WORKSHEET.md
git commit -m "docs: record removal verification"
git status --short
git diff --check 89bb674608169e18d2735621445c796c12c13dd3..HEAD
git rev-parse HEAD
```

Expected: clean worktree, clean diff check, and one exact final head SHA for the completion report. Report local verification only; do not claim CI, PR, merge, or deployment.

# Stage F.7 Remote Envelope Codec Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add bounded deterministic DRQ1/DRS1 serialization for the existing F.6 remote-worker structured request, successful response, and `Error` result without adding a network transport.

**Architecture:** Add a separate installed public header `<codec/distributed_wire.hpp>` and focused `src/distributed/wire.cpp`. The codec owns only structural serialization, strict bounded decode, canonical length/reserved-field rules, stable error-code mapping, and SHA-256 corruption evidence; F.2 remains authoritative for partition exactness and processor/truth/provenance semantics.

**Tech Stack:** C++20, existing CODEC `Result`/`Error`, `ExtractedRecord`, `ProcessorOutput`, `ProvenanceProcess`, SHA-256 helper, CMake/CTest, GitHub Actions GCC/Clang/sanitizers.

**Spec:** `docs/superpowers/specs/2026-08-29-stage-f7-remote-envelope-codec-design.md`

## Global Constraints

- Base exact SHA: `5892437b150ab8e4e7b16ece95870c70130a553c` with post-merge CI run 277 green.
- Work branch: `automation/stage-f7-remote-envelope-codec`.
- Request magic is `DRQ1`; reply magic is `DRS1`; wire version is `1`.
- Common header is exactly 56 bytes, little-endian, with flags `0`, exact total/body lengths, and SHA-256 over bytes `[0,24)` followed by the body.
- The digest is corruption evidence only; no authentication/active-tamper claim.
- Unknown 16-bit record type codes round-trip exactly.
- Successful reply structural decode accepts only defined `TruthClass` enum values S0/S1/D; F.2 still decides whether an output may be S1/D.
- Error wire numbers are explicit/stable mappings, not implicit C++ enum ordinals; `ErrorCode::ok` is not a valid error reply.
- Default process text/details bounds are 4096 bytes / 1 MiB, aligned with existing SPV1 development-profile limits.
- No change to `src/distributed/worker.cpp`, `src/distributed/remote_worker.cpp`, or `src/distributed/scheduler.cpp` behavior is required.
- No socket/HTTP/gRPC/TLS/authentication/retry/discovery/concurrency/server/deployment functionality enters F.7.

---

### Task 1: Register the RED public wire contract

**Files:**
- Create: `include/codec/distributed_wire.hpp`
- Create: `tests/test_distributed_wire.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing `<codec/distributed.hpp>` F.6 types and `Error`.
- Produces: `DistributedRemoteWireLimits`, `DistributedRemoteExecutionRequest`, `DistributedRemoteExecutionReply`, `encode_distributed_remote_request`, `decode_distributed_remote_request`, `encode_distributed_remote_reply`, `decode_distributed_remote_reply`.

- [ ] **Step 1: Add only the failing test translation unit and CMake registration**

Create `tests/test_distributed_wire.cpp` with the smallest contract test that includes `<codec/distributed_wire.hpp>`, constructs a request, and calls the request encoder:

```cpp
#include "test.hpp"

#include <codec/distributed_wire.hpp>

#include <string>

TEST(distributed_wire_request_contract_exists) {
  codec::DistributedRemoteExecutionRequest request{
      .worker_name = "remote-a",
      .processor_name = "processor-a",
      .inputs = {},
  };
  auto encoded = codec::encode_distributed_remote_request(request);
  EXPECT_FALSE(encoded);
}
```

Register `tests/test_distributed_wire.cpp` in `codec_tests` but do not create the header yet.

- [ ] **Step 2: Run PR CI and verify RED is caused by the absent F.7 header/API**

Open a non-draft PR after committing the test-only RED head. Expected GCC/Clang/sanitizer build failure: missing `<codec/distributed_wire.hpp>` or missing F.7 public symbols. Existing production objects must compile before the new test translation unit fails.

- [ ] **Step 3: Add the public declarations, still without implementation**

Create `include/codec/distributed_wire.hpp`:

```cpp
#pragma once

#include <codec/distributed.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace codec {

inline constexpr std::uint16_t distributed_remote_wire_version = 1;

struct DistributedRemoteWireLimits {
  std::uint64_t maximum_envelope_bytes{128ULL * 1024ULL * 1024ULL};
  std::size_t maximum_input_records{1024};
  std::uint64_t maximum_input_bytes{64ULL * 1024ULL * 1024ULL};
  std::size_t maximum_outputs{1024};
  std::uint64_t maximum_output_bytes{64ULL * 1024ULL * 1024ULL};
  std::size_t maximum_label_bytes{256};
  std::size_t maximum_process_text_bytes{4096};
  std::uint64_t maximum_process_details_bytes{1024ULL * 1024ULL};
  std::size_t maximum_error_message_bytes{4096};
};

struct DistributedRemoteExecutionRequest {
  std::string worker_name;
  std::string processor_name;
  std::vector<ExtractedRecord> inputs;
};

using DistributedRemoteExecutionReply =
    std::variant<DistributedRemoteExecutionResponse, Error>;

Result<std::vector<std::byte>> encode_distributed_remote_request(
    const DistributedRemoteExecutionRequest& request,
    DistributedRemoteWireLimits limits = {});
Result<DistributedRemoteExecutionRequest> decode_distributed_remote_request(
    std::span<const std::byte> encoded,
    DistributedRemoteWireLimits limits = {});
Result<std::vector<std::byte>> encode_distributed_remote_reply(
    const DistributedRemoteExecutionReply& reply,
    DistributedRemoteWireLimits limits = {});
Result<DistributedRemoteExecutionReply> decode_distributed_remote_reply(
    std::span<const std::byte> encoded,
    DistributedRemoteWireLimits limits = {});

}  // namespace codec
```

Update the RED test to use a non-empty input fixture so linkage, not argument validation, is the next intentional failure.

- [ ] **Step 4: Commit the public-contract RED stage**

Commit message: `test: define Stage F.7 wire contract`.

---

### Task 2: Implement the deterministic DRQ1 request codec

**Files:**
- Create: `src/distributed/wire.cpp`
- Modify: `tests/test_distributed_wire.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 declarations, `sha256`, `RecordInfo::type_code()`.
- Produces: working deterministic request encode/decode with exact full record round trip.

- [ ] **Step 1: Expand RED request tests**

Add helpers for bytes and exact records, including an unknown record type:

```cpp
codec::ExtractedRecord make_input(codec::RecordTypeCode type,
                                  std::string_view stream_name,
                                  std::uint64_t sequence,
                                  std::string_view payload) {
  codec::ExtractedRecord out;
  out.record.type = static_cast<codec::RecordType>(type);
  out.record.sequence = sequence;
  out.record.stream = codec::derive_stream_id(stream_name);
  out.record.start_ns = static_cast<std::int64_t>(sequence * 10);
  out.record.end_ns = out.record.start_ns + 10;
  out.record.file_offset = 9000 + sequence;
  out.payload = bytes(payload);
  out.record.payload_size = static_cast<std::uint64_t>(out.payload.size());
  out.record.hash = codec::sha256(out.payload);
  return out;
}
```

Tests must assert:

- same request encodes identically twice;
- bytes begin with `DRQ1`;
- decode preserves worker/processor labels, input order, type code, sequence, stream, interval, payload size, file offset, hash, and payload;
- a decoded request re-encodes byte-for-byte identically;
- unknown type `0x7d55` survives exactly.

- [ ] **Step 2: Add request failure-path RED tests**

Cover encoder rejection of:

- any zero-valued `DistributedRemoteWireLimits` field;
- empty worker/processor label;
- empty input vector;
- label over limit;
- input count over limit;
- aggregate payload over limit;
- payload-size metadata mismatch;
- envelope over limit.

Cover decoder rejection of:

- wrong magic;
- version != 1;
- non-zero header flags;
- total/body length inconsistency;
- digest mismatch;
- truncation;
- appended trailing byte;
- non-zero per-record reserved field;
- declared payload/count/label/envelope limits above configured decode limits.

- [ ] **Step 3: Implement common framing and request encoding**

In `src/distributed/wire.cpp`, define private constants:

```cpp
constexpr std::size_t envelope_header_size = 56;
constexpr std::array<std::byte, 4> request_magic{
    std::byte{'D'}, std::byte{'R'}, std::byte{'Q'}, std::byte{'1'}};
constexpr std::array<std::byte, 4> reply_magic{
    std::byte{'D'}, std::byte{'R'}, std::byte{'S'}, std::byte{'1'}};
constexpr std::size_t digest_offset = 24;
constexpr std::size_t digest_size = 32;
constexpr std::size_t encoded_record_fixed_bytes = 92;
```

Add private little-endian `put_le/get_le`, checked-add helpers, hash copy helpers, common-limit validation, and a `finalize_envelope(magic, body, limits)` helper that fills the fixed header and digest.

Request encode preflights all lengths/aggregates before allocating the body. Use `u32` string/count lengths and the exact 92-byte fixed record metadata followed by payload bytes.

- [ ] **Step 4: Implement common header validation and request decode**

Validate the 56-byte header before body parsing:

```text
magic exact
version == 1
flags == 0
total == encoded.size()
body == total - 56
total <= maximum_envelope_bytes
SHA-256(prefix[0,24] + body) matches stored digest
```

Use a bounded reader that checks remaining bytes before every read. Read labels/count first, enforce configured limits before reserving the record vector, then decode each 92-byte record prefix and exact payload length. Reject any unconsumed body bytes.

- [ ] **Step 5: Run full CI and confirm request tests plus all regressions are green**

Required: GCC build/tests/install package consumer, Clang build/tests/install package consumer, sanitizer build/tests.

- [ ] **Step 6: Commit request codec**

Commit message: `feat: add F.7 remote request codec`.

---

### Task 3: Implement DRS1 success/error reply codec

**Files:**
- Modify: `src/distributed/wire.cpp`
- Modify: `tests/test_distributed_wire.cpp`

**Interfaces:**
- Consumes: common envelope helpers from Task 2, existing `DistributedRemoteExecutionResponse`, `ProcessorOutput`, `ProvenanceProcess`, `Error`.
- Produces: deterministic success/error reply encode/decode.

- [ ] **Step 1: Add success-reply RED round-trip coverage**

Build outputs containing:

```cpp
codec::ProvenanceProcess process{
    .operation = "f7-test",
    .implementation_id = "codec-test",
    .implementation_version = "1",
    .implementation_hash = codec::sha256(bytes("impl")),
    .configuration_hash = codec::sha256(bytes("config")),
    .created_utc_ns = 123456,
    .details_type = "application/f7-test",
    .details = bytes("details"),
};
```

Use at least two outputs with different stream/type/truth/payload values. Assert deterministic encode, `DRS1` magic, exact response-label/order/process/hash/details/payload round trip, and decode→encode byte identity.

Also round-trip a structurally valid `TruthClass::source_exact` output to prove F.7 is structural rather than duplicating F.2's S1/D rule.

- [ ] **Step 2: Add error-reply RED round-trip coverage**

Enumerate every current non-`ok` `ErrorCode` and assert code/message/retryable round trip. Include one message constructed with embedded NUL:

```cpp
std::string message{"bad\0wire", 8};
```

Assert byte-exact message preservation.

- [ ] **Step 3: Add reply failure-path RED tests**

Success-side encoder/decode coverage:

- empty or oversized response labels;
- output count over limit;
- aggregate output payload over limit;
- process text/details over configured limits;
- undefined truth value `static_cast<TruthClass>(0xff)` rejected structurally;
- unknown process flag bit rejected on decode;
- non-zero output reserved byte rejected;
- total envelope limit enforced.

Error-side coverage:

- `ErrorCode::ok` rejected by encoder;
- oversized error message rejected;
- unknown wire error number rejected;
- retryable byte other than 0/1 rejected;
- non-zero error reserved byte rejected.

Common reply coverage:

- wrong `DRS1` magic/version/flags;
- unknown outcome kind;
- non-zero seven-byte outcome reserved region;
- truncation/trailing byte/body-length/digest mismatch.

- [ ] **Step 4: Implement explicit stable error mapping**

Use explicit switches, not `static_cast` serialization. Freeze the current map:

```text
1 invalid_argument
2 unauthorized_source
3 network
4 protocol
5 decode
6 archive_io
7 archive_corrupt
8 model_incompatible
9 inference
10 watermark_model_missing
11 watermark_code_ambiguous
12 watermark_signature_invalid
13 watermark_replay_suspected
14 watermark_path_unqualified
15 identity_not_enrolled
16 identity_uncalibrated
17 cancelled
18 resource_exhausted
19 internal
```

`0` is reserved and never represents an error reply.

- [ ] **Step 5: Implement success body encoding/decoding**

Use outcome byte `0` plus seven zero bytes. Encode label lengths, output count, then each output with raw stream/type/truth/interval/payload size, process presence flags, created time, four process-text lengths, details length, optional hashes, process text/details, and payload.

Validate only structural enum domain and resource/canonicality rules. Do not add F.2 semantic checks for S0-vs-S1/D, reserved record types, interval order, non-empty process identity, embedded NUL, or details pairing.

- [ ] **Step 6: Implement error body encoding/decoding**

Use outcome byte `1` plus seven zero bytes, explicit wire error code, retryable byte, zero reserved byte, `u32` message length, and exact message bytes.

- [ ] **Step 7: Prove F.2 remains authoritative**

Add a test transport that decodes a prebuilt DRS1 success containing S0 output and returns the decoded success from `dispatch()`. Pass it through `RemoteDistributedWorker` + `execute_partition()` and assert F.2 returns `invalid_argument` after one dispatch. This test demonstrates that F.7 structural acceptance does not weaken F.2 truth semantics.

- [ ] **Step 8: Run full CI and commit reply codec**

Commit message: `feat: add F.7 remote reply codec`.

---

### Task 4: Prove installed-package usability and synchronize repository truth

**Files:**
- Create: `tests/package_consumer/remote_wire.cpp`
- Modify: `tests/package_consumer/CMakeLists.txt`
- Modify: `README.md`
- Modify: `AI_WORKSHEET.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: installed `<codec/distributed_wire.hpp>` and `codec::codec`.
- Produces: downstream build/run proof plus truthful F.7 status docs.

- [ ] **Step 1: Add installed-package remote-wire executable**

`tests/package_consumer/remote_wire.cpp` must use only installed public headers. It should:

1. build one exact input record;
2. encode/decode a `DistributedRemoteExecutionRequest` and verify labels/hash/payload;
3. encode/decode a successful reply with one D output and process identity;
4. encode/decode a retryable network `Error`;
5. return non-zero on any mismatch.

Add `codec_package_remote_wire_consumer` to `tests/package_consumer/CMakeLists.txt`, link only `codec::codec`, require C++20, and run it as a `POST_BUILD` command exactly like the F.6 remote consumer.

- [ ] **Step 2: Run GCC/Clang installed-package proof**

Do not claim package support until both release jobs pass the existing `Test installed package consumer` step with the new executable.

- [ ] **Step 3: Update README current truth**

Add one implemented generic bullet describing bounded deterministic DRQ1/DRS1 request/reply serialization, exact request record preservation, full output/process/error preservation, strict bounded decode, canonical SHA-256 corruption evidence, and explicit non-claims.

Update `planned_not_implemented` and Stage F roadmap wording so deterministic remote serialization is no longer listed as missing while concrete network transport/auth/discovery/retry/concurrency remain planned.

Add an F.7 paragraph under `## Distributed Processing Profile` and extend the repository-map description for `src/distributed/`.

- [ ] **Step 4: Update AI_WORKSHEET active work record**

Replace the F.6 active record with F.7, base SHA `5892437b...`, branch name, exact capability claim, proof files, invariants, and explicit non-claims. Keep S0/S1/D untouched.

- [ ] **Step 5: Add Unreleased changelog entry**

State only implemented behavior. Explicitly say no concrete network transport, endpoint policy, authentication, retry/failover, discovery, concurrency/server, persistence, deployment, or network performance/availability claim is added.

- [ ] **Step 6: Commit package/docs stage**

Commit message: `docs: record Stage F.7 remote wire codec`.

---

### Task 5: Final exact-head review, CI gate, merge, and dependency audit

**Files:**
- No new implementation files unless review/CI exposes a defect.
- Roadmap issue #10 receives the completion record after merge.

**Interfaces:**
- Consumes: complete feature branch.
- Produces: exact-green squash merge to `main`, completion evidence, and fresh next-dependency selection.

- [ ] **Step 1: Diff audit from base**

Compare `5892437b...` to the final feature head. Expected scope is the F.7 spec/plan, one public wire header, one wire implementation, one unit-test file, package-consumer proof/wiring, CMake registration, and README/worksheet/changelog updates. Confirm no behavioral diff in F.6 worker/scheduler files and no generated artifacts/credentials.

- [ ] **Step 2: Code review gate**

Review the complete PR diff for integer overflow, pre-allocation bounds, digest coverage, decode exact-consumption, stable error mapping, unknown type preservation, semantic duplication with F.2, and accidental capability overclaim. Any Critical/Important finding blocks merge.

- [ ] **Step 3: Exact-head CI gate**

Required final workflow on the exact final head:

```text
build (gcc): configure/build/full CTest/install/installed package consumer = success
build (clang): configure/build/full CTest/install/installed package consumer = success
sanitizers: configure/build/CTest = success
```

- [ ] **Step 4: Merge with exact head guard**

Squash merge the PR only with `expected_head_sha=<the exact green head>`. If the head changes, rerun the full gate.

- [ ] **Step 5: Verify main push CI**

Confirm `main` points at the squash merge commit and its push-triggered CI completes successfully before recording completion.

- [ ] **Step 6: Update roadmap issue #10 and audit Stage F dependencies**

Record RED/GREEN/package/final-CI/merge SHAs and explicit non-claims. Then search merged code/docs for the next unmet Stage F dependency rather than assuming a number. Candidate direction after F.7 is a concrete transport adapter, but the audit must decide whether endpoint/request handling, transport mapping, or another prerequisite is smallest.
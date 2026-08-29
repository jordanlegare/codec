# Stage F.7 — bounded deterministic remote-worker envelope codec

## Status

Approved for implementation by the user's Stage F.7 go-ahead. This document freezes the smallest dependency identified after F.6: deterministic, bounded serialization for the existing provider-neutral remote-worker request/result structures. It does not add a network provider.

## Context

Stage F.6 added `DistributedWorkerTransport` and `RemoteDistributedWorker`. The transport interface currently exchanges structured in-memory C++ values:

- configured worker label;
- configured processor label;
- ordered materialized `ExtractedRecord` inputs;
- either `DistributedRemoteExecutionResponse` or `Error`.

A future HTTP/gRPC/socket provider cannot interoperate without inventing a private serialization format. F.7 closes only that dependency.

## Scope decision

F.7 adds a separate public header, `<codec/distributed_wire.hpp>`, plus one implementation unit. It does not change `DistributedWorker`, `RemoteDistributedWorker`, `execute_partition()`, or `schedule_partitions()`.

The wire codec is a versioned development-profile interoperability contract, not a complete RPC protocol and not a frozen CODA format. It provides deterministic bytes, strict decode, explicit bounds, and corruption detection. Endpoint selection, framing on a socket, HTTP/gRPC mapping, TLS, credentials, authentication, retries, discovery, concurrency, cancellation, idempotency, exactly-once behavior, persistence, and deployment remain outside F.7.

## Public API

```cpp
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
```

The request owns its decoded records. The reply is exactly one successful F.6 response or one `Error`; an envelope cannot contain both.

## Common envelope framing

F.7 uses two distinct magic values:

- request: ASCII `DRQ1`;
- reply: ASCII `DRS1`.

Both use one fixed 56-byte little-endian header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic |
| 4 | 2 | version = 1 |
| 6 | 2 | flags = 0 |
| 8 | 8 | total envelope bytes |
| 16 | 8 | body bytes |
| 24 | 32 | SHA-256 over header bytes `[0,24)` followed by the body |

`total_bytes` must equal `56 + body_bytes` and exactly equal the supplied encoded span. No trailing bytes are accepted. Reserved flags must be zero. The digest detects accidental/malicious byte corruption but is not a signature, MAC, authentication, or authorization claim.

All integers are little-endian, matching CODEC's generic CMX1/SPV1 development-profile conventions.

## Request body

The request body is:

1. worker label: `u32 length + raw string bytes`;
2. processor label: `u32 length + raw string bytes`;
3. input count: `u32`;
4. ordered input records.

Each input record contains:

| Field | Encoding |
|---|---|
| record type | raw `RecordTypeCode` as `u16` |
| reserved | `u16 = 0` |
| archive sequence | `u64` |
| stream | exact 16 `StreamId` bytes |
| start/end | `i64`, `i64` |
| payload size | `u64` |
| original file offset | `u64` |
| record SHA-256 | 32 bytes |
| payload | exactly `payload_size` bytes |

F.7 preserves unknown 16-bit record type codes by round-tripping their underlying value. It preserves the complete `RecordInfo`, including `file_offset`, even though F.2 exact partition membership currently keys on stream/type/sequence/hash.

The encoder requires non-empty labels and inputs, label/count/aggregate payload bounds, and `record.payload_size == payload.size()`. It does not duplicate F.2's CDP1 identity, partition-membership, stream, or payload-hash verification because no partition descriptor is present in this codec and F.2 remains authoritative when the decoded request is executed.

The decoder performs the same structural/bound checks before allocation and rejects non-zero reserved fields, truncation, length overflow, aggregate-limit overflow, and trailing body bytes. It reconstructs unknown record types with their exact 16-bit value.

## Successful reply body

A reply body starts with:

- outcome kind `u8`: `0 = success`, `1 = error`;
- seven reserved zero bytes.

For a success reply the remainder is:

1. worker label (`u32 length + bytes`);
2. processor label (`u32 length + bytes`);
3. output count (`u32`);
4. ordered `ProcessorOutput` values.

Each output preserves:

- exact 16-byte `StreamId`;
- raw `RecordTypeCode` (`u16`);
- `TruthClass` (`u8`), accepting only the defined enum domain S0/S1/D structurally;
- one reserved zero byte;
- start/end `i64`;
- payload length `u64`;
- process flags `u32`: bit 0 = implementation hash present, bit 1 = configuration hash present; all other bits rejected;
- `created_utc_ns` (`i64`);
- four `u32` text lengths for operation, implementation id, implementation version, and details type;
- details length (`u64`);
- optional hashes in implementation/configuration order;
- the four process text byte strings in the same order;
- process details bytes;
- output payload bytes.

Per-process text uses the same 4096-byte default bound and process details use the same 1 MiB default bound as the existing SPV1 provenance development profile. F.7 preserves bytes structurally; F.2/invoke_processor remains authoritative for non-empty process identity, embedded-NUL restrictions, details-type/details pairing, S1/D-only output truth, interval semantics, reserved artifact types, and processor provenance validity.

Success encode/decode also enforces label, output-count, aggregate output-payload, per-process metadata, total envelope, flags, truncation, and exact-consumption bounds.

## Error reply body

For outcome kind `1`, the remainder is:

- stable wire error code `u16`;
- retryable `u8` (`0` or `1` only);
- one reserved zero byte;
- message length `u32`;
- exact message bytes.

F.7 does not rely on the C++ enum's implicit ordinal as a long-term wire contract. The implementation uses an explicit bidirectional mapping table assigning stable wire numbers to every current `ErrorCode`. `ErrorCode::ok` is invalid in an error reply. Unknown wire error codes are rejected as `protocol`.

Error message bytes may contain embedded NUL and are round-tripped exactly, subject only to the configured byte bound. This preserves the existing F.6 `Error` contract rather than introducing text semantics.

## Error taxonomy

Caller-owned values rejected by an encoder use `invalid_argument` for malformed shape/configuration and `resource_exhausted` for configured resource-limit excess or allocation failure.

Malformed encoded bytes use `protocol` for incompatible magic/version/flags/outcome/reserved fields, invalid enum/wire-code values, length inconsistencies, digest mismatch, truncation, or trailing bytes. Configured decode-limit excess uses `resource_exhausted`.

This distinction keeps transport/provider failures separate from wire-format failures.

## Determinism and canonicality

For the same structured value and limits, encoding produces byte-for-byte identical output.

Canonicality rules include:

- one integer byte order;
- one field order;
- exact explicit lengths;
- zero reserved fields;
- no alternate representation for reply outcome;
- one order for optional process hashes;
- exact total/body lengths;
- no trailing bytes;
- one digest position and hash input.

Decode followed by encode must reproduce the original bytes for every accepted envelope.

## Security and trust boundary

F.7 treats encoded envelopes as untrusted input. It validates the fixed header and declared sizes against caller limits before allocating variable-sized fields, uses checked arithmetic for aggregate sizes, and rejects unknown flags and enum values.

The SHA-256 envelope digest is integrity evidence only. It does not identify a worker, authenticate a processor, authorize execution, protect confidentiality, prevent replay, bind an endpoint, or prove that a remote machine executed anything. Worker and processor names remain descriptive labels exactly as in F.6.

## Testing contract

RED must first prove the public codec API is absent.

GREEN coverage must include:

- deterministic request encode and exact request round trip;
- preservation of full `RecordInfo`, payload bytes, order, and unknown 16-bit record type codes;
- deterministic success reply encode and exact round trip of output/process metadata, optional hashes, details, payload, and labels;
- exact error reply round trip including `retryable`, embedded-NUL message bytes, and every current non-`ok` `ErrorCode` mapping;
- decode→encode canonical byte identity;
- magic/version/flags/reserved/outcome/truncation/trailing-byte/length/digest corruption rejection;
- unknown error wire code and invalid retryable byte rejection;
- input/output/label/process/error/envelope resource-limit rejection;
- F.2 semantic separation: structurally valid S0 output may round-trip through F.7 but remains rejected when supplied through the existing F.2 validation path;
- installed-package consumer proof using only installed public headers and `codec::codec`;
- unchanged full GCC, Clang, sanitizer, C ABI, CLI, Audio, Stage E, and F.1-F.6 regression suite.

## Files

Expected implementation scope:

- `include/codec/distributed_wire.hpp`;
- `src/distributed/wire.cpp`;
- `tests/test_distributed_wire.cpp`;
- `tests/package_consumer/remote_wire.cpp` and package-consumer CMake wiring;
- root `CMakeLists.txt` source/test registration;
- `README.md`, `AI_WORKSHEET.md`, and `CHANGELOG.md` truth synchronization;
- this design plus the implementation plan.

No F.6 scheduler/worker implementation file should require a behavioral change.

## Explicit non-goals

F.7 adds no socket, HTTP, HTTPS, TLS, QUIC, gRPC, protobuf, JSON, CBOR, endpoint or DNS policy, SSRF defense, credential store, authentication, authorization, attestation, signing, encryption, replay protection, request IDs, correlation IDs, retries, failover, leases, heartbeats, cancellation protocol, idempotency key, exactly-once claim, worker discovery, health/capability negotiation, concurrent dispatch, server loop, object-store integration, automatic CODA persistence, deployment integration, or network performance/availability/scale claim.

A concrete remote transport remains a later Stage F milestone built on this codec.
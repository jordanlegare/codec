# Stage E.4 Bounded XOR Single-Erasure Recovery

## Work record

```yaml
task: Add a versioned bounded XOR repair symbol that reconstructs and exactly verifies one missing E.1 CMX1 frame from one explicit E.2 recovery group.
base_ref: origin/main
base_head_sha: bd8a15ec3e3306b0fbe415064b128fb2e313f6f4
work_branch: automation/stage-e4-xor-recovery
current_version: 0.2.0
active_roadmap_stage: E — transport/recovery profile; E.1 multiplexing, E.2 loss/group semantics, and E.3 concurrent recording/follow extraction are integrated
scope: generic transport recovery
touched_truth_classes: []
coda_layout_delta: none
cmx1_layout_delta: none
new_capability_claim: one known missing CMX1 frame can be reconstructed byte-for-byte from a bounded XRF1 XOR symbol and all other exact group members, then verified against its committed encoded length and SHA-256 before being decoded and returned
change_class: generic_stream_abstraction
```

## Decision

Stage E.4 adds the first concrete recovery scheme above the existing E.1 and E.2 boundaries. It uses one XOR parity symbol per explicit `RecoveryGroupDescriptor` and supports exactly one known erasure. The parity calculation covers the complete deterministic CMX1 encoding of each source frame, not only its payload, so successful recovery restores the missing frame's stream identity, sequence, epochs, clock, interval, payload, and embedded CMX1 integrity digest exactly.

The repair symbol is an independent deterministic `XRF1` byte format. It binds the recovery-group descriptor, every source frame's exact encoded length, every exact encoded-frame SHA-256, and the zero-padded XOR parity. A trailing SHA-256 detects repair-symbol corruption. SHA-256 remains integrity-only; neither XRF1 nor CMX1 authenticates a sender.

Recovery is fail-closed. A caller supplies all observed source frames for the group. E.4 requires exactly `source_count - 1` unique valid members, re-encodes each through the existing CMX1 encoder, verifies its declared length and hash, identifies the one absent source sequence, XORs the observed encoded bytes out of the parity, truncates to the committed missing length, verifies the missing hash, decodes exactly one complete CMX1 frame, and rechecks group membership before returning both the exact encoded bytes and decoded frame.

This is exact encoded-data recovery after a known erasure. It is not reconstruction of pre-lossy source content and does not create an S0, S1, or D record.

## Alternatives considered

1. **Versioned XOR symbol over exact CMX1 bytes — selected.** It is dependency-free, deterministic, bounded, and proves a real exact-recovery path while preserving all frame metadata. Its one-erasure limit is explicit and easy to validate exhaustively.
2. **A generic FEC provider interface without an implementation — rejected for E.4.** E.2 already supplies an algorithm-neutral semantic boundary. Another interface would not prove that CODEC can generate or consume a repair symbol.
3. **Reed–Solomon or fountain coding — deferred.** Multi-erasure recovery is useful, but a new finite-field implementation or third-party dependency would enlarge the security, compatibility, packaging, and qualification surface before the exact repair-symbol contract is established.
4. **XOR only the payloads — rejected.** That would require inferred or separately reconstructed clocks, intervals, epochs, and other CMX1 metadata, weakening the exactness claim.

## Public API

Add `include/codec/xor_recovery.hpp`.

```cpp
#pragma once

#include <codec/integrity.hpp>
#include <codec/recovery.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace codec {

inline constexpr std::uint16_t xor_repair_symbol_version = 1;

struct XorRepairLimits {
  MultiplexLimits multiplex{};
  std::size_t maximum_source_frames{256};
  std::uint64_t maximum_total_encoded_source_bytes{64ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_symbol_bytes{32ULL * 1024ULL * 1024ULL};
};

struct XorRepairSymbol {
  RecoveryGroupDescriptor descriptor{};
  std::vector<std::uint64_t> encoded_frame_sizes;
  std::vector<Sha256> encoded_frame_hashes;
  std::vector<std::byte> parity;
};

struct XorRecoveredFrame {
  MultiplexFrame frame{};
  std::vector<std::byte> encoded_frame;
  Sha256 encoded_frame_hash{};
};

Result<XorRepairSymbol> create_xor_repair_symbol(
    const RecoveryGroupDescriptor& descriptor,
    std::span<const MultiplexFrame> source_frames,
    XorRepairLimits limits = {});

Result<std::vector<std::byte>> encode_xor_repair_symbol(
    const XorRepairSymbol& symbol, XorRepairLimits limits = {});

Result<XorRepairSymbol> decode_xor_repair_symbol(
    std::span<const std::byte> bytes, XorRepairLimits limits = {});

Result<XorRecoveredFrame> recover_xor_single_erasure(
    const XorRepairSymbol& symbol,
    std::span<const MultiplexFrame> observed_source_frames,
    XorRepairLimits limits = {});

}  // namespace codec
```

The API is generic. It depends only on the generic CMX1 and E.2 recovery types and adds no media, audio, inference, archive, network-provider, or vendor-specific field.

## Source-group validation

`RecoveryGroupDescriptor` remains the only group identity and range contract. E.4 tightens the input required by this concrete scheme:

- `source_count` must be at least 2;
- `source_count` must not exceed `maximum_source_frames` or addressable vector size;
- `[first_sequence, first_sequence + source_count)` must not overflow;
- every source frame must have the exact descriptor `StreamId` and exact connection/format epoch;
- source frame sequences must cover the full descriptor range exactly once;
- input order is irrelevant; slot order in the symbol is canonical ascending source sequence;
- each frame must encode successfully under `limits.multiplex`;
- each encoded frame and the aggregate encoded source bytes must stay within the configured limits.

Duplicate, missing, unrelated, out-of-range, or malformed source frames fail before returning a symbol. Caller-visible output is transactional.

## XOR construction

For source slot `i`, E.4 stores:

1. `encoded_frame_sizes[i]`, the exact CMX1 encoded length;
2. `encoded_frame_hashes[i] = sha256(exact_cmx1_bytes)`.

`parity.size()` is the maximum encoded-frame size in the group. Each source CMX1 byte vector is conceptually right-padded with zero bytes to that length. The parity byte at offset `j` is the XOR of byte `j` from every padded source vector.

The explicit length table distinguishes real trailing zero bytes from XOR padding. The per-frame hash commits the exact expected byte vector and prevents a syntactically valid but incorrect reconstruction from being returned. The CMX1 decoder independently verifies the recovered frame's internal integrity digest and metadata validity.

## XRF1 wire format

All integers are unsigned little-endian. The format is canonical and contains no optional fields in version 1.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `XRF1` |
| 4 | 2 | version, exactly `1` |
| 6 | 2 | flags, exactly `0` |
| 8 | 4 | fixed header size, exactly `92` |
| 12 | 8 | total symbol bytes, including trailing digest |
| 20 | 16 | `StreamId` |
| 36 | 8 | connection epoch |
| 44 | 8 | format epoch |
| 52 | 8 | recovery-group sequence |
| 60 | 8 | first source sequence |
| 68 | 8 | source count |
| 76 | 8 | parity byte count |
| 84 | 4 | table-entry size, exactly `40` |
| 88 | 4 | reserved, exactly `0` |
| 92 | `source_count * 40` | canonical ascending source-slot table |
| variable | `parity byte count` | XOR parity bytes |
| final | 32 | SHA-256 of every preceding XRF1 byte |

Each 40-byte table entry contains an 8-byte exact CMX1 length followed by its 32-byte SHA-256. Entry `i` always describes sequence `first_sequence + i`; sequence values are therefore not repeated in the table.

Canonical structural rules:

- source count is at least 2 and within `maximum_source_frames`;
- every exact frame length is nonzero, is representable, does not exceed `limits.multiplex.maximum_buffered_bytes`, and contributes without overflow to the configured aggregate bound;
- parity size is nonzero, equals the maximum declared frame length, and does not exceed the multiplex buffer limit;
- computed header, table, parity, trailer, and total sizes cannot overflow and equal the one canonical total exactly;
- total size does not exceed `maximum_symbol_bytes` or addressable memory;
- truncation, trailing bytes, unknown version/flags, noncanonical sizes, nonzero reserved data, invalid group range, inconsistent parity size, and digest mismatch fail before returning a symbol.

The trailing digest protects accidental or unauthenticated corruption only. A party that can replace the symbol can replace and rehash its commitments; authentication belongs to a later trust/transport layer.

## Recovery algorithm

`recover_xor_single_erasure()` validates limits and the symbol as strictly as the encoder before inspecting observed frames. It then:

1. requires exactly `source_count - 1` observed frames;
2. maps frames to source slots by exact stream, exact epoch, and sequence range;
3. rejects duplicates and unrelated frames;
4. deterministically re-encodes every observed frame under the configured CMX1 limits;
5. verifies each observed encoded length and SHA-256 against its XRF1 slot commitment;
6. copies parity into one bounded working buffer and XORs every observed encoded byte vector into it;
7. truncates the buffer to the missing slot's committed length;
8. verifies the recovered exact encoded SHA-256;
9. decodes the bytes through `MultiplexDecoder`, requiring exactly one complete frame and a successful `finish()`;
10. rechecks that the decoded stream, epoch, and sequence match the missing slot;
11. returns the exact recovered encoded bytes, decoded frame, and verified encoded-frame hash.

No result is returned if zero or more than one source frame is absent. E.4 does not guess which corrupted present frame should be treated as an erasure. A wrong present member, wrong parity, or inconsistent commitment fails closed.

## Limits and allocation behavior

`XorRepairLimits` composes the existing `MultiplexLimits` so callers control the same maximum payload/buffer geometry used for CMX1 encoding and decoding. The E.4-specific limits additionally bound:

- group member count;
- aggregate exact CMX1 bytes processed during symbol creation or recovery;
- complete XRF1 wire bytes.

All limits must be nonzero and internally consistent. Size multiplication and addition are checked before allocation. Allocation failure at this boundary maps to `resource_exhausted`. Encode/decode/create/recover return no partial vector or recovered frame on failure.

`maximum_total_encoded_source_bytes` bounds the sum of exact source-frame encodings. Working memory is also bounded by the source vectors or caller-provided frames, the XRF1 symbol, and one parity/recovery buffer. E.4 makes no constant-memory or zero-copy claim.

## Error mapping

- invalid limits, invalid group descriptors, incomplete creation inputs, duplicate or unrelated recovery members, or a recovery request with zero/multiple erasures: `invalid_argument`;
- declared or actual count/byte/allocation limits exceeded: `resource_exhausted`;
- malformed, noncanonical, corrupt, truncated, or trailing XRF1 bytes: `protocol`;
- observed or reconstructed CMX1 bytes that conflict with committed lengths/hashes or cannot decode as the exact missing member: `protocol`;
- unexpected stored-state invariant failure: `internal`.

No retryability claim is added.

## Relationship to E.1, E.2, and E.3

- **E.1 CMX1:** unchanged. E.4 calls the existing deterministic encoder and incremental decoder; it does not add fields or flags to CMX1.
- **E.2 recovery semantics:** reused directly through `RecoveryGroupDescriptor`. A sealed incomplete group can identify the missing sequence range that a caller uses with XRF1. E.4 does not mutate `RecoveryGroupTracker` or reinterpret provisional loss observations.
- **E.3 concurrent recording/follow extraction:** unchanged. Local CODA recording does not automatically create XRF1 symbols, wrap records in CMX1, or persist recovered frames.

The boundaries remain separable so later streaming repair or network transport can choose when and where to transmit, persist, or apply a symbol.

## Truth, archive, compatibility, and security effects

- **S0/S1/D:** unchanged. Recovered bytes are verified transport data, not an automatically classified CODA record.
- **CODA:** no header, envelope, record type, reader, writer, repair, or persistence change.
- **CMX1:** no layout or version change.
- **Public compatibility:** one additive C++ header and symbols; no C ABI or CLI change.
- **Dependencies:** no new runtime or link dependency.
- **Authentication:** none. SHA-256 detects inconsistency relative to supplied XRF1/CMX1 data; it does not prove origin or authorization.

## Implementation structure

- `include/codec/xor_recovery.hpp` — public limits, symbol/result structures, and create/encode/decode/recover functions.
- `src/transport/xor_recovery.cpp` — canonical XRF1 codec, bounded XOR construction, and exact recovery implementation.
- `tests/test_transport_xor_recovery.cpp` — focused E.4 TDD proof and E.1/E.2 integration.
- `CMakeLists.txt` — production/test source registration.
- `tests/package_consumer/main.cpp` — installed public-header/link proof.
- `README.md`, `CHANGELOG.md` — current runtime truth and explicit non-claims.

## Proof contract

The E.4 suite must first establish RED against the absent public API, then prove GREEN for:

1. create accepts an unordered exact group and canonicalizes slots by ascending sequence;
2. XRF1 encoding is deterministic, versioned, little-endian, and decode round-trips the exact descriptor, size table, hashes, and parity;
3. variable-length source frames produce parity sized to the longest exact CMX1 frame;
4. omitting each source position in turn reconstructs byte-for-byte the exact deterministic CMX1 encoding and every decoded frame field;
5. actual E.2 sealed incomplete-group missing ranges agree with the recovered source sequence;
6. observed frames may arrive out of order without changing recovery output;
7. wrong stream, epoch, sequence, duplicate slot, present-frame content, or symbol parity/hash fails closed;
8. zero-erasure and multiple-erasure recovery requests are rejected without output;
9. invalid creation groups, incomplete/full-range mismatch, sequence overflow, and invalid CMX1 metadata are rejected;
10. zero/inconsistent limits, source count, aggregate bytes, frame bytes, symbol bytes, and allocation-representability bounds are enforced before partial output;
11. unknown magic/version/flags/header/table sizes, nonzero reserved data, noncanonical parity length, truncation, trailing bytes, and XRF1 digest mismatch are rejected;
12. a semantically corrupted XRF1 with a recomputed trailing digest is still rejected by invariant, frame-commitment, or recovered-CMX1 checks as applicable;
13. existing CMX1 bytes and all CODA/audio/C API/CLI behavior remain unchanged;
14. an installed-package consumer includes and exercises `<codec/xor_recovery.hpp>` with no extra dependency;
15. Release GCC/Clang, leak-enabled sanitizer CI, package-consumer, capability, and AI-contract gates remain green.

## Non-claims

E.4 does not implement multiple-erasure Reed–Solomon, fountain/Raptor, convolutional or rateless coding; bit-error location/correction; corrupted-member selection; inter-group coding; retransmission/ARQ; socket/TCP/UDP/QUIC/WebSocket transport; packet scheduling, reordering, congestion, or QoS policy; encryption, signatures, or authentication; automatic CODA or CMX1 persistence; recovery of pre-lossy source information; S0/S1/D classification; CLI or C ABI recovery; benchmarked loss tolerance, latency, throughput, memory, or scale; a frozen normative recovery standard; streaming repair orchestration; or Stage E completion.

The next recovery milestone after E.4 should compose this exact single-erasure primitive into a bounded streaming repair session or deliberately select and qualify a separately versioned multi-erasure scheme. Neither claim is part of E.4.

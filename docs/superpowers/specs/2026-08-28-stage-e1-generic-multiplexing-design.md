# Stage E.1 Generic Multiplexing Design

## Work record

```yaml
task: Add a bounded deterministic generic multiplex framing layer that carries many logical CODEC streams over one physical byte stream without changing logical stream identity or CODA truth semantics.
base_ref: main
base_head_sha: 592b34646bd42095bf804a9d850b10747c13a840
work_branch: automation/stage-e1-multiplexing
current_version: 0.1.0
active_roadmap_stage: E — transport/recovery profile; D.9 is integrated and the approved architecture selects generic multiplexing/recovery semantics next
continuity_evidence:
  git_head: main at 592b34646bd42095bf804a9d850b10747c13a840 at start
  exact_head_ci: post-D.9 main CI 127 / 33204127986 succeeded
  roadmap_issue: issue 10 records D.9 integrated all-green
scope: generic transport multiplexing
truth_classes: unchanged
coda_layout_delta: none
new_capability_claim: bounded corruption-detecting byte-stream framing can interleave records from many logical StreamIds while preserving each frame's independent sequence, epoch, clock, interval, and opaque payload exactly
change_class: transport_profile
```

## Decision

Stage E.1 adds a generic versioned byte-stream multiplex frame and an incremental decoder. A physical byte stream is simply a concatenation of frames. Frames from unrelated logical `StreamId`s may be interleaved arbitrarily; the decoder returns them in physical arrival order with their logical metadata unchanged.

E.1 is deliberately below recovery semantics. It does not infer gaps, reorder frames, retransmit, generate parity, reconstruct loss, or enforce per-stream sequence continuity. Those behaviors belong to later Stage E milestones. The multiplex layer only frames, bounds, integrity-checks, and demultiplexes logical records.

The framing API is transport-neutral and contains no socket, TCP, UDP, QUIC, cloud, file, worker, or archive-placement identity. A connection may carry any number of logical streams, and a logical stream may later migrate between connections without changing `StreamId`.

## Alternatives considered

1. **Versioned framed byte stream — selected.** This proves actual physical multiplexing, arbitrary read chunking, backpressure, corruption detection, and exact logical metadata preservation without depending on a network stack.
2. **Message-level mux API only — rejected for E.1.** Passing a vector of tagged messages would show routing but would not establish a physical framing boundary or incremental parser behavior.
3. **CODA archive interleaving — rejected as the wrong layer.** CODA already stores many streams. Stage E must separate transport from archive placement rather than redefine archive multiplexing.

## Public API

Add `include/codec/transport.hpp`:

```cpp
namespace codec {

inline constexpr std::uint16_t multiplex_frame_version = 1;

struct MultiplexFrame {
  StreamId stream{};
  std::uint64_t sequence{};
  StreamEpoch epoch{};
  StreamClock clock{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::vector<std::byte> payload;
};

struct MultiplexLimits {
  std::uint64_t maximum_payload_bytes{16ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_buffered_bytes{32ULL * 1024ULL * 1024ULL};
  std::size_t maximum_frames_per_push{4096};
};

Result<std::vector<std::byte>> encode_multiplex_frame(
    const MultiplexFrame& frame, MultiplexLimits limits = {});

class MultiplexDecoder {
 public:
  explicit MultiplexDecoder(MultiplexLimits limits = {});
  MultiplexDecoder(MultiplexDecoder&&) noexcept;
  MultiplexDecoder& operator=(MultiplexDecoder&&) noexcept;
  ~MultiplexDecoder();

  MultiplexDecoder(const MultiplexDecoder&) = delete;
  MultiplexDecoder& operator=(const MultiplexDecoder&) = delete;

  Result<std::vector<MultiplexFrame>> push(
      std::span<const std::byte> bytes);
  Result<void> finish();
  std::size_t buffered_bytes() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace codec
```

The public type intentionally carries only generic logical-stream metadata and opaque bytes. It does not carry a truth class, CODA `RecordTypeCode`, archive sequence, physical connection identifier, worker identifier, or transport provider. Higher layers decide what the payload means and whether received bytes are preserved as S0, interpreted as S1, or used to produce D.

## Frame format

E.1 uses a fixed little-endian version-1 frame named `CMX1`. The version-1 header is exactly 164 bytes:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `CMX1` |
| 4 | 2 | version = 1 |
| 6 | 2 | flags = 0 |
| 8 | 4 | header size = 164 |
| 12 | 8 | total frame size |
| 20 | 16 | logical `StreamId` |
| 36 | 8 | per-stream sequence |
| 44 | 8 | connection epoch |
| 52 | 8 | format epoch |
| 60 | 8 | start_ns |
| 68 | 8 | end_ns |
| 76 | 8 | monotonic_ns |
| 84 | 8 | observed_utc_ns |
| 92 | 8 | observed_utc_uncertainty_ns |
| 100 | 8 | source_timestamp |
| 108 | 8 | source_timebase_numerator |
| 116 | 8 | source_timebase_denominator |
| 124 | 8 | payload size |
| 132 | 32 | frame SHA-256 |
| 164 | N | opaque payload |

`total frame size` must equal `164 + payload size` with checked arithmetic.

The frame SHA-256 is computed over bytes `[0, 132)` followed immediately by the payload. The stored digest field itself is excluded. This detects accidental or untrusted-wire corruption of all semantic header fields and payload bytes. It is **not** a signature, MAC, authenticity, authorization, or adversarial-tamper-resistance claim because an attacker able to rewrite the frame can also recompute an unkeyed SHA-256.

Unknown versions, nonzero version-1 flags, a different version-1 header size, inconsistent lengths, invalid timebase, inverted interval, or digest mismatch fail closed with `ErrorCode::protocol` on decode.

## Encoder validation

Before allocating output, `encode_multiplex_frame` requires:

- all configured limits to be internally valid and nonzero;
- positive source-timebase numerator and denominator;
- `end_ns >= start_ns`;
- payload length no greater than `maximum_payload_bytes`;
- checked `164 + payload_size` representability and a frame size no greater than `maximum_buffered_bytes`.

Caller-supplied invalid metadata returns `invalid_argument`. Caller size limits return `resource_exhausted`. Encoding never mutates the frame.

Empty payloads are valid. A zero-valued `StreamId`, epoch, sequence, or timestamp is not rejected because the generic core does not define those values as globally invalid.

## Incremental decoder and backpressure

`MultiplexDecoder` accepts arbitrary physical chunks. A caller may provide one byte at a time, partial headers, partial payloads, several complete frames, or an empty span used only to drain already-buffered complete frames.

The decoder buffers at most `maximum_buffered_bytes`. It parses complete frames in physical order and returns at most `maximum_frames_per_push` per call. If that output limit is reached, remaining complete bytes stay buffered and can be drained by a later `push({})`. This provides caller-controlled backpressure without dropping or reordering logical frames.

A partial trailing frame remains buffered until more bytes arrive. `finish()` succeeds only when no bytes remain buffered. A truncated final frame therefore becomes an explicit `protocol` error rather than disappearing silently.

Malformed framing, digest mismatch, or a declared size outside configured limits is terminal for that decoder instance. No frame from the same `push()` call becomes caller-visible if a later complete frame in that call proves malformed. The caller may create a new decoder after handling the error; E.1 does not attempt magic-byte resynchronization because silent resync could discard evidence and belongs to a separately specified recovery policy.

After successful `finish()`, further `push()` calls fail with `invalid_argument`.

## Multiplexing semantics

Physical order and logical order are distinct:

```text
physical:  A#7  B#100  A#8  C#3  B#101
logical A: A#7  A#8
logical B: B#100 B#101
logical C: C#3
```

The decoder preserves the sequence/epoch/clock supplied in each frame exactly. It does not require sequences to start at zero, be contiguous, or increase. It does not compare clocks or epochs between frames. Existing CODEC continuity semantics remain a separate layer, allowing later E milestones to represent loss, reordering, recovery, or transport migration explicitly rather than having E.1 manufacture a continuity interpretation.

Connection epoch remains logical stream metadata. It is never derived from the lifetime of the `MultiplexDecoder` object or the physical byte-stream connection. This preserves the architectural rule that stream identity and epochs survive transport changes.

## Error mapping

- Invalid local limits or frame metadata: `invalid_argument`.
- Local encoder/decoder payload or buffering bounds: `resource_exhausted`.
- Malformed, truncated, unsupported, inconsistent, or digest-invalid wire frame: `protocol`.
- Memory allocation failure caught at this boundary: `resource_exhausted`.
- Unexpected implementation failure: `internal`.

No network error is produced because E.1 has no network I/O.

## Truth, archive, and compatibility effects

- **S0/S1/D:** unchanged. Multiplex framing does not classify truth.
- **CODA layout/version:** unchanged. `CMX1` is not a CODA record or archive revision.
- **Stream identity:** unchanged. Existing `StreamId` is carried verbatim.
- **Timing/continuity:** existing `StreamEpoch` and `StreamClock` are carried verbatim; no new continuity rule is introduced.
- **Generic processing APIs:** unchanged.
- **Audio Profile:** unchanged.
- **C ABI/CLI:** unchanged.

A future adapter may translate a decoded multiplex frame into preservation records, but E.1 itself performs no archive write and makes no S0 capture claim.

## Implementation structure

- `include/codec/transport.hpp` — public generic frame/limits/decoder API.
- `src/transport/multiplex.cpp` — little-endian frame encoder, integrity validation, incremental decoder.
- `tests/test_transport_multiplex.cpp` — deterministic framing/multiplex/backpressure/failure tests.
- `CMakeLists.txt` — compile and test registration only.
- `tests/package_consumer/main.cpp` — prove installed public transport API is usable without new dependencies.
- `README.md` and `CHANGELOG.md` — truthful Stage E.1 status and non-claims.

No external dependency is added; E.1 reuses CODEC's existing SHA-256 implementation.

## Proof contract

The dedicated E.1 test must first establish RED by referring to the absent transport API, then prove GREEN for:

1. exact single-frame round trip of every generic field and arbitrary binary payload;
2. interleaved frames from at least three logical streams preserving physical order and each stream's independent sequence/epoch/clock values;
3. arbitrary physical chunk boundaries, including one-byte pushes;
4. several frames in one physical input chunk;
5. `maximum_frames_per_push` backpressure with lossless draining through empty pushes;
6. empty payload support;
7. no implicit sequence-continuity enforcement or gap invention;
8. header and payload corruption detected by the frame digest;
9. bad magic/version/flags/header size, invalid timebase, inverted interval, inconsistent lengths, oversize declarations, and truncated `finish()` failing with the specified error classes;
10. buffer/payload limits enforced before unsafe growth;
11. no partial frame vector returned when a later complete frame in the same push is corrupt;
12. installed-package consumer can include and use `codec/transport.hpp` with no new link dependency;
13. all existing GCC, Clang, sanitizer, package-consumer, C ABI, CLI, Audio Profile, capability, and AI-contract gates remain green.

## Non-claims

E.1 does not provide TCP/UDP/QUIC/WebSocket transport, sockets, TLS, encryption, signatures, authorization, packet reordering, retransmission, gap inference, parity/FEC, erasure coding, loss recovery, resynchronization after corruption, congestion control, QoS/fair scheduling, priority, stream admission, archive persistence, distributed workers, object storage, performance/latency/scale evidence, CLI/C ABI transport commands, a frozen normative wire standard, or Stage E completion.

The next Stage E dependency after E.1 should be selected from explicit gap/loss observation and recovery-group semantics before choosing a concrete FEC implementation.
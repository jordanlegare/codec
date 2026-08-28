# Stage D.8 Audio Separation ModelBundle Design

## Work record

```yaml
task: Add a bounded deterministic Audio Profile ModelBundle contract for portable separation-model bytes before integrating a neural runtime.
base_ref: main
base_head_sha: 31b77e89cb06cfb149b933934e0d589e7d77369f
work_branch: automation/stage-d8-model-bundle
current_version: 0.1.0
active_roadmap_stage: D — Audio Stream Profile 1.0; compatible runtime/model bundles remain the next unmet dependency after D.7
continuity_evidence:
  git_head: main and origin/main both 31b77e89cb06cfb149b933934e0d589e7d77369f at start
  open_prs: none
  exact_head_ci: main CI 113 / 33172929465 succeeded
  roadmap_issue: exactly one open issue titled CODEC v1.0 roadmap execution log, issue 10; latest entry records D.7 complete
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: audio-profile
touched_truth_classes: []
current_behavior_verified_from: [cmake, public_headers, inference_backend, audio_offline_separation_tests, cli, changelog]
new_capability_claim: deterministic bounded AMB1 encode/decode with structural compatibility and SHA-256 model integrity; no model execution or neural availability
change_class: inference_or_derived_output
```

```text
BEFORE: D.7 accepts an opaque bounded model-bundle reference, while the default
        backend returns model_incompatible and CODEC has no bundle schema or
        integrity/compatibility loader.
AFTER:  The Audio Profile can deterministically encode and strictly verify one
        bounded AMB1 separation bundle containing a manifest and opaque ONNX
        model bytes; execution remains unavailable until a later runtime gate.
```

## Context

Stage D.7 proved the offline S1-to-D orchestration and provenance contract using
a caller-supplied backend. Its completion record identifies a compatible
runtime/model bundle as the next hard dependency and explicitly places streaming
after that gate.

The historical architecture described a versioned ModelBundle with task,
format, SHA-256, exact input geometry, framing, source count, causal behavior,
tensor layout, normalization, output semantics, license, and quality-domain
metadata. It separately ordered “Implement ModelBundle” before “Integrate ONNX
Runtime CPU.” D.8 follows that dependency order without reviving historical
claims that current code does not prove.

## Decision

Add one profile-scoped, self-contained binary bundle format named **AMB1**
(Audio Model Bundle version 1) and two public functions:

1. deterministic encoding from a validated separation manifest plus opaque ONNX
   bytes; and
2. strict bounded decoding that verifies canonical structure and the embedded
   model SHA-256 before returning owned verified bytes.

AMB1 is an in-memory representation. D.8 does not read paths, download models,
write archives, parse ONNX protobuf graphs, construct runtime sessions, or
execute inference. Those are separate security, compatibility, and runtime
boundaries.

## Alternatives considered

### JSON manifest in a filesystem directory

Rejected for D.8. The repository has no JSON dependency, and combining schema
work with path traversal, symlink, replacement, and multi-file snapshot rules
would enlarge the milestone without helping a later in-memory archive loader.

### Model metadata only, supplied as a C++ structure

Rejected. It would not establish a portable canonical byte contract, would not
bind the metadata to exact model bytes, and would leave archive or distribution
integrations to invent incompatible encodings.

### AMB1 plus ONNX Runtime CPU execution

Rejected for this milestone. Runtime dependency discovery, ONNX graph and I/O
validation, session construction, numerical conversion, cancellation, provider
selection, and model qualification form the next bounded gate. Combining those
with the first bundle parser would obscure both failure surfaces and overstate
neural availability.

## Public API

Add `include/codec/profiles/audio_model_bundle.hpp` in
`codec::profiles::audio`:

```cpp
struct SeparationModelManifest {
  std::string model_id;
  std::string model_version;
  std::string license_id;
  std::string quality_domain;
  std::uint32_t input_sample_rate{};
  std::uint16_t input_channels{};
  std::uint32_t window_samples{};
  std::uint32_t hop_samples{};
  std::uint32_t lookahead_samples{};
  std::uint16_t maximum_sources{};
  bool causal{};
  std::string input_tensor_name;
  std::string output_tensor_name;
};

struct SeparationModelBundle {
  SeparationModelManifest manifest;
  std::vector<std::byte> onnx_model;
};

struct VerifiedSeparationModelBundle {
  SeparationModelManifest manifest;
  std::vector<std::byte> onnx_model;
  Sha256 model_hash{};
  Sha256 bundle_hash{};
};

struct SeparationModelBundleLimits {
  std::uint64_t maximum_bundle_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_model_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint16_t maximum_text_bytes{1024};
};

Result<std::vector<std::byte>> encode_separation_model_bundle(
    const SeparationModelBundle& bundle,
    const SeparationModelBundleLimits& limits = {});

Result<VerifiedSeparationModelBundle> decode_separation_model_bundle(
    std::span<const std::byte> encoded,
    const SeparationModelBundleLimits& limits = {});
```

The Audio Profile facade includes this header. No root-level compatibility
alias, generic core type, C ABI, CLI surface, or legacy
`SeparationBackend` virtual method changes.

## AMB1 canonical encoding

All integers are unsigned big-endian. Fields appear exactly once in this order:

| Field | Encoding |
|---|---|
| magic | four bytes `AMB1` |
| flags | 16 bits; bit 0 is `causal`, all other bits must be zero |
| manifest length | 32 bits |
| model length | 64 bits |
| model SHA-256 | 32 bytes |
| sample rate | 32 bits |
| channels | 16 bits |
| maximum sources | 16 bits |
| window samples | 32 bits |
| hop samples | 32 bits |
| lookahead samples | 32 bits |
| six text fields | each a 16-bit byte length followed by bytes, in public-structure order |
| model | exact opaque bytes |

The six text fields are model ID, model version, license ID, quality domain,
input tensor name, and output tensor name. The manifest length covers the
numeric and text manifest body only. A decoder must consume exactly that body,
then exactly the declared model bytes, with no trailing bytes. This produces one
canonical encoding for every accepted public value.

The task and portable model format are fixed by AMB1 to audio separation and
ONNX. Version 1 also fixes:

- input tensor element type and layout to float32 `[batch, channel, sample]`;
- output tensor element type and layout to float32
  `[batch, source, channel, sample]`;
- PCM conversion to signed PCM16 divided by 32768.0; and
- output semantics to source waveforms, with the runtime responsible for the
  mandatory residual required by D.7.

Fixing these values avoids accepting metadata the next runtime cannot interpret.
A later incompatible representation requires a new magic/version, not ambiguous
optional fields.

## Validation and failure behavior

Both encode and decode validate their complete contracts before returning a
value:

- all limits are non-zero and the text limit is representable by AMB1;
- each text field is non-empty, no longer than the configured text limit, and
  consists of printable ASCII without NUL or control bytes;
- input sample rate and channel count are non-zero, with at most 64 channels;
- window and hop are non-zero, hop is no greater than window, and lookahead is
  no greater than window;
- causal bundles have zero lookahead;
- maximum sources is between 1 and the D.7 profile ceiling of 64;
- input and output tensor names differ;
- model bytes are non-empty and within the model limit;
- checked arithmetic proves the exact encoded size is within both `size_t` and
  the configured bundle limit before allocation;
- magic, flags, lengths, and total size are exact on decode; and
- the decoded model bytes hash to the embedded SHA-256.

Caller-provided invalid structures return `invalid_argument`. Exceeded caller
resource bounds return `resource_exhausted`. Malformed, noncanonical,
unsupported, truncated, trailing, or hash-mismatched encoded bundles return
`model_incompatible`. Errors expose no partial verified bundle.

The decoder checks the outer encoded-size limit before parsing and checks
declared lengths before copying model bytes. It never trusts a length field for
pointer arithmetic or allocation.

## Integrity and identity

The encoder computes the model SHA-256 from the exact supplied ONNX bytes and
stores it in AMB1. The decoder recomputes and compares that hash in constant
work per byte before returning `VerifiedSeparationModelBundle::model_hash`.
It also computes `bundle_hash` over the entire canonical AMB1 byte sequence so
a later archive, registry, or runtime can retain exact bundle identity.

These hashes prove byte identity and corruption detection only. D.8 does not
claim authorship, signature verification, license validity, model safety,
graph validity, runtime compatibility, or model quality.

## Truth and compatibility

- **S0/S1/D:** unchanged. A bundle is configuration/software metadata, not a
  captured source, canonical stream state, or derived stream artifact.
- **CODA layout:** unchanged; AMB1 is not persisted automatically and no record
  type is allocated.
- **Generic core:** unchanged; all model semantics remain in the Audio Profile.
- **Legacy inference ABI:** unchanged. D.7 continues to accept its bounded
  opaque reference and caller-supplied backend.
- **Capabilities:** `neural_separation:false` and `gpu_inference:false` remain
  unchanged. Successfully decoding AMB1 does not mean a runtime is installed.

## Proof contract

The dedicated D.8 test is added before implementation and must first fail to
compile because the new header/API does not exist. It then proves:

1. a known manifest and model encode to one exact AMB1 fixture;
2. decode returns byte-identical ONNX data, all manifest fields, the expected
   model hash, and a deterministic bundle hash;
3. encode-decode-encode is byte-identical;
4. every public manifest invariant is rejected;
5. zero/too-small resource limits fail with the specified error class;
6. wrong magic, unknown flags, forged lengths, truncation, trailing bytes,
   invalid text, and model-hash corruption fail closed with no verified value;
7. the Audio Profile facade exposes the additive API;
8. the default backend and CLI capabilities remain explicitly unavailable; and
9. Release, sanitizer, installed-package consumer, C ABI, CLI, archive, audio,
   generic processing, and existing D.7 coverage remain green.

```yaml
proof:
  regression_test: tests/test_audio_model_bundle.cpp
  exactness_test: exact AMB1 fixture plus byte-identical round trip
  compatibility_test: installed CMake package consumer compiles the profile API; legacy ABI and C ABI remain green
  failure_path_test: malformed/noncanonical/hash-mismatched input and resource bounds fail closed
  security_test: bounded lengths, checked arithmetic, no filesystem/network/archive access, no partial verified output
  benchmark: n/a; no performance or scale claim
```

## Non-claims and next dependency

D.8 does not implement or claim ONNX graph parsing, runtime/provider loading,
CPU or GPU inference, a bundled production model, signature/trust policy,
download, registry, filesystem or archive persistence, calibration or quality
qualification, streaming, latency/throughput/energy/scale, identity fusion,
recovery, distributed execution, a frozen ModelBundle schema, or completion of
Stage D.

The next dependency is a D.9 CPU reference runtime that accepts only a
`VerifiedSeparationModelBundle`, validates the actual ONNX graph/session and
declared tensor contract, converts bounded PCM deterministically, produces
stems plus the required residual, and keeps capability reporting unavailable
unless a compatible runtime and bundle are both active.

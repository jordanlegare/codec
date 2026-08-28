# Stage D.9 ONNX Runtime CPU Separation Backend Design

## Work record

```yaml
task: Add a bounded caller-activated ONNX Runtime CPU separation backend that consumes a verified D.8 ModelBundle and can drive D.7 offline inference.
base_ref: main
base_head_sha: 0fe1151cdfb4594b08b75cbd58c7e1d25dd6f775
work_branch: automation/stage-d9-onnx-cpu-runtime
current_version: 0.1.0
active_roadmap_stage: D — Audio Stream Profile 1.0; D.8 is integrated and actual CPU model/session execution is the next unmet dependency
continuity_evidence:
  git_head: main and origin/main both 0fe1151cdfb4594b08b75cbd58c7e1d25dd6f775 at start
  open_prs: none
  exact_head_ci: main CI 116 / 33200186931 succeeded
  roadmap_issue: exactly one open issue titled CODEC v1.0 roadmap execution log, issue 10; latest entry records D.8 complete and selects D.9
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: audio-profile, inference
touched_truth_classes: [S1, D]
current_behavior_verified_from: [cmake, public_headers, inference_backend, d7_tests, d8_tests, cli, changelog]
new_capability_claim: caller-activated bounded ONNX Runtime CPU session validation and offline PCM16 separation for a compatible verified AMB1 bundle; no bundled production model or default neural availability
change_class: inference_or_derived_output
```

```text
BEFORE: D.8 verifies canonical AMB1 structure and model-byte identity, but CODEC
        does not parse an ONNX graph, create a runtime session, or execute it.
AFTER:  A caller can bind a verified compatible AMB1 bundle to a dynamically
        loaded ONNX Runtime CPU library, validate the graph/session contract,
        and execute bounded windowed separation through the existing backend.
```

## Decision

Add one profile-only ONNX Runtime CPU backend factory. The factory accepts a
`VerifiedSeparationModelBundle`, revalidates its D.8 identities, dynamically
loads a caller-selected ONNX Runtime library, creates an in-memory session from
the exact verified bytes, and validates the graph I/O contract before returning
an available `SeparationBackend`.

The backend performs deterministic rectangular overlap/add over the manifest's
window and hop geometry. It converts signed PCM16 to float32 by division by
32768, runs only the CPU execution provider, converts source waveforms back to
PCM16 with defined rounding and saturation, and constructs the mandatory D.7
residual from the original mixture and returned stems.

CODEC does not distribute an ONNX Runtime binary or a production model. Build
support is discovered privately from `CODEC_ONNXRUNTIME_ROOT`; source builds
without compatible headers still expose the factory but return explicit
`model_incompatible`. GitHub CI and the milestone proof use the official pinned
ONNX Runtime 1.29.0 CPU release and its published SHA-256.

## Public API

Add `include/codec/profiles/audio_onnx_cpu_runtime.hpp`:

```cpp
struct OnnxCpuSeparationLimits {
  std::uint64_t maximum_input_frames{28'800'000};
  std::uint64_t maximum_output_samples{134'217'728};
  std::uint64_t maximum_windows{1'000'000};
};

struct OnnxCpuSeparationOptions {
  std::string runtime_library;
  std::uint32_t intra_op_threads{1};
  std::uint32_t inter_op_threads{1};
  OnnxCpuSeparationLimits limits{};
};

bool onnx_cpu_separation_runtime_compiled() noexcept;

Result<std::unique_ptr<SeparationBackend>>
create_onnx_cpu_separation_backend(
    const VerifiedSeparationModelBundle& bundle,
    const OnnxCpuSeparationOptions& options = {});
```

No ONNX Runtime type appears in installed headers. The existing legacy backend
ABI is unchanged. `default_separation_backend()` remains unavailable because no
model is bundled or selected by default.

`runtime_library` is an explicit caller-controlled path or loader name. An
empty value selects only the platform's ordinary ONNX Runtime library name;
the implementation does not inspect environment variables, search the network,
or download code.

## Factory validation

Before loading executable code, validate non-zero limits, thread counts in
`[1, 64]`, a runtime-library string no longer than 4096 bytes and containing no
NUL, and a non-empty verified model.

Re-encode the manifest and model with the D.8 encoder, then require both:

- SHA-256 of the exact model bytes equals `model_hash`; and
- SHA-256 of the canonical AMB1 bytes equals `bundle_hash`.

This prevents a caller from fabricating the public aggregate and passing
unverified bytes across the execution boundary.

Load `OrtGetApiBase`, require the API compiled from the discovered headers,
create one environment and sequential CPU session, set explicit intra/inter-op
thread counts, enable graph optimizations, and call the in-memory session API.
Session construction or graph incompatibility returns `model_incompatible` and
exposes no backend.

The accepted graph contract is exactly:

- one input and one output;
- manifest-declared, distinct tensor names;
- float32 input rank 3 `[batch, channel, sample]`;
- float32 output rank 4 `[batch, source, channel, sample]`;
- input batch dimension fixed to 1 or dynamic;
- input channel and sample dimensions equal the manifest values or are dynamic;
- output dimensions are fixed to batch 1, a source count between 1 and
  `maximum_sources`, the manifest channel count, and `window_samples`.

No extra graph input, initializer exposed as input, output, element type, rank,
or incompatible fixed dimension is accepted.

## Execution contract

Each request must contain valid complete interleaved PCM16, match the bundle's
sample rate and channel count, use the exact lowercase whole-bundle SHA-256 as
`model_bundle`, and provide `maximum_sources` in `[1, 64]`.

For a mixture with at least one frame, windows start at frame 0 and advance by
`hop_samples` while the start is below the input length. Each input window has
exactly `window_samples`; its tail is zero-padded. Interleaved PCM is transposed
to channel-major float32 and divided by 32768.0.

Every runtime output must be a CPU float32 tensor with exact runtime shape
`[1, source, channels, window_samples]`. Source count must remain stable across
windows, be non-zero, not exceed the manifest ceiling, and not exceed the
caller's `maximum_sources`. Non-finite values fail closed.

For every covered output sample, the runtime adds the value to a per-source
accumulator and increments a shared overlap count. Final source values divide
by that count, multiply by 32768, round half away from zero, and saturate to
signed PCM16. Because `hop_samples <= window_samples`, every input frame must
receive at least one contribution.

The residual is the original sample minus the sum of PCM16 stems, accumulated
widely and saturated to PCM16. The backend reports RMS sample-domain error of
`mixture - (stems + residual)`; D.7 independently recomputes its own metrics.

Preflight checked arithmetic enforces maximum input frames, maximum windows,
and one aggregate CODEC-owned output-sample budget covering all stems plus the
residual. It also rejects any required `size_t` or declared input/output tensor
element overflow before CODEC allocation. These limits do not sandbox the
authorized ONNX graph or impose a hard quota on ONNX Runtime's internal
intermediate allocations.

## Failure mapping

- Caller option/request defects: `invalid_argument`.
- Caller working/output bounds: `resource_exhausted`.
- Runtime unavailable, bad D.8 identity, malformed ONNX, or incompatible graph:
  `model_incompatible`.
- Session-run failure, malformed/non-finite runtime output, or inconsistent
  output across windows: `inference`.

No partial stems or backend escape a failed call. Runtime/session exceptions are
caught at the profile boundary and converted to bounded CODEC errors.

## Build and distribution

`CODEC_ONNXRUNTIME_ROOT` is a CMake cache path. When it contains the official
headers, CODEC compiles the real runtime implementation while still resolving
the shared library only at factory time. When absent, the same public factory
is a truthful unavailable stub. ONNX Runtime remains a private build detail and
is not a required link dependency of installed consumers.

CI downloads only the official `onnxruntime-linux-x64-1.29.0.tgz`, verifies
SHA-256 `c3fddc4f139a045b0c4902c57410f0694f1c2fdf9b6939fbe38b1aeae7cd14ba`,
and configures CODEC with its extracted root. The archive and binary are not
committed or installed by CODEC.

Leak-enabled sanitizer CI keeps `ASAN_OPTIONS=detect_leaks=1` globally. The
released ONNX Runtime 1.29.0 Linux CPU binary leaves small process-lifetime
allocations created specifically during dynamic loading; CI #118 observed
720 bytes in 18 allocations, each allocation stack passing through `dlopen()`
inside D.9 `DynamicLibrary::open()`, after all D.9 functional tests had passed.
To avoid masking CODEC leaks, sanitizer CTest isolates only
`audio_onnx_cpu_runtime_*` into `codec-onnx-runtime` and applies
`tests/lsan_onnxruntime.supp` with the single rule `leak:dlopen` to that process.
The ordinary `codec-unit`, C API, CLI integration, and AI-contract tests remain
unsuppressed. Allocations made later by CODEC or ONNX Runtime that do not have
`dlopen()` in their allocation stack remain visible to LeakSanitizer. This is a
CI/test-harness accommodation for third-party loader lifetime behavior; it does
not alter production ownership or runtime behavior.

## Truth and capability effects

- **S0:** unchanged.
- **S1:** read-only PCM16 input, still selected only through D.3 when D.7 is
  used.
- **D:** actual runtime-produced stems and mandatory residual flow through the
  existing D.7 derived-output/provenance contract.
- **CODA layout:** unchanged.
- **Generic core:** unchanged.
- **Capabilities:** `neural_separation:false` and `gpu_inference:false` remain
  unchanged because no production model/runtime selection is installed in the
  default backend.

`onnx_cpu_separation_runtime_compiled()` reports only whether this binary was
built with compatible headers. It does not prove that a library can be loaded,
a model is compatible, inference quality is acceptable, or a default neural
capability is available.

## Proof contract

The dedicated D.9 test must first fail to compile before the API exists, then
use a real pinned ONNX Runtime CPU library and a minimal in-memory ONNX graph to
prove:

1. a valid D.8 bundle creates an available backend only after real session and
   I/O validation;
2. overlap/add inference returns exact identity-model PCM16, a zero residual,
   the exact model hash, and versioned CPU provider identity;
3. the backend drives an actual D.7 verified-S1-to-D orchestration without
   mutating its archive;
4. fabricated bundle identities, malformed ONNX, wrong names/types/ranks/fixed
   geometry, extra graph I/O, and missing runtime libraries fail closed;
5. request bundle-reference, geometry, source, frame, window, and aggregate
   output bounds are enforced before unsafe allocation or execution;
6. installed consumers see the additive factory without ONNX types or a new
   link-time ONNX dependency; and
7. all existing Release, sanitizer, capability, package, and unavailable-default
   tests remain green. Sanitizer proof must keep global leak detection enabled,
   isolate the ONNX runtime tests, and permit only the documented `dlopen()`
   loader-lifetime suppression in that isolated process.

## Non-claims

D.9 does not provide or claim a production model, separation quality,
perceptual transparency, trained source taxonomy, signatures/trust, licensing
validity, model download/update, GPU/provider selection, cancellation, live
streaming, latency/throughput/scale, automatic CODA persistence, identity
fusion/diarization, CLI or C ABI inference, a default neural capability, frozen
CODA v1, or completion of Stage D. It does not sandbox a caller-authorized
model or hard-limit ONNX Runtime's internal graph allocations.

The next dependency after D.9 is a separately bounded streaming/runtime
orchestration gate; production model qualification remains independent.

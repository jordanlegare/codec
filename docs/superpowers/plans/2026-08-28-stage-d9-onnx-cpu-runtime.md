# Stage D.9 ONNX Runtime CPU Separation Backend Implementation Plan

**Goal:** Add a bounded caller-activated CPU backend that executes a compatible
verified D.8 AMB1 model through a real dynamically loaded ONNX Runtime session
and plugs into the existing D.7 offline derived-output workflow.

**Base:** `0fe1151cdfb4594b08b75cbd58c7e1d25dd6f775`

**Branch:** `automation/stage-d9-onnx-cpu-runtime`

**Spec:** `docs/superpowers/specs/2026-08-28-stage-d9-onnx-cpu-runtime-design.md`

## Constraints

- Keep every tensor, PCM, model, and runtime assumption in the Audio Profile.
- Do not expose ONNX Runtime types or add a mandatory consumer link dependency.
- Revalidate both D.8 identities before parsing or executing model bytes.
- Use only CPU execution, checked bounded allocation, and no network/runtime
  discovery beyond the caller path or ordinary platform loader name.
- Preserve the unavailable default backend and false neural/GPU capabilities.
- Use ONNX Runtime 1.29.0 and its published archive digest for milestone proof.
- Use build parallelism 4 in this managed environment.

## Task 1: Establish the RED runtime contract

**Create:** `tests/test_audio_onnx_cpu_runtime.cpp`

**Modify:** `CMakeLists.txt`

1. Add the dedicated test file to `codec_tests` before the API exists.
2. Add compact protobuf-writing helpers that produce an in-memory ONNX
   Unsqueeze identity model with input `[1, 1, 4]` and output `[1, 1, 1, 4]`.
3. Define tests for compiled runtime truth, valid session creation, exact
   overlap/add inference over six mono frames, exact model identity, provider
   identity, zero residual, and D.7 integration.
4. Define failure tables for bad bundle identities, malformed models, graph
   name/shape mismatch, missing library, request geometry/reference defects,
   and resource limits.
5. Configure with the pinned extracted ONNX Runtime root and build. Accept RED
   only when existing production code compiles and the new public D.9 types and
   factory are missing.
6. Commit the proof as `test: define Stage D.9 ONNX CPU runtime contract`.

## Task 2: Implement optional build discovery and session validation

**Create:**

- `include/codec/profiles/audio_onnx_cpu_runtime.hpp`
- `src/audio/onnx_cpu_separation.cpp`

**Modify:**

- `include/codec/profiles/audio.hpp`
- `CMakeLists.txt`

1. Add the exact profile-only public options, limits, compiled-status query,
   and factory from the design.
2. Discover `onnxruntime_cxx_api.h` privately beneath
   `CODEC_ONNXRUNTIME_ROOT`. Define `CODEC_HAS_ONNXRUNTIME` only for
   `codec_core` when found; otherwise compile the explicit unavailable branch.
3. Implement cross-platform shared-library ownership and symbol loading without
   exposing a vendor type in installed headers.
4. Re-encode and hash-check the supplied verified bundle before loading the
   runtime.
5. Initialize the manual ONNX Runtime C++ API, create a sequential optimized CPU
   session from the in-memory bytes, and validate exact graph names, counts,
   element types, ranks, and compatible fixed/dynamic dimensions.
6. Convert all runtime exceptions/status failures to bounded CODEC errors and
   return no partial backend.
7. Commit as `feat: validate ONNX CPU separation sessions`.

## Task 3: Implement bounded windowed execution

**Modify:**

- `src/audio/onnx_cpu_separation.cpp`
- `tests/test_audio_onnx_cpu_runtime.cpp`

1. Validate request PCM, exact bundle reference, geometry, source bound, and
   all configured resource ceilings before execution.
2. Preflight window count, tensor sizes, accumulation buffers, and aggregate
   output samples with checked arithmetic.
3. Normalize and transpose each zero-padded input window, execute the session,
   and validate the exact CPU float output tensor on every call.
4. Apply deterministic rectangular overlap/add, reject non-finite output,
   convert with half-away rounding and saturation, and create the saturated
   mandatory residual.
5. Compute the backend RMS reconstruction metric and return exact model/provider
   identity.
6. Run the direct and CTest suites using the real runtime, then commit as
   `feat: execute bounded ONNX CPU separation`.

## Task 4: Prove packaging, CI, and truthful status

**Modify:**

- `.github/workflows/ci.yml`
- `tests/package_consumer/main.cpp`
- `README.md`
- `CHANGELOG.md`

1. Download the official Linux x64 ONNX Runtime 1.29.0 archive in every Ubuntu
   CI job, verify its exact SHA-256, extract under the runner temporary
   directory, and configure CODEC with that root.
2. Exercise the installed D.9 declarations and compiled-status query from the
   package consumer without loading a runtime or exposing vendor headers.
3. Record the narrow real CPU session/execution claim while keeping the default
   backend and neural/GPU capabilities unavailable and listing all non-claims.
4. Run fresh Release configure/build, direct tests, CTest, capabilities,
   install, and clean downstream consumer proof on one exact committed head.
5. Run fresh ASan/UBSan with leak detection enabled. If the official released
   ONNX Runtime binary reports process-lifetime allocations made specifically
   during `dlopen()`, isolate `audio_onnx_cpu_runtime_*` into its own sanitizer
   CTest process and apply only a `leak:dlopen` LSan suppression to that process.
   Keep every other CODEC test unsuppressed and never disable `detect_leaks`
   globally.
6. Audit the full diff, generated files, API/profile boundary, claims, and exact
   tree; commit as `docs: record Stage D.9 ONNX CPU runtime`.

## Task 5: Exact-head publication and integration

1. Publish the RED tree and require a remote RED failure at the missing D.9
   contract.
2. Publish the exact fully verified green tree without force, open one D.9 PR,
   and require GCC, Clang, package/install, and sanitizer success on that exact
   head. Any LSan suppression must satisfy Task 4's isolated-loader rule.
3. Recheck base/head/tree, reviews, mergeability, claims, and all worksheet merge
   gates after CI.
4. Squash-merge only with the exact expected head SHA.
5. Verify the signed published `main` tree equals the locally tested tree and
   require push-triggered main CI to pass.
6. Fast-forward local `main` and add the complete D.9 evidence record to roadmap
   issue #10. Stage D remains active.

## Sanitizer investigation record

- CI #118 / `33202567559` passed GCC, Clang, package install, and the external
  consumer, but LeakSanitizer failed `codec-unit` after all functional tests
  passed. It reported 720 bytes in 18 allocations, and every reported
  allocation stack passed through `dlopen()` in D.9 `DynamicLibrary::open()`.
- D.9 production ownership already releases session, session options,
  environment, and the dynamic-library handle. The observed allocation shape
  matches ONNX Runtime process-lifetime initialization reports rather than a
  growing CODEC allocation.
- The test harness was therefore split only for sanitizer builds: ordinary
  CODEC unit tests exclude `audio_onnx_cpu_runtime_*`, while
  `codec-onnx-runtime` runs exactly that group with `leak:dlopen` from
  `tests/lsan_onnxruntime.supp`. Global `ASAN_OPTIONS=detect_leaks=1` remains
  active.
- CI #121 / `33203341083` on `95ec16a62d97a54016b3ca82c782ad1b3ceaff8f`
  passed GCC, Clang, package/install, and sanitizer jobs. Sanitizer CTest ran
  5/5 tests, including both unsuppressed `codec-unit` and isolated
  `codec-onnx-runtime`.

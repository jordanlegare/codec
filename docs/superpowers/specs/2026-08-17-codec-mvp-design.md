# CODEC Executable MVP Design

## Authority and scope

The root `README.md` is the normative product specification. This document fixes the executable scope of the first implementation so the repository moves from a design-only state to a useful, verifiable product without weakening any capability boundary in the specification.

The MVP delivers deterministic CODA preservation and recovery, file and HTTP(S) byte capture, PCM16 WAV handling, reference W1/W2 acoustic carriers, Ed25519 COSE_Sign1 W0 statements, live-style identity events, feed extraction, C++ and C APIs, a CLI, tests, installation, and CI. It also exposes a stable separation-backend interface. No bundled neural weights are invented; when no compatible backend is installed, the API returns `model_incompatible` and capability output reports separation as unavailable.

## Architecture

`codec_core` is a dependency-light C++20 library. The archive module owns a distinctly versioned provisional CODA development-profile envelope, SHA-256 chain, committed-record trailer, scanner, verifier, repair, and feed index. It does not claim wire compatibility with the still-unfrozen normative CODA v1 schema. The capture module accepts local files, stdin, and HTTP(S) through libcurl while streaming accepted bytes directly to the archive writer. The audio module parses and writes integer PCM16 WAV without altering samples.

The watermark module creates and validates a compact canonical-CBOR COSE_Sign1 statement using Ed25519 through OpenSSL. The reference carrier uses low-amplitude framed binary FSK: W1 uses bins below 20 kHz and W2 uses bins above 24 kHz only when the sample rate and guarded Nyquist test pass. Correlation dominance is the detection statistic; no waveform impulse is inserted. Three matching hops plus a valid current W0 yield only a `signature_bound_candidate`; this stateless detector never emits authoritative `verified_feed` without the later replay-safe fusion layer.

The CLI composes these modules. Public headers expose no curl or OpenSSL types. A narrow versioned C ABI catches all exceptions and returns stable status codes.

## Data flow

1. `codec record` validates every feed and opens one CODA writer.
2. A feed descriptor record binds a stable stream ID to label and URI.
3. Accepted input bytes are appended in bounded S0 records before optional analysis.
4. WAV inputs can be watermarked to a separate D-class file; the original bytes remain untouched.
5. `codec watermark detect` decodes correlation candidates and optionally validates a COSE statement.
6. Identity events are emitted as JSON Lines and can also be archived.
7. `codec verify`, `list`, `extract`, and `repair` scan only committed records and validate the chain.

## Failure and safety behavior

- A record is visible only when its envelope, payload, and commit trailer are complete and valid.
- Truncation discards only the incomplete tail. Repair never modifies its source.
- Unknown compatible record types are skipped but remain chain-verified.
- HTTP errors, size limits, malformed WAV, failed signatures, expired statements, ambiguous detections, and unqualified W2 paths have distinct errors.
- Private keys are read only for issuance and are never written to CODA.
- W1 is a reference carrier, not a claim of perceptual transparency. W2 is experimental and gated.
- Neural output is never labeled exact. The original mixture and residual contract are mandatory for any future separation backend.

## Verification

Unit tests cover SHA-256, archive byte round-trip, chain tamper detection, interrupted-tail recovery, WAV sample round-trip, W1 decode, W2 gating and decode, COSE valid/invalid/expired statements, and C ABI ownership. Integration tests exercise record/inspect/verify/list/extract/repair and watermark CLI workflows. GitHub Actions builds with GCC and Clang, runs CTest, and performs an AddressSanitizer/UndefinedBehaviorSanitizer pass.

## Explicitly deferred capability

FFmpeg demux/decode, FLAC S1 compression, HLS-specific timeline handling, ONNX Runtime model execution, trained neural watermark localization, identity-conditioned neural separation, diarization/enrollment, GPU providers, and 24-hour qualification remain later README delivery phases. Their API boundaries and truthful capability reporting ship now; the MVP does not simulate them.

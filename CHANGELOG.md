# Changelog

All notable changes use semantic versioning.

## Unreleased

- Add generic CLI stream listing and exact S0 extraction by canonical stable `StreamId`, while preserving compatibility `list feeds` and `extract --feed` behavior without a CODA format, C ABI, or recording-format change.
- Add typed, stable-identity C++ generic stream recording through the existing hardened URI capture path, preserving exact caller-supplied `StreamDescriptor` metadata and S0 bytes while retaining legacy `FeedSpec`, CLI, C ABI, descriptor layout, and ordering behavior.
- Add payload-agnostic C++ `StreamAdapter` and `StreamProcessor` contracts with owned pull-based S0 records, exact extracted input batches, S1/D process identity, deterministic provider-output validation, and caller-supplied resource bounds without adding a registry, scheduler, persistence transaction, C ABI, CLI, or format change.
- Add direct S1/D provenance queries with AND-combined truth-class, physical subject, and existential immediate-input filters, archive-order results, exact record links, verified-prefix support, and pre-scan argument validation without recursive graph traversal or a format change.
- Add generic C++ physical-record query and boundary-preserving extraction APIs with exact stream/raw-type filters, half-open archive-sequence and envelope-time ranges, verified-prefix support, and per-record payload/hash retention without changing CODA bytes, the CLI, or C ABI.
- Add versioned declared S1/D provenance sidecars with exact backward-only subject/input record links, bounded generic process identity, typed opaque profile details, deterministic writer/reader validation, failure isolation, and repair coverage without changing the CODA development-profile envelope or C ABI.
- Add versioned generic S0 timing and gap persistence with C++ append/read APIs, deterministic per-stream sequence/epoch/clock validation, explicit missing ranges, exact prior source-record SHA-256 links, repair coverage, and malformed-metadata isolation without changing the CODA development-profile envelope or C ABI.
- Add versioned, payload-type-agnostic `StreamDescriptor` persistence and C++ append/list APIs, including legacy `FeedInfo` projection and failure-isolation coverage without changing the CODA development-profile header, record envelope, or C ABI.
- Add explicit C++ append/extract APIs for unknown 16-bit CODA development-profile record type codes, with byte-exact S0 and repair round-trip coverage and no binary-layout change.
- Add generic C++ stream metadata primitives (`StreamId`, `StreamType`, `TruthClass`, `StreamDescriptor`, `StreamClock`, and `StreamEpoch`) while retaining v0.1 `Feed*` compatibility and the existing C ABI/archive behavior.
- Add a repository-wide agent bootstrap, cold-start state recovery contract, aligned contribution guidance, and CI coverage for AI-control-file and version drift.

## 0.1.0 — 2026-08-17

- Add a distinctly versioned provisional CODA development profile with an append-only archive, SHA-256 record chain, committed-record trailers, exact S0 extraction, verification, and non-mutating repair. It is not the frozen normative CODA v1 schema.
- Add bounded file, stdin, HTTP, and HTTPS capture through the C++ engine, with pre-opened no-follow local descriptors and proxy-free globally-routable HTTP enforcement under the default private-network policy.
- Add integer PCM16 WAV support and reference W1/W2 binary-FSK embedding and Goertzel detection.
- Add canonical-CBOR W0 feed statements in COSE_Sign1 with Ed25519 signing and verification, including canonical UTF-8 validation before signing.
- Add a deterministic internal SHA-256 implementation so integrity digests cannot silently degrade when a crypto provider is unavailable.
- Add live-style JSON Lines candidates; three matching hops plus a current valid W0 produce a non-authoritative `signature_bound_candidate`. Replay-safe `verified_feed` remains gated on stateful fusion.
- Add C++20, versioned C, and CLI interfaces; examples; CMake install packages; tests; and CI.
- Add an explicit unavailable neural separation backend instead of claiming inference without compatible model weights.

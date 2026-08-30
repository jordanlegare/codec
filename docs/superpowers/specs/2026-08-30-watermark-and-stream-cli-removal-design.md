# Watermark and Generic-Stream CLI Removal Design

Date: 2026-08-30
Repository: `jordanlegare/codec`
Status: Approved design; implementation pending
Base: `a73b15c474b6974bf74b2c622b81bdcafee7890c`

## Purpose

Remove CODEC's watermark feature completely and narrow the command-line interface so users can list and extract only legacy named feeds. Retain the generic stream substrate as an installed C++ capability because processing, provenance, transport, recovery, distributed execution, Audio Profile ingest/export, and archive compatibility depend on it.

This specification supersedes the watermark-retention statements in the 2026-08-18 generalized-direction design. The user explicitly selected full watermark removal with CLI-only removal of generic stream listing and extraction.

## Required outcome

After the change:

- no watermark command, public header, public type, implementation source, test target, capability field, or Audio Profile re-export remains;
- `codec list` accepts only `feeds`;
- `codec extract` accepts only `--feed LABEL`, defaults to source-exact fidelity, and retains the existing `--follow` behavior;
- generic `StreamId`, descriptors, recording, archive queries, stream extraction, verified-prefix following, processing, transport, recovery, and distributed APIs remain available to C++ callers;
- every unrelated CLI command retains its existing implementation and observable behavior;
- retired numeric identifiers are not reused.

No new runtime capability, archive format, truth-class semantic, transport behavior, or release-version change is introduced.

## Alternatives considered

### Compatibility-tombstone removal — selected

Delete executable and public watermark functionality while preserving gaps for its historical numeric identifiers. Old archives remain structurally usable through unknown-record support, and retained distributed errors keep their current wire numbers.

### Unavailable stubs — rejected

Keeping deprecated watermark headers and functions that always return an unavailable error would reduce source breakage, but it would leave a public watermark feature and contradict the requested removal.

### Purge and renumber — rejected

Closing enum and wire-code gaps would create unrelated archive, C ABI, C++ error-value, and distributed-wire incompatibilities without improving the requested CLI simplification.

## CLI contract

### Removed watermark command family

Remove watermark help text, parsing helpers, command functions, and top-level dispatch. Any invocation beginning with `codec watermark` follows the existing unknown-command path, prints the normal usage text to standard error, and exits with status 2. It must not create a key, statement, WAV, or other output.

### Feed-only listing

`codec list feeds ARCHIVE` remains byte-for-byte compatible in behavior and JSON Lines shape, including its existing `stream_id` field. `codec list streams ARCHIVE` is rejected before opening the archive, exits with status 2, and reports that `list` supports only `feeds`.

The generic `CodaArchive::streams()` library API and descriptor projection remain unchanged.

### Feed-only extraction

The supported form is:

```text
codec extract ARCHIVE --feed LABEL [--fidelity source-exact] [--follow] --output FILE
```

Source-exact is the default when `--fidelity` is omitted. The explicit `--fidelity source-exact` spelling remains accepted for existing scripts; every other fidelity value remains invalid. Successful JSON output continues reporting `"fidelity":"source_exact"` regardless of whether the option was explicit.

Remove UUID parsing and every `--stream` selection branch from the CLI. An invocation containing `--stream`, whether alone or alongside `--feed`, is rejected during argument validation with status 2 and before the output path is opened. Existing regular and live-follow extraction by feed remain unchanged, work without a fidelity option, and may continue resolving the feed descriptor to an internal `StreamId`.

The generic `CodaArchive::extract_stream()`, raw extraction, record query, and `extract_stream_source_exact_prefix()` library APIs remain unchanged.

### Other CLI commands

Do not modify the command bodies for `record`, `verify`, `inspect`, or `repair`. Do not change the successful output of `list feeds` or `extract --feed`. Top-level usage and dispatch change only to remove retired surfaces.

`codec capabilities` removes `w0_ed25519`, `w1_reference`, `w2_reference`, and `w2_policy` rather than reporting false values. All retained fields keep their names, values, and ordering.

## Watermark library removal

Delete:

- `include/codec/watermark.hpp`;
- `include/codec/statement.hpp`;
- `src/watermark/carrier.cpp`;
- `src/watermark/statement.cpp`;
- `tests/test_watermark.cpp`;
- `tests/test_statement.cpp`.

Remove their CMake source/test entries and remove all watermark/statement includes, aliases, static assertions, and facade checks from `codec/profiles/audio.hpp` and Audio Profile tests. Remove watermark-specific capability members from `codec::Capabilities`.

OpenSSL remains a required dependency because CODA archive creation uses `RAND_bytes()` independently of watermark signing.

## Compatibility tombstones

### Archive record codes

Remove `RecordType::watermark_statement` and `RecordType::watermark_observation` plus their display-name cases. Do not assign codes 20 or 21 to another registered type.

CODA already treats the physical 16-bit record tag as opaque. Archives containing codes 20 or 21 must continue to verify, repair, query, preserve, and raw-extract those records as unknown compatible types. CODEC no longer interprets their payloads.

### C++ error values and distributed wire

Remove the five watermark-specific `ErrorCode` enumerators and their name/status mappings. Give every retained enumerator an explicit numeric value matching its value before removal. In particular, identity, cancellation, resource and internal errors retain values 15-19.

Distributed reply wire slots 10-14 become reserved and are never emitted. A decoder receiving one of those retired slots rejects it as an unknown remote error with the existing protocol-error path. Wire slots 1-9 and 15-19 retain their exact mappings and canonical round trips.

### C ABI status

Remove `CODEC_STATUS_WATERMARK`. Other explicitly assigned C status values remain unchanged, including `CODEC_STATUS_INTERNAL = 255`. No retained C API operation previously returned the watermark-only status once the watermark feature is absent.

## Documentation contract

Update current documentation and control files to describe the smaller feature set:

- README overview, CLI table/reference/workflows, capabilities, metrics, Audio Profile list, security boundaries and dependencies;
- `AGENTS.md` and `CONTRIBUTING.md` profile-boundary language;
- `AI_WORKSHEET.md` active work/proof record;
- `CHANGELOG.md` under Unreleased.

Do not rewrite historical released changelog entries. Keep the 2026-08-18 design as historical context and link this specification as the explicit superseding removal decision.

## Proof-first test design

Before production removal, change tests so the current tree fails the new contract:

1. CLI integration requires `watermark`, `list streams`, and `extract --stream` to exit 2.
2. The negative commands must not create requested outputs or mutate a sentinel output.
3. Help and capabilities output must omit every retired surface and field.
4. Regular and live-follow feed extraction must succeed without `--fidelity` and report `"fidelity":"source_exact"`; the explicit source-exact spelling remains covered for compatibility.
5. Existing record, verify, inspect, list-feeds, repair, version and retained-capability checks remain green.
6. Archive tests write raw type codes 20 and 21 and prove verification, repair and byte-exact raw extraction.
7. Distributed-wire tests prove all retained errors round-trip at their old numbers and retired slots 10-14 fail as protocol errors.
8. Repository contract tests prove the removed public headers and build sources are absent and current README/CMake/CLI claims agree.
9. Installed-package consumers compile and run without watermark headers or facade aliases.

Run the repository-mandated strict Release build/test suite, sanitizer build/test suite, CLI capability sanity check, and final targeted negative commands on the exact resulting head.

## Scope controls

- Do not remove or weaken generic stream library APIs.
- Do not change CODA record envelopes, version numbers, hashing, commit chains, S0/S1/D meanings, or unknown-type preservation.
- Do not refactor unrelated CLI command bodies.
- Do not change capture authorization, HTTP policy, repair, inference, Audio Profile ingest/export/separation, transport, recovery, or distributed behavior.
- Do not introduce replacement identity, signing, or watermark functionality.
- Do not bump the project version as part of this implementation; record the breaking removal under Unreleased for the next release decision.

## Acceptance criteria

The design is satisfied when all retired runtime and public surfaces are absent, the three removed CLI paths fail safely, regular and follow feed extraction default to source-exact without requiring the option, retained CLI workflows remain behaviorally intact, generic stream C++ functionality remains tested, compatibility tombstones prevent numeric reassignment, old raw records remain preservable, documentation matches runtime truth, and every mandatory verification gate passes on the exact final head.

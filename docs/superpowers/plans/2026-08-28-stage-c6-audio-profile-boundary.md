# Stage C.6 Audio Stream Profile Public Boundary

## Goal

Add an explicit compatibility-preserving C++ Audio Stream Profile facade for the already-implemented WAV/PCM, watermark, and source-separation APIs.

## Boundary

- New canonical additive include: `codec/profiles/audio.hpp`.
- New namespace: `codec::profiles::audio`.
- The facade aliases existing ABI-bearing types and imports existing functions; it does not move or duplicate implementations.
- Existing `codec::WavPcm16`, watermark APIs, separation APIs, legacy headers, CLI, and C ABI remain unchanged.
- No CODA/archive format or truth-class behavior changes.

## TDD proof

1. Add a test that includes `codec/profiles/audio.hpp`, proves profile/root type identity, and invokes the existing watermark-name and default separation backend through the profile namespace.
2. Verify RED because the profile header does not exist.
3. Add the minimal facade header.
4. Verify full GCC, Clang, sanitizer, package/install, CLI, C ABI, and AI-contract CI.

## Documentation

Update README and CHANGELOG narrowly to identify `codec::profiles::audio` as the explicit Audio Stream Profile public facade while documenting root-level names as compatibility surfaces.

## Non-claims

No Stage D audio feature work, ABI symbol migration, deprecation removal, new model/runtime, new watermark algorithm, archive change, C ABI migration, or CLI migration.

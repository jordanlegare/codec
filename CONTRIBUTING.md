# Contributing to CODEC

CODEC is a preservation-first, stream-first system. Changes must keep exact accepted S0, exact profile-defined S1, and provenance-bearing derived D output distinct. Generic core behavior must remain payload-type agnostic; audio decoding, separation, and feed identity belong to the Audio Stream Profile.

Before editing, follow the repository-wide bootstrap in [`AGENTS.md`](AGENTS.md), read the architectural manifest in [`README.md`](README.md), and complete the work record and proof contract in [`AI_WORKSHEET.md`](AI_WORKSHEET.md). Recover current state from the exact checked-out SHA, open work, and current CI rather than relying on a previous conversation.

## Development

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For memory and undefined-behavior checks, configure a second build with `-DCODEC_ENABLE_SANITIZERS=ON`.

Every behavioral change needs a test that fails before the implementation is changed. Keep public headers free of vendor and profile-only types, keep secrets outside fixtures and archives, and document any proven capability or format change in `CHANGELOG.md` and `README.md`. Merge only the exact head SHA whose applicable worksheet gates and required CI are green.

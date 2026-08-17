# Contributing to CODEC

CODEC is preservation-first. Changes must keep exact source/decoded data separate from derived inference, preserve identity uncertainty, and never promote a watermark candidate to `verified_feed` without a valid W0 signature.

## Development

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For memory and undefined-behavior checks, configure a second build with `-DCODEC_ENABLE_SANITIZERS=ON`.

Every behavioral change needs a test that fails before the implementation is changed. Keep public headers free of vendor types, keep secrets outside fixtures and archives, and document any capability or format change in `CHANGELOG.md` and `README.md`.

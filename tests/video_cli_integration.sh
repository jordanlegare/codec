#!/usr/bin/env bash
set -euo pipefail

codec_bin=${1:?codec binary path required}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test_dir=$(mktemp -d)
cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fixture="$test_dir/fixture.mp4"
base64 --decode "$script_dir/fixtures/video_4x4_h264.mp4.b64" > "$fixture"

"$codec_bin" --help > "$test_dir/help.txt"
grep -Fq 'codec video ingest' "$test_dir/help.txt"

set +e
"$codec_bin" video ingest --source "$fixture" \
  > "$test_dir/missing.stdout" 2> "$test_dir/missing.stderr"
missing_status=$?
set -e
if [ "$missing_status" -ne 2 ]; then
  echo "video ingest missing-argument case should exit 2, got $missing_status" >&2
  exit 1
fi
[ ! -e "$test_dir/missing.coda" ]

probe_archive="$test_dir/probe.coda"
set +e
"$codec_bin" video ingest \
  --source "$fixture" \
  --archive "$probe_archive" \
  --label camera \
  --start-ns 1000000000 \
  --end-ns 2000000000 \
  --layout yuv420p8 \
  > "$test_dir/probe.stdout" 2> "$test_dir/probe.stderr"
probe_status=$?
set -e

if [ "$probe_status" -eq 1 ] && grep -q 'model_incompatible' "$test_dir/probe.stderr"; then
  [ ! -e "$probe_archive" ]
  exit 0
fi
if [ "$probe_status" -ne 0 ]; then
  echo "video ingest probe failed unexpectedly with status $probe_status" >&2
  cat "$test_dir/probe.stderr" >&2 || true
  exit 1
fi

grep -q '"state_exact":true' "$test_dir/probe.stdout"
grep -q '"frames":1' "$test_dir/probe.stdout"
grep -q '"provenance":1' "$test_dir/probe.stdout"
grep -q '"layout":"yuv420p8"' "$test_dir/probe.stdout"
grep -q '"stream_id":"' "$test_dir/probe.stdout"
"$codec_bin" verify "$probe_archive" --level full > "$test_dir/probe-verify.json"
grep -q '"ok":true' "$test_dir/probe-verify.json"
if LC_ALL=C grep -aFq "$fixture" "$probe_archive"; then
  echo "video ingest persisted the raw source URI/path in archive metadata" >&2
  exit 1
fi

for layout in gray8 rgb24 rgba32; do
  archive="$test_dir/$layout.coda"
  "$codec_bin" video ingest \
    --source "$fixture" \
    --archive "$archive" \
    --label "camera-$layout" \
    --start-ns 1000000000 \
    --end-ns 2000000000 \
    --layout "$layout" \
    --maximum-source-bytes 1048576 \
    --maximum-decoded-bytes 1048576 \
    --maximum-frames 4 \
    > "$test_dir/$layout.json"
  grep -q '"state_exact":true' "$test_dir/$layout.json"
  grep -q '"frames":1' "$test_dir/$layout.json"
  grep -q "\"layout\":\"$layout\"" "$test_dir/$layout.json"
  "$codec_bin" verify "$archive" --level full > "$test_dir/$layout-verify.json"
  grep -q '"ok":true' "$test_dir/$layout-verify.json"
done

printf 'not media\n' > "$test_dir/malformed.bin"
malformed_archive="$test_dir/malformed.coda"
set +e
"$codec_bin" video ingest \
  --source "$test_dir/malformed.bin" \
  --archive "$malformed_archive" \
  --label malformed \
  --start-ns 0 \
  --end-ns 1000000000 \
  --layout gray8 \
  > "$test_dir/malformed.stdout" 2> "$test_dir/malformed.stderr"
malformed_status=$?
set -e
if [ "$malformed_status" -ne 1 ]; then
  echo "malformed video should leave source-only archive and exit 1, got $malformed_status" >&2
  exit 1
fi
grep -q '"state_exact":false' "$test_dir/malformed.stdout"
grep -q '"frames":0' "$test_dir/malformed.stdout"
grep -q '"profile_error":"decode"' "$test_dir/malformed.stdout"
"$codec_bin" verify "$malformed_archive" --level full > "$test_dir/malformed-verify.json"
grep -q '"ok":true' "$test_dir/malformed-verify.json"

invalid_archive="$test_dir/invalid-layout.coda"
set +e
"$codec_bin" video ingest \
  --source "$fixture" \
  --archive "$invalid_archive" \
  --label invalid-layout \
  --start-ns 0 \
  --end-ns 1000000000 \
  --layout nv12 \
  > "$test_dir/invalid-layout.stdout" 2> "$test_dir/invalid-layout.stderr"
invalid_status=$?
set -e
if [ "$invalid_status" -ne 2 ]; then
  echo "invalid layout should exit 2, got $invalid_status" >&2
  exit 1
fi
[ ! -e "$invalid_archive" ]

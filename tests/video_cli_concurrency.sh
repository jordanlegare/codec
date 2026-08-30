#!/usr/bin/env bash
set -euo pipefail

codec_bin=${1:?codec binary path required}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test_dir=$(mktemp -d)
writer_a_pid=
writer_b_pid=
cleanup() {
  for pid in "$writer_a_pid" "$writer_b_pid"; do
    if [ -n "$pid" ]; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  rm -rf "$test_dir"
}
trap cleanup EXIT

fixture="$test_dir/fixture.mp4"
base64 --decode "$script_dir/fixtures/video_4x4_h264.mp4.b64" > "$fixture"

# FFmpeg-disabled builds are covered by the main video integration test and
# cannot exercise worker concurrency.
probe_archive="$test_dir/probe.coda"
set +e
"$codec_bin" video ingest \
  --source "$fixture" \
  --archive "$probe_archive" \
  --label concurrency-probe \
  --start-ns 0 \
  --end-ns 1000000000 \
  > "$test_dir/probe.stdout" 2> "$test_dir/probe.stderr"
probe_status=$?
set -e
if [ "$probe_status" -eq 1 ] && grep -q 'model_incompatible' "$test_dir/probe.stderr"; then
  [ ! -e "$probe_archive" ]
  exit 0
fi
if [ "$probe_status" -ne 0 ]; then
  echo "video concurrency probe failed unexpectedly with status $probe_status" >&2
  cat "$test_dir/probe.stderr" >&2 || true
  exit 1
fi
rm -f "$probe_archive"

# Concurrency proof without timing assumptions: each FIFO writer waits until
# the other FIFO also has an active reader before writing any bytes. A
# sequential grouped ingest therefore cannot make progress; concurrent workers
# open both sources and release both writers.
fifo_a="$test_dir/camera-a.fifo"
fifo_b="$test_dir/camera-b.fifo"
ready_a="$test_dir/camera-a.ready"
ready_b="$test_dir/camera-b.ready"
mkfifo "$fifo_a" "$fifo_b"

(
  exec 3>"$fifo_a"
  touch "$ready_a"
  for _ in $(seq 1 400); do
    [ -e "$ready_b" ] && break
    sleep 0.01
  done
  [ -e "$ready_b" ]
  cat "$fixture" >&3
  exec 3>&-
) &
writer_a_pid=$!
(
  exec 4>"$fifo_b"
  touch "$ready_b"
  for _ in $(seq 1 400); do
    [ -e "$ready_a" ] && break
    sleep 0.01
  done
  [ -e "$ready_a" ]
  cat "$fixture" >&4
  exec 4>&-
) &
writer_b_pid=$!

archive="$test_dir/concurrent.coda"
set +e
timeout 8s "$codec_bin" video ingest \
  --archive "$archive" \
  --video \
    --source "$fifo_a" \
    --label camera-a \
    --start-ns 0 \
    --end-ns 1000000000 \
    --layout gray8 \
    --maximum-frames 4 \
  --video \
    --source "$fifo_b" \
    --label camera-b \
    --start-ns 0 \
    --end-ns 1000000000 \
    --layout rgb24 \
    --maximum-frames 4 \
  > "$test_dir/concurrent.jsonl" 2> "$test_dir/concurrent.stderr"
status=$?
set -e

if [ "$status" -ne 0 ]; then
  echo "grouped video ingest did not process FIFO sources concurrently; status $status" >&2
  cat "$test_dir/concurrent.stderr" >&2 || true
  exit 1
fi

wait "$writer_a_pid"; writer_a_pid=
wait "$writer_b_pid"; writer_b_pid=

python3 - "$test_dir/concurrent.jsonl" "$archive" <<'PY'
import json
import sys

records = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(records) != 2:
    raise SystemExit(f"expected two grouped video results, got {len(records)}")
if [record["archive"] for record in records] != [sys.argv[2], sys.argv[2]]:
    raise SystemExit("concurrent groups did not report the shared archive")
if not all(record["state_exact"] for record in records):
    raise SystemExit("concurrent grouped ingest did not preserve verified S1")
if records[0]["stream_id"] == records[1]["stream_id"]:
    raise SystemExit("concurrent groups unexpectedly share a stream ID")
PY

"$codec_bin" verify "$archive" --level full > "$test_dir/verify.json"
grep -q '"ok":true' "$test_dir/verify.json"

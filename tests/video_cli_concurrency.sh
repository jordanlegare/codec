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

# A hard failure in one worker must not discard a successful concurrent worker.
# The shared archive contains every usable staged stream and the command reports
# every group in command-line order before returning non-zero.
partial_archive="$test_dir/partial.coda"
missing_source="$test_dir/does-not-exist.mp4"
set +e
"$codec_bin" video ingest \
  --archive "$partial_archive" \
  --video \
    --source "$fixture" \
    --label camera-good \
    --start-ns 0 \
    --end-ns 1000000000 \
    --layout yuv420p8 \
    --maximum-frames 4 \
  --video \
    --source "$missing_source" \
    --label camera-missing \
    --start-ns 0 \
    --end-ns 1000000000 \
    --layout gray8 \
    --maximum-frames 4 \
  > "$test_dir/partial.jsonl" 2> "$test_dir/partial.stderr"
partial_status=$?
set -e
if [ "$partial_status" -ne 1 ]; then
  echo "partial grouped ingest should exit 1, got $partial_status" >&2
  exit 1
fi

test -s "$partial_archive"
"$codec_bin" verify "$partial_archive" --level full > "$test_dir/partial-verify.json"
grep -q '"ok":true' "$test_dir/partial-verify.json"

python3 - "$test_dir/partial.jsonl" "$partial_archive" "$test_dir/partial-good-stream.txt" <<'PY'
import json
import sys

records = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(records) != 2:
    raise SystemExit(f"expected two partial-ingest JSON lines, got {len(records)}")
if [record["archive"] for record in records] != [sys.argv[2], sys.argv[2]]:
    raise SystemExit("partial-ingest groups did not report the shared archive")
if [record["status"] for record in records] != ["ok", "error"]:
    raise SystemExit(f"unexpected partial-ingest statuses: {[record['status'] for record in records]}")
if not records[0]["state_exact"] or records[0]["frames"] != 1:
    raise SystemExit("successful partial-ingest stream lost verified S1")
if records[1]["error"] != "archive_io" or records[1]["preserved"] is not False:
    raise SystemExit("hard-failed partial-ingest stream was not reported as unpreserved archive_io")
if records[0]["stream_id"] == records[1]["stream_id"]:
    raise SystemExit("partial-ingest groups unexpectedly share a stream ID")
open(sys.argv[3], "w", encoding="utf-8").write(records[0]["stream_id"] + "\n")
PY

partial_good_stream=$(cat "$test_dir/partial-good-stream.txt")
partial_export="$test_dir/partial-good.mp4"
"$codec_bin" video export "$partial_archive" \
  --stream "$partial_good_stream" \
  --output "$partial_export" \
  --maximum-frames 4 \
  --maximum-input-bytes 1048576 \
  --maximum-output-bytes 1048576 \
  > "$test_dir/partial-export.json"
grep -q '"payload_type":"video/mp4"' "$test_dir/partial-export.json"
grep -q '"frames":1' "$test_dir/partial-export.json"
test -s "$partial_export"

if find "$test_dir" -maxdepth 1 -name 'partial.coda.video-*.tmp' -print -quit | grep -q .; then
  echo "partial grouped ingest leaked staging archives" >&2
  exit 1
fi

# A profile failure after S0 preservation is a usable staged stream. It must be
# merged beside successful streams and reported distinctly from a hard error.
printf 'not media\n' > "$test_dir/malformed.bin"
profile_archive="$test_dir/profile-partial.coda"
set +e
"$codec_bin" video ingest \
  --archive "$profile_archive" \
  --video \
    --source "$fixture" \
    --label camera-profile-good \
    --start-ns 0 \
    --end-ns 1000000000 \
    --layout yuv420p8 \
    --maximum-frames 4 \
  --video \
    --source "$test_dir/malformed.bin" \
    --label camera-profile-bad \
    --start-ns 0 \
    --end-ns 1000000000 \
    --layout gray8 \
    --maximum-frames 4 \
  > "$test_dir/profile-partial.jsonl" 2> "$test_dir/profile-partial.stderr"
profile_status=$?
set -e
if [ "$profile_status" -ne 1 ]; then
  echo "profile-error grouped ingest should exit 1, got $profile_status" >&2
  exit 1
fi

test -s "$profile_archive"
"$codec_bin" verify "$profile_archive" --level full > "$test_dir/profile-partial-verify.json"
grep -q '"ok":true' "$test_dir/profile-partial-verify.json"

python3 - "$test_dir/profile-partial.jsonl" "$profile_archive" "$test_dir/profile-good-stream.txt" <<'PY'
import json
import sys

records = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(records) != 2:
    raise SystemExit(f"expected two profile-partial JSON lines, got {len(records)}")
if [record["archive"] for record in records] != [sys.argv[2], sys.argv[2]]:
    raise SystemExit("profile-partial groups did not report the shared archive")
if [record["status"] for record in records] != ["ok", "profile_error"]:
    raise SystemExit(f"unexpected profile-partial statuses: {[record['status'] for record in records]}")
if not records[0]["state_exact"] or not records[0]["preserved"]:
    raise SystemExit("successful profile-partial stream lost verified S1")
failed = records[1]
if failed["profile_error"] != "decode" or failed["state_exact"]:
    raise SystemExit("malformed profile stream was not reported as a decode profile_error")
if failed["preserved"] is not True or failed["frames"] != 0 or failed["source_bytes"] <= 0:
    raise SystemExit("profile-error stream did not retain its preserved S0 source")
open(sys.argv[3], "w", encoding="utf-8").write(records[0]["stream_id"] + "\n")
PY

profile_good_stream=$(cat "$test_dir/profile-good-stream.txt")
profile_export="$test_dir/profile-good.mp4"
"$codec_bin" video export "$profile_archive" \
  --stream "$profile_good_stream" \
  --output "$profile_export" \
  --maximum-frames 4 \
  --maximum-input-bytes 1048576 \
  --maximum-output-bytes 1048576 \
  > "$test_dir/profile-export.json"
grep -q '"payload_type":"video/mp4"' "$test_dir/profile-export.json"
grep -q '"frames":1' "$test_dir/profile-export.json"
test -s "$profile_export"

if find "$test_dir" -maxdepth 1 -name 'profile-partial.coda.video-*.tmp' -print -quit | grep -q .; then
  echo "profile-error grouped ingest leaked staging archives" >&2
  exit 1
fi

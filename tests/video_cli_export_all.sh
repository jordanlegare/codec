#!/usr/bin/env bash
set -euo pipefail

codec_bin=${1:?codec binary path required}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test_dir=$(mktemp -d)
cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

"$codec_bin" --help > "$test_dir/help.txt"
grep -Fxq '  codec video ingest --archive FILE' "$test_dir/help.txt"
grep -Fxq '      --video --source URI --label LABEL --start-ns NS --end-ns NS [VIDEO OPTIONS]' "$test_dir/help.txt"
grep -Fxq '      [--video --source URI --label LABEL --start-ns NS --end-ns NS [VIDEO OPTIONS] ...]' "$test_dir/help.txt"
grep -Fxq '  Legacy single-video form:' "$test_dir/help.txt"
grep -Fxq '  codec video export ARCHIVE --all --output-dir DIR [EXPORT OPTIONS]' "$test_dir/help.txt"
grep -Fxq '  VIDEO OPTIONS:' "$test_dir/help.txt"
grep -Fxq '    --maximum-hls-resources N' "$test_dir/help.txt"
grep -Fxq '    --maximum-hls-resource-bytes N' "$test_dir/help.txt"
grep -Fxq '    --maximum-hls-total-bytes N' "$test_dir/help.txt"

fixture="$test_dir/fixture.mp4"
base64 --decode "$script_dir/fixtures/video_4x4_h264.mp4.b64" > "$fixture"

# FFmpeg-disabled builds still exercise the deterministic help surface but
# cannot exercise verified MP4 export.
probe_archive="$test_dir/probe.coda"
set +e
"$codec_bin" video ingest \
  --source "$fixture" \
  --archive "$probe_archive" \
  --label export-all-probe \
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
  echo "video export-all probe failed unexpectedly with status $probe_status" >&2
  cat "$test_dir/probe.stderr" >&2 || true
  exit 1
fi
rm -f "$probe_archive"

# Export every verified video stream from one shared CODA into one directory.
archive="$test_dir/all.coda"
"$codec_bin" video ingest \
  --archive "$archive" \
  --video \
    --source "$fixture" \
    --label camera-a \
    --start-ns 0 \
    --end-ns 1000000000 \
    --layout gray8 \
    --maximum-frames 4 \
  --video \
    --source "$fixture" \
    --label camera-b \
    --start-ns 0 \
    --end-ns 1000000000 \
    --layout rgb24 \
    --maximum-frames 4 \
  > "$test_dir/ingest.jsonl"

output_dir="$test_dir/exported"
"$codec_bin" video export "$archive" \
  --all \
  --output-dir "$output_dir" \
  --maximum-frames 4 \
  --maximum-input-bytes 1048576 \
  --maximum-output-bytes 1048576 \
  > "$test_dir/export-all.jsonl"

python3 - "$test_dir/export-all.jsonl" "$archive" "$output_dir" <<'PY'
import json
import os
import sys

records = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(records) != 2:
    raise SystemExit(f"expected two export-all results, got {len(records)}")
if [record["status"] for record in records] != ["ok", "ok"]:
    raise SystemExit(f"unexpected export-all statuses: {[record['status'] for record in records]}")
if [record["label"] for record in records] != ["camera-a", "camera-b"]:
    raise SystemExit("export-all did not preserve video descriptor order/labels")
if [record["archive"] for record in records] != [sys.argv[2], sys.argv[2]]:
    raise SystemExit("export-all did not report the source archive")
expected = ["camera-a.mp4", "camera-b.mp4"]
actual = [os.path.basename(record["output"]) for record in records]
if actual != expected:
    raise SystemExit(f"unexpected export-all filenames: {actual}")
for record in records:
    if record["payload_type"] != "video/mp4" or record["frames"] != 1:
        raise SystemExit("export-all did not use the verified MP4 exporter")
    if not os.path.isfile(record["output"]):
        raise SystemExit(f"missing export-all output: {record['output']}")
    if os.path.dirname(record["output"]) != sys.argv[3]:
        raise SystemExit("export-all output escaped the requested directory")
    payload = open(record["output"], "rb").read(12)
    if len(payload) < 8 or payload[4:8] != b"ftyp":
        raise SystemExit("export-all output is not an MP4")
PY

# Duplicate labels remain collision-safe by appending each stream UUID.
cp "$fixture" "$test_dir/fixture-copy.mp4"
collision_archive="$test_dir/collision.coda"
"$codec_bin" video ingest \
  --archive "$collision_archive" \
  --video \
    --source "$fixture" \
    --label camera \
    --start-ns 0 \
    --end-ns 1000000000 \
    --maximum-frames 4 \
  --video \
    --source "$test_dir/fixture-copy.mp4" \
    --label camera \
    --start-ns 0 \
    --end-ns 1000000000 \
    --maximum-frames 4 \
  > "$test_dir/collision-ingest.jsonl"

collision_dir="$test_dir/collision-out"
"$codec_bin" video export "$collision_archive" --all --output-dir "$collision_dir" \
  --maximum-frames 4 --maximum-input-bytes 1048576 --maximum-output-bytes 1048576 \
  > "$test_dir/collision-export.jsonl"
python3 - "$test_dir/collision-export.jsonl" <<'PY'
import json
import os
import re
import sys
records = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(records) != 2 or any(record["status"] != "ok" for record in records):
    raise SystemExit("duplicate-label export-all did not export both streams")
names = [os.path.basename(record["output"]) for record in records]
if len(set(names)) != 2:
    raise SystemExit("duplicate-label export-all filenames collided")
pattern = re.compile(r"^camera-[0-9a-f-]{36}\.mp4$")
if not all(pattern.match(name) for name in names):
    raise SystemExit(f"duplicate labels did not fall back to label-UUID filenames: {names}")
PY

# A preserved profile-error stream must not prevent verified peers from export.
printf 'not media\n' > "$test_dir/malformed.bin"
partial_archive="$test_dir/partial.coda"
set +e
"$codec_bin" video ingest \
  --archive "$partial_archive" \
  --video \
    --source "$fixture" \
    --label camera-good \
    --start-ns 0 \
    --end-ns 1000000000 \
    --maximum-frames 4 \
  --video \
    --source "$test_dir/malformed.bin" \
    --label camera-bad \
    --start-ns 0 \
    --end-ns 1000000000 \
    --maximum-frames 4 \
  > "$test_dir/partial-ingest.jsonl" 2> "$test_dir/partial-ingest.stderr"
partial_ingest_status=$?
set -e
if [ "$partial_ingest_status" -ne 1 ]; then
  echo "profile-error grouped ingest should exit 1, got $partial_ingest_status" >&2
  exit 1
fi

partial_dir="$test_dir/partial-out"
set +e
"$codec_bin" video export "$partial_archive" --all --output-dir "$partial_dir" \
  --maximum-frames 4 --maximum-input-bytes 1048576 --maximum-output-bytes 1048576 \
  > "$test_dir/partial-export.jsonl" 2> "$test_dir/partial-export.stderr"
partial_export_status=$?
set -e
if [ "$partial_export_status" -ne 1 ]; then
  echo "partial export-all should exit 1, got $partial_export_status" >&2
  exit 1
fi

python3 - "$test_dir/partial-export.jsonl" "$partial_dir" <<'PY'
import json
import os
import sys
records = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(records) != 2:
    raise SystemExit(f"expected two partial export-all results, got {len(records)}")
if [record["status"] for record in records] != ["ok", "error"]:
    raise SystemExit(f"unexpected partial export-all statuses: {[record['status'] for record in records]}")
if records[0]["label"] != "camera-good" or records[1]["label"] != "camera-bad":
    raise SystemExit("partial export-all reordered streams")
if not os.path.isfile(records[0]["output"]):
    raise SystemExit("partial export-all discarded successful output")
if records[1]["error"] != "invalid_argument":
    raise SystemExit(f"unexpected preserved-only export error: {records[1]['error']}")
if os.path.exists(os.path.join(sys.argv[2], "camera-bad.mp4")):
    raise SystemExit("partial export-all wrote an output for a non-exportable stream")
PY

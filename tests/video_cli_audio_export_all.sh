#!/usr/bin/env bash
set -euo pipefail

codec_bin=${1:?codec binary path required}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test_dir=$(mktemp -d)
cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

video_fixture="$test_dir/video.mp4"
audio_fixture="$test_dir/audio.mp4"
base64 --decode "$script_dir/fixtures/video_4x4_h264.mp4.b64" > "$video_fixture"
base64 --decode "$script_dir/fixtures/video_audio_mono.mp4.b64" > "$audio_fixture"

# FFmpeg-disabled builds cannot exercise MP4 export. Probe first so the
# dependency-free matrix keeps its explicit model_incompatible contract.
probe_archive="$test_dir/probe.coda"
set +e
"$codec_bin" video ingest \
  --source "$video_fixture" \
  --archive "$probe_archive" \
  --label audio-export-all-probe \
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
  echo "bulk audiovisual export probe failed unexpectedly with status $probe_status" >&2
  cat "$test_dir/probe.stderr" >&2 || true
  exit 1
fi
rm -f "$probe_archive"

archive="$test_dir/mixed.coda"
"$codec_bin" video ingest \
  --archive "$archive" \
  --video \
    --source "$audio_fixture" \
    --label camera-audio \
    --start-ns 0 \
    --end-ns 1000000000 \
    --maximum-decoded-audio-bytes 1048576 \
    --maximum-frames 8 \
  --video \
    --source "$video_fixture" \
    --label camera-video \
    --start-ns 0 \
    --end-ns 1000000000 \
    --maximum-frames 8 \
  > "$test_dir/ingest.jsonl"
"$codec_bin" verify "$archive" --level full > "$test_dir/verify.json"
grep -q '"ok":true' "$test_dir/verify.json"

output_dir="$test_dir/output"
"$codec_bin" video export "$archive" \
  --all \
  --output-dir "$output_dir" \
  --maximum-frames 8 \
  --maximum-input-bytes 1048576 \
  --maximum-output-bytes 1048576 \
  > "$test_dir/export.jsonl"

python3 - "$test_dir/export.jsonl" "$archive" "$output_dir" <<'PY'
import json
import os
import sys

records = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(records) != 2:
    raise SystemExit(f"expected two bulk audiovisual exports, got {len(records)}")
if [record["status"] for record in records] != ["ok", "ok"]:
    raise SystemExit(f"unexpected bulk audiovisual statuses: {[record['status'] for record in records]}")
if [record["label"] for record in records] != ["camera-audio", "camera-video"]:
    raise SystemExit("bulk audiovisual export reordered descriptors")
if [record["archive"] for record in records] != [sys.argv[2], sys.argv[2]]:
    raise SystemExit("bulk audiovisual export did not report its source archive")
if [record.get("audio") for record in records] != [True, False]:
    raise SystemExit(f"bulk export did not report verified audio truth: {records}")
expected_names = ["camera-audio.mp4", "camera-video.mp4"]
if [os.path.basename(record["output"]) for record in records] != expected_names:
    raise SystemExit("bulk audiovisual output filenames changed unexpectedly")
for record in records:
    if record["payload_type"] != "video/mp4" or record["frames"] <= 0:
        raise SystemExit("bulk audiovisual output did not use verified MP4 export")
    if not os.path.isfile(record["output"]):
        raise SystemExit(f"missing bulk audiovisual output: {record['output']}")
    if os.path.dirname(record["output"]) != sys.argv[3]:
        raise SystemExit("bulk audiovisual output escaped requested directory")
PY

python3 - "$output_dir/camera-audio.mp4" "$output_dir/camera-video.mp4" <<'PY'
import sys
audio_payload = open(sys.argv[1], "rb").read()
video_payload = open(sys.argv[2], "rb").read()
if b"soun" not in audio_payload:
    raise SystemExit("bulk audiovisual export omitted the AAC audio track")
if b"soun" in video_payload:
    raise SystemExit("bulk video-only export unexpectedly contains an audio track")
PY

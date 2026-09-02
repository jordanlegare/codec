#!/usr/bin/env bash
set -euo pipefail

codec_bin=${1:?codec binary path required}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
python3 - "$script_dir/../CMakeLists.txt" <<'PY'
import re
import sys

text = open(sys.argv[1], encoding="utf-8").read()
pattern = re.compile(
    r'option\s*\(\s*CODEC_ENABLE_FFMPEG_VIDEO\s*'
    r'"[^"]*"\s+ON\s*\)',
    re.MULTILINE,
)
if not pattern.search(text):
    raise SystemExit(
        "CODEC_ENABLE_FFMPEG_VIDEO must default to ON in CMakeLists.txt"
    )
PY

test_dir=$(mktemp -d)
cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fixture="$test_dir/fixture.mp4"
audio_fixture="$test_dir/audio-fixture.mp4"
base64 --decode "$script_dir/fixtures/video_4x4_h264.mp4.b64" > "$fixture"
base64 --decode "$script_dir/fixtures/video_audio_mono.mp4.b64" > "$audio_fixture"

"$codec_bin" --help > "$test_dir/help.txt"
grep -Fq 'codec video ingest' "$test_dir/help.txt"
grep -Fq 'codec video export' "$test_dir/help.txt"
grep -Fq 'codec list videos ARCHIVE' "$test_dir/help.txt"
grep -Fq -- 'codec video ingest --archive FILE' "$test_dir/help.txt"
grep -Fq -- '--maximum-hls-resources' "$test_dir/help.txt"
grep -Fq -- '--maximum-hls-resource-bytes' "$test_dir/help.txt"
grep -Fq -- '--maximum-hls-total-bytes' "$test_dir/help.txt"
grep -Fxq '    --maximum-decoded-audio-bytes N' "$test_dir/help.txt"
python3 - "$test_dir/help.txt" <<'PY'
import sys
lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
long = [(index + 1, len(line), line) for index, line in enumerate(lines) if len(line) > 100]
if long:
    raise SystemExit(f"video help contains overlong lines: {long}")
PY

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

# Repeated-group syntax has one command-level archive and must preflight every
# group before creating it. The second group intentionally omits --label.
multi_invalid="$test_dir/multi-invalid.coda"
set +e
"$codec_bin" video ingest \
  --archive "$multi_invalid" \
  --video \
    --source "$fixture" \
    --label multi-a \
    --start-ns 0 \
    --end-ns 1000000000 \
  --video \
    --source "$fixture" \
    --start-ns 0 \
    --end-ns 1000000000 \
  > "$test_dir/multi-invalid.stdout" 2> "$test_dir/multi-invalid.stderr"
multi_invalid_status=$?
set -e
if [ "$multi_invalid_status" -ne 2 ]; then
  echo "repeated video ingest must preflight every group; got $multi_invalid_status" >&2
  exit 1
fi
[ ! -e "$multi_invalid" ]

for hls_option in \
  --maximum-hls-resources \
  --maximum-hls-resource-bytes \
  --maximum-hls-total-bytes; do
  option_name=${hls_option#--}
  invalid_hls_archive="$test_dir/$option_name.coda"
  set +e
  "$codec_bin" video ingest \
    --source "$fixture" \
    --archive "$invalid_hls_archive" \
    --label "invalid-$option_name" \
    --start-ns 0 \
    --end-ns 1000000000 \
    "$hls_option" 0 \
    > "$test_dir/$option_name.stdout" \
    2> "$test_dir/$option_name.stderr"
  invalid_hls_status=$?
  set -e
  if [ "$invalid_hls_status" -ne 2 ]; then
    echo "$hls_option zero case should exit 2, got $invalid_hls_status" >&2
    exit 1
  fi
  [ ! -e "$invalid_hls_archive" ]
done

invalid_audio_limit_archive="$test_dir/maximum-decoded-audio-bytes.coda"
set +e
"$codec_bin" video ingest \
  --source "$audio_fixture" \
  --archive "$invalid_audio_limit_archive" \
  --label invalid-audio-limit \
  --start-ns 0 \
  --end-ns 1000000000 \
  --maximum-decoded-audio-bytes 0 \
  > "$test_dir/maximum-decoded-audio-bytes.stdout" \
  2> "$test_dir/maximum-decoded-audio-bytes.stderr"
invalid_audio_limit_status=$?
set -e
if [ "$invalid_audio_limit_status" -ne 2 ]; then
  echo "--maximum-decoded-audio-bytes zero case should exit 2, got $invalid_audio_limit_status" >&2
  exit 1
fi
[ ! -e "$invalid_audio_limit_archive" ]

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
grep -q '"secondary_sources":0' "$test_dir/probe.stdout"
grep -q '"secondary_source_bytes":0' "$test_dir/probe.stdout"
grep -q '"layout":"yuv420p8"' "$test_dir/probe.stdout"
grep -q '"stream_id":"' "$test_dir/probe.stdout"
grep -q '"audio_present":false' "$test_dir/probe.stdout"
grep -q '"audio_state_exact":false' "$test_dir/probe.stdout"
if grep -Fqa "$fixture" "$probe_archive"; then
  echo "video ingest archive persisted the raw source URI/path" >&2
  exit 1
fi
"$codec_bin" verify "$probe_archive" --level full > "$test_dir/probe-verify.json"
grep -q '"ok":true' "$test_dir/probe-verify.json"

# A direct audiovisual source exposes its strict H.1 PCM state through the CLI
# and honors the dedicated decoded-audio resource bound.
audio_archive="$test_dir/audio.coda"
set +e
"$codec_bin" video ingest \
  --source "$audio_fixture" \
  --archive "$audio_archive" \
  --label camera-audio \
  --start-ns 0 \
  --end-ns 1000000000 \
  --layout yuv420p8 \
  --maximum-decoded-audio-bytes 1048576 \
  > "$test_dir/audio-ingest.json"
audio_ingest_status=$?
set -e
if [ "$audio_ingest_status" -ne 0 ]; then
  echo "audiovisual CLI ingest failed with status $audio_ingest_status" >&2
  cat "$test_dir/audio-ingest.json" >&2 || true
  exit 1
fi
grep -q '"state_exact":true' "$test_dir/audio-ingest.json"
grep -q '"audio_present":true' "$test_dir/audio-ingest.json"
grep -q '"audio_state_exact":true' "$test_dir/audio-ingest.json"
"$codec_bin" verify "$audio_archive" --level full > "$test_dir/audio-verify.json"
grep -q '"ok":true' "$test_dir/audio-verify.json"
"$codec_bin" list videos "$audio_archive" > "$test_dir/audio-videos.jsonl"
python3 - "$test_dir/audio-videos.jsonl" "$test_dir/audio-ingest.json" <<'PY'
import json
import sys

lines = [line for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(lines) != 1:
    raise SystemExit(f"expected one audiovisual video descriptor, got {len(lines)}")
record = json.loads(lines[0])
ingest = json.load(open(sys.argv[2], encoding="utf-8"))
expected = {
    "label": "camera-audio",
    "stream_id": ingest["stream_id"],
    "source_id": "codec.video.cli",
    "payload_type": "application/octet-stream",
    "fidelity": "S0",
}
if record != expected:
    raise SystemExit(f"unexpected audiovisual video descriptor: {record!r}")
PY

audio_stream=$(python3 - "$test_dir/audio-ingest.json" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["stream_id"])
PY
)
audio_export="$test_dir/audio-export.mp4"
"$codec_bin" video export "$audio_archive" \
  --stream "$audio_stream" \
  --output "$audio_export" \
  --maximum-frames 8 \
  --maximum-input-bytes 1048576 \
  --maximum-output-bytes 1048576 \
  > "$test_dir/audio-export.json"
grep -q '"payload_type":"video/mp4"' "$test_dir/audio-export.json"
grep -q '"audio":true' "$test_dir/audio-export.json"
test -s "$audio_export"
python3 - "$audio_export" <<'PY'
import sys
payload = open(sys.argv[1], "rb").read()
if b"soun" not in payload:
    raise SystemExit("audiovisual CLI export has no MP4 audio handler")
PY

# Two valid repeated --video groups land in one CODA and emit ordered JSONL.
multi_archive="$test_dir/multi.coda"
"$codec_bin" video ingest \
  --archive "$multi_archive" \
  --video \
    --source "$fixture" \
    --label camera-a \
    --start-ns 1000000000 \
    --end-ns 2000000000 \
    --layout gray8 \
    --maximum-frames 4 \
  --video \
    --source "$fixture" \
    --label camera-b \
    --start-ns 2000000000 \
    --end-ns 3000000000 \
    --layout rgb24 \
    --maximum-frames 4 \
  > "$test_dir/multi.jsonl"
python3 - "$test_dir/multi.jsonl" "$multi_archive" "$test_dir/multi-streams.txt" <<'PY'
import json
import sys

lines = [line for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(lines) != 2:
    raise SystemExit(f"expected two repeated-ingest JSON lines, got {len(lines)}")
records = [json.loads(line) for line in lines]
if [record["archive"] for record in records] != [sys.argv[2], sys.argv[2]]:
    raise SystemExit("repeated-ingest groups did not report one shared archive")
if [record["layout"] for record in records] != ["gray8", "rgb24"]:
    raise SystemExit("repeated-ingest layouts do not match their groups")
if not all(record["state_exact"] for record in records):
    raise SystemExit("repeated-ingest group did not produce exact state")
if any(record["audio_present"] or record["audio_state_exact"] for record in records):
    raise SystemExit("video-only repeated-ingest group unexpectedly reported audio")
streams = [record["stream_id"] for record in records]
if streams[0] == streams[1]:
    raise SystemExit("distinct repeated-ingest groups unexpectedly share a stream ID")
open(sys.argv[3], "w", encoding="utf-8").write("\n".join(streams) + "\n")
PY
"$codec_bin" verify "$multi_archive" --level full > "$test_dir/multi-verify.json"
grep -q '"ok":true' "$test_dir/multi-verify.json"
"$codec_bin" list videos "$multi_archive" > "$test_dir/multi-videos.jsonl"
python3 - "$test_dir/multi-videos.jsonl" "$test_dir/multi.jsonl" <<'PY'
import json
import sys

listed = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
ingested = [json.loads(line) for line in open(sys.argv[2], encoding="utf-8") if line.strip()]
expected = [
    {
        "label": label,
        "stream_id": record["stream_id"],
        "source_id": "codec.video.cli",
        "payload_type": "application/octet-stream",
        "fidelity": "S0",
    }
    for label, record in zip(("camera-a", "camera-b"), ingested, strict=True)
]
if listed != expected:
    raise SystemExit(f"video descriptors are missing, reordered, or malformed: {listed!r}")
PY
mapfile -t multi_streams < "$test_dir/multi-streams.txt"
for index in 0 1; do
  output="$test_dir/multi-$index.mp4"
  "$codec_bin" video export "$multi_archive" \
    --stream "${multi_streams[$index]}" \
    --output "$output" \
    --maximum-frames 4 \
    --maximum-input-bytes 1048576 \
    --maximum-output-bytes 1048576 \
    > "$test_dir/multi-$index-export.json"
  grep -q '"payload_type":"video/mp4"' "$test_dir/multi-$index-export.json"
  grep -q '"frames":1' "$test_dir/multi-$index-export.json"
  grep -q '"audio":false' "$test_dir/multi-$index-export.json"
  test -s "$output"
done

probe_stream=$(python3 - "$test_dir/probe.stdout" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["stream_id"])
PY
)
export_output="$test_dir/probe-export.mp4"
"$codec_bin" video export "$probe_archive" \
  --stream "$probe_stream" \
  --output "$export_output" \
  --maximum-frames 4 \
  --maximum-input-bytes 1048576 \
  --maximum-output-bytes 1048576 \
  > "$test_dir/export.json"
grep -q '"payload_type":"video/mp4"' "$test_dir/export.json"
grep -q '"frames":1' "$test_dir/export.json"
grep -q '"audio":false' "$test_dir/export.json"
grep -q "\"stream_id\":\"$probe_stream\"" "$test_dir/export.json"
test -s "$export_output"
python3 - "$export_output" <<'PY'
import sys
payload = open(sys.argv[1], "rb").read(12)
if len(payload) < 8 or payload[4:8] != b"ftyp":
    raise SystemExit("video export did not write an MP4 ftyp box")
PY

invalid_export="$test_dir/invalid-export.mp4"
set +e
"$codec_bin" video export "$probe_archive" \
  --stream "$probe_stream" \
  --output "$invalid_export" \
  --maximum-output-bytes 0 \
  > "$test_dir/invalid-export.stdout" 2> "$test_dir/invalid-export.stderr"
invalid_export_status=$?
set -e
if [ "$invalid_export_status" -ne 2 ]; then
  echo "video export zero output limit should exit 2, got $invalid_export_status" >&2
  exit 1
fi
[ ! -e "$invalid_export" ]

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
  grep -q '"audio_present":false' "$test_dir/$layout.json"
  grep -q '"audio_state_exact":false' "$test_dir/$layout.json"
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
grep -q '"audio_present":false' "$test_dir/malformed.stdout"
grep -q '"audio_state_exact":false' "$test_dir/malformed.stdout"
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

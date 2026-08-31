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
audio_fixture_copy="$test_dir/audio-copy.mp4"
base64 --decode "$script_dir/fixtures/video_4x4_h264.mp4.b64" > "$video_fixture"
base64 --decode "$script_dir/fixtures/video_audio_mono.mp4.b64" > "$audio_fixture"
cp "$audio_fixture" "$audio_fixture_copy"

# FFmpeg-disabled builds cannot exercise audiovisual workers. Probe the backend
# before the grouped behavior assertions so the dependency-free matrix remains
# a truthful model_incompatible guardrail.
probe_archive="$test_dir/probe.coda"
set +e
"$codec_bin" video ingest \
  --source "$video_fixture" \
  --archive "$probe_archive" \
  --label audio-group-probe \
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
  echo "grouped audiovisual probe failed unexpectedly with status $probe_status" >&2
  cat "$test_dir/probe.stderr" >&2 || true
  exit 1
fi
rm -f "$probe_archive"

# A positive decoded-audio limit is a supported per-group option and reaches
# the audiovisual ingest backend rather than being rejected as an unknown flag.
limit_archive="$test_dir/grouped-limit.coda"
"$codec_bin" video ingest \
  --archive "$limit_archive" \
  --video \
    --source "$audio_fixture" \
    --label camera-limit \
    --start-ns 0 \
    --end-ns 1000000000 \
    --maximum-decoded-audio-bytes 1048576 \
    --maximum-frames 8 \
  > "$test_dir/grouped-limit.jsonl"
python3 - "$test_dir/grouped-limit.jsonl" <<'PY'
import json
import sys
records = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(records) != 1:
    raise SystemExit(f"expected one grouped audio-limit result, got {len(records)}")
record = records[0]
if record["status"] != "ok":
    raise SystemExit(f"grouped audio limit was not accepted: {record}")
if not record["audio_present"] or not record["audio_state_exact"] or not record["state_exact"]:
    raise SystemExit("grouped audio-limit ingest did not preserve verified audiovisual state")
PY
"$codec_bin" verify "$limit_archive" --level full > "$test_dir/grouped-limit-verify.json"
grep -q '"ok":true' "$test_dir/grouped-limit-verify.json"

# Two independent audiovisual workers must merge their video and PCM16 S1 plus
# exact provenance into one finalized CODA. Re-export through the strict reader
# proves the rewritten provenance links remain valid after the merge.
av_archive="$test_dir/two-av.coda"
"$codec_bin" video ingest \
  --archive "$av_archive" \
  --video \
    --source "$audio_fixture" \
    --label camera-av-a \
    --start-ns 0 \
    --end-ns 1000000000 \
    --maximum-decoded-audio-bytes 1048576 \
    --maximum-frames 8 \
  --video \
    --source "$audio_fixture_copy" \
    --label camera-av-b \
    --start-ns 1000000000 \
    --end-ns 2000000000 \
    --maximum-decoded-audio-bytes 1048576 \
    --maximum-frames 8 \
  > "$test_dir/two-av.jsonl"
python3 - "$test_dir/two-av.jsonl" "$av_archive" "$test_dir/two-av-streams.txt" <<'PY'
import json
import sys
records = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(records) != 2:
    raise SystemExit(f"expected two grouped audiovisual results, got {len(records)}")
if [record["archive"] for record in records] != [sys.argv[2], sys.argv[2]]:
    raise SystemExit("grouped audiovisual workers did not report one shared archive")
if [record["status"] for record in records] != ["ok", "ok"]:
    raise SystemExit(f"unexpected grouped audiovisual statuses: {[record['status'] for record in records]}")
if not all(record["audio_present"] and record["audio_state_exact"] and record["state_exact"] for record in records):
    raise SystemExit("grouped audiovisual merge lost verified audio or exact state")
streams = [record["stream_id"] for record in records]
if streams[0] == streams[1]:
    raise SystemExit("grouped audiovisual workers unexpectedly share a stream ID")
open(sys.argv[3], "w", encoding="utf-8").write("\n".join(streams) + "\n")
PY
"$codec_bin" verify "$av_archive" --level full > "$test_dir/two-av-verify.json"
grep -q '"ok":true' "$test_dir/two-av-verify.json"
mapfile -t av_streams < "$test_dir/two-av-streams.txt"
for index in 0 1; do
  output="$test_dir/two-av-$index.mp4"
  "$codec_bin" video export "$av_archive" \
    --stream "${av_streams[$index]}" \
    --output "$output" \
    --maximum-frames 8 \
    --maximum-input-bytes 1048576 \
    --maximum-output-bytes 1048576 \
    > "$test_dir/two-av-$index-export.json"
  grep -q '"audio":true' "$test_dir/two-av-$index-export.json"
  test -s "$output"
  python3 - "$output" <<'PY'
import sys
payload = open(sys.argv[1], "rb").read()
if b"soun" not in payload:
    raise SystemExit("grouped audiovisual export lost its MP4 audio handler")
PY
done

# An audio-only profile failure is still a usable staged stream when video S1
# succeeded. The good peer remains exact; the failed-audio peer remains
# preserved with verified video and exports truthfully as video-only.
partial_archive="$test_dir/audio-partial.coda"
set +e
"$codec_bin" video ingest \
  --archive "$partial_archive" \
  --video \
    --source "$audio_fixture" \
    --label camera-audio-good \
    --start-ns 0 \
    --end-ns 1000000000 \
    --maximum-decoded-audio-bytes 1048576 \
    --maximum-frames 8 \
  --video \
    --source "$audio_fixture_copy" \
    --label camera-audio-limited \
    --start-ns 0 \
    --end-ns 1000000000 \
    --maximum-decoded-audio-bytes 2 \
    --maximum-frames 8 \
  > "$test_dir/audio-partial.jsonl" 2> "$test_dir/audio-partial.stderr"
partial_status=$?
set -e
if [ "$partial_status" -ne 1 ]; then
  echo "audio profile-error grouped ingest should exit 1, got $partial_status" >&2
  cat "$test_dir/audio-partial.stderr" >&2 || true
  exit 1
fi
test -s "$partial_archive"
"$codec_bin" verify "$partial_archive" --level full > "$test_dir/audio-partial-verify.json"
grep -q '"ok":true' "$test_dir/audio-partial-verify.json"
python3 - "$test_dir/audio-partial.jsonl" "$partial_archive" "$test_dir/audio-partial-streams.txt" <<'PY'
import json
import sys
records = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(records) != 2:
    raise SystemExit(f"expected two audio-partial results, got {len(records)}")
if [record["archive"] for record in records] != [sys.argv[2], sys.argv[2]]:
    raise SystemExit("audio-partial groups did not report one shared archive")
if [record["status"] for record in records] != ["ok", "profile_error"]:
    raise SystemExit(f"unexpected audio-partial statuses: {[record['status'] for record in records]}")
good, limited = records
if not good["audio_present"] or not good["audio_state_exact"] or not good["state_exact"]:
    raise SystemExit("good audiovisual peer lost exact audio state")
if not limited["audio_present"] or limited["audio_state_exact"] or limited["state_exact"]:
    raise SystemExit("audio-limited peer did not expose partial audiovisual truth")
if limited["profile_error"] != "resource_exhausted" or limited["preserved"] is not True:
    raise SystemExit("audio-limited peer did not retain preserved video after resource exhaustion")
if limited["frames"] <= 0:
    raise SystemExit("audio-limited peer lost verified video S1")
open(sys.argv[3], "w", encoding="utf-8").write(good["stream_id"] + "\n" + limited["stream_id"] + "\n")
PY
mapfile -t partial_streams < "$test_dir/audio-partial-streams.txt"
for index in 0 1; do
  output="$test_dir/audio-partial-$index.mp4"
  "$codec_bin" video export "$partial_archive" \
    --stream "${partial_streams[$index]}" \
    --output "$output" \
    --maximum-frames 8 \
    --maximum-input-bytes 1048576 \
    --maximum-output-bytes 1048576 \
    > "$test_dir/audio-partial-$index-export.json"
  test -s "$output"
done
grep -q '"audio":true' "$test_dir/audio-partial-0-export.json"
grep -q '"audio":false' "$test_dir/audio-partial-1-export.json"

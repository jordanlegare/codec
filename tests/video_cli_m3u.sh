#!/usr/bin/env bash
set -euo pipefail

codec_bin=${1:?codec binary path required}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test_dir=$(mktemp -d)
cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fixture="$test_dir/probe.mp4"
base64 --decode "$script_dir/fixtures/video_4x4_h264.mp4.b64" > "$fixture"

# FFmpeg-disabled builds cannot exercise playlist ingestion; the normal video
# CLI integration test already proves explicit backend unavailability.
probe_archive="$test_dir/probe.coda"
set +e
"$codec_bin" video ingest \
  --source "$fixture" \
  --archive "$probe_archive" \
  --label m3u-probe \
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
  echo "video M3U probe failed unexpectedly with status $probe_status" >&2
  cat "$test_dir/probe.stderr" >&2 || true
  exit 1
fi
rm -f "$probe_archive"

playlist_dir="$test_dir/playlists"
mkdir -p "$playlist_dir/sub"
cp "$fixture" "$playlist_dir/camera-a.mp4"
cp "$fixture" "$playlist_dir/sub/camera-b.mp4"

# Extended M3U: BOM/CRLF, comments and blank lines are ignored; EXTINF titles
# become labels; relative media paths resolve against the playlist directory;
# command-level timing/options apply to entries that do not supply duration.
printf '\357\273\277#EXTM3U\r\n# source set\r\n#EXTINF:1.0,Camera Alpha\r\ncamera-a.mp4\r\n\r\n# no EXTINF for this entry\r\nsub/camera-b.mp4\r\n' \
  > "$playlist_dir/cameras.m3u8"

archive="$test_dir/cameras.coda"
"$codec_bin" video ingest \
  --archive "$archive" \
  --m3u "$playlist_dir/cameras.m3u8" \
  --start-ns 0 \
  --end-ns 1000000000 \
  --layout gray8 \
  --maximum-frames 4 \
  --maximum-source-bytes 1048576 \
  --maximum-decoded-bytes 1048576 \
  --maximum-decoded-audio-bytes 1048576 \
  > "$test_dir/cameras.jsonl" 2> "$test_dir/cameras.stderr"

test -s "$archive"
"$codec_bin" verify "$archive" --level full > "$test_dir/cameras-verify.json"
grep -q '"ok":true' "$test_dir/cameras-verify.json"
"$codec_bin" list videos "$archive" > "$test_dir/cameras-videos.jsonl"

python3 - "$test_dir/cameras.jsonl" "$test_dir/cameras-videos.jsonl" "$archive" <<'PY'
import json
import sys

results = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(results) != 2:
    raise SystemExit(f"expected two M3U ingest results, got {len(results)}")
if [record["archive"] for record in results] != [sys.argv[3], sys.argv[3]]:
    raise SystemExit("M3U entries did not report the shared archive")
if [record["status"] for record in results] != ["ok", "ok"]:
    raise SystemExit(f"unexpected M3U ingest statuses: {[record['status'] for record in results]}")
if [record["layout"] for record in results] != ["gray8", "gray8"]:
    raise SystemExit("command-level M3U layout did not apply to every entry")
if not all(record["state_exact"] and record["preserved"] for record in results):
    raise SystemExit("M3U ingest did not preserve verified video state")
if results[0]["stream_id"] == results[1]["stream_id"]:
    raise SystemExit("M3U entries unexpectedly share a stream ID")

videos = [json.loads(line) for line in open(sys.argv[2], encoding="utf-8") if line.strip()]
labels = [record["label"] for record in videos]
if labels != ["Camera Alpha", "camera-b.mp4"]:
    raise SystemExit(f"unexpected M3U-derived labels: {labels}")
PY

# Plain M3U without an extended header or durations is accepted when the
# command supplies the existing video-ingest interval. Relative paths still
# resolve from the playlist location and the basename is the label.
printf '# plain playlist\ncamera-a.mp4\n' > "$playlist_dir/plain.m3u"
plain_archive="$test_dir/plain.coda"
"$codec_bin" video ingest \
  --archive "$plain_archive" \
  --m3u "$playlist_dir/plain.m3u" \
  --start-ns 0 \
  --end-ns 1000000000 \
  --maximum-frames 4 \
  > "$test_dir/plain.jsonl" 2> "$test_dir/plain.stderr"
"$codec_bin" verify "$plain_archive" --level full > "$test_dir/plain-verify.json"
grep -q '"ok":true' "$test_dir/plain-verify.json"
"$codec_bin" list videos "$plain_archive" > "$test_dir/plain-videos.jsonl"
grep -q '"label":"camera-a.mp4"' "$test_dir/plain-videos.jsonl"

# A quoted wildcard is expanded by CODEC, not by the shell. Matching playlist
# files are processed in lexical path order, and each playlist retains its own
# relative-path base directory.
mkdir -p "$playlist_dir/glob/a" "$playlist_dir/glob/b"
cp "$fixture" "$playlist_dir/glob/a/wild-a.mp4"
cp "$fixture" "$playlist_dir/glob/b/wild-b.mp4"
printf '#EXTM3U\n#EXTINF:1,Wildcard Alpha\na/wild-a.mp4\n' \
  > "$playlist_dir/glob/set-01.m3u"
printf '#EXTM3U\n#EXTINF:1,Wildcard Beta\nb/wild-b.mp4\n' \
  > "$playlist_dir/glob/set-02.m3u"
glob_archive="$test_dir/glob.coda"
"$codec_bin" video ingest \
  --archive "$glob_archive" \
  --m3u "$playlist_dir/glob/set-*.m3u" \
  --start-ns 0 \
  --maximum-frames 4 \
  > "$test_dir/glob.jsonl" 2> "$test_dir/glob.stderr"
"$codec_bin" verify "$glob_archive" --level full > "$test_dir/glob-verify.json"
grep -q '"ok":true' "$test_dir/glob-verify.json"
"$codec_bin" list videos "$glob_archive" > "$test_dir/glob-videos.jsonl"
python3 - "$test_dir/glob.jsonl" "$test_dir/glob-videos.jsonl" <<'PY'
import json
import sys

results = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
if len(results) != 2 or [record["status"] for record in results] != ["ok", "ok"]:
    raise SystemExit(f"unexpected wildcard M3U results: {results}")
videos = [json.loads(line) for line in open(sys.argv[2], encoding="utf-8") if line.strip()]
labels = [record["label"] for record in videos]
if labels != ["Wildcard Alpha", "Wildcard Beta"]:
    raise SystemExit(f"wildcard playlists were not processed lexically: {labels}")
PY

expect_status_2() {
  local stdout_path=$1
  local stderr_path=$2
  shift 2
  set +e
  "$@" > "$stdout_path" 2> "$stderr_path"
  local status=$?
  set -e
  if [ "$status" -ne 2 ]; then
    echo "expected status 2, got $status: $*" >&2
    return 1
  fi
}

# An unmatched wildcard is a CLI error and must not create an archive.
unmatched_archive="$test_dir/unmatched.coda"
expect_status_2 "$test_dir/unmatched.stdout" "$test_dir/unmatched.stderr" \
  "$codec_bin" video ingest --archive "$unmatched_archive" \
  --m3u "$playlist_dir/glob/missing-*.m3u"
[ ! -e "$unmatched_archive" ]
grep -qi 'matched no' "$test_dir/unmatched.stderr"

# Duplicate normalized media entries are rejected before any archive is made,
# even if EXTINF titles differ.
printf '#EXTM3U\n#EXTINF:1,First\ncamera-a.mp4\n#EXTINF:1,Second\n./camera-a.mp4\n' \
  > "$playlist_dir/duplicate.m3u"
duplicate_archive="$test_dir/duplicate.coda"
expect_status_2 "$test_dir/duplicate.stdout" "$test_dir/duplicate.stderr" \
  "$codec_bin" video ingest --archive "$duplicate_archive" \
  --m3u "$playlist_dir/duplicate.m3u"
[ ! -e "$duplicate_archive" ]
grep -qi 'duplicate' "$test_dir/duplicate.stderr"

# Empty/comment-only and malformed dangling EXTINF playlists fail before
# archive creation.
printf '#EXTM3U\n# comments only\n\n' > "$playlist_dir/empty.m3u"
empty_archive="$test_dir/empty.coda"
expect_status_2 "$test_dir/empty.stdout" "$test_dir/empty.stderr" \
  "$codec_bin" video ingest --archive "$empty_archive" \
  --m3u "$playlist_dir/empty.m3u"
[ ! -e "$empty_archive" ]

printf '#EXTM3U\n#EXTINF:1,Missing media\n' > "$playlist_dir/dangling.m3u"
dangling_archive="$test_dir/dangling.coda"
expect_status_2 "$test_dir/dangling.stdout" "$test_dir/dangling.stderr" \
  "$codec_bin" video ingest --archive "$dangling_archive" \
  --m3u "$playlist_dir/dangling.m3u"
[ ! -e "$dangling_archive" ]

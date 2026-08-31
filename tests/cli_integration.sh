#!/usr/bin/env bash
set -euo pipefail

codec_bin=${1:?codec binary path required}
expected_version=${2:?expected CODEC version required}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test_dir=$(mktemp -d)
record_pid=
feed_a_pid=
feed_b_pid=
follow_pid=
cleanup() {
  touch "$test_dir/release-live" 2>/dev/null || true
  for pid in "$follow_pid" "$record_pid" "$feed_a_pid" "$feed_b_pid"; do
    if [ -n "$pid" ]; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  rm -rf "$test_dir"
}
trap cleanup EXIT

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

printf 'internet audio source bytes\n' > "$test_dir/input.bin"
"$codec_bin" capabilities > "$test_dir/capabilities.json"
grep -Fq "\"version\":\"$expected_version\"" "$test_dir/capabilities.json"
grep -q '"neural_separation":false' "$test_dir/capabilities.json"

"$codec_bin" record --archive "$test_dir/session.coda" \
  --feed "news=$test_dir/input.bin"
"$codec_bin" verify "$test_dir/session.coda" --level full
"$codec_bin" list feeds "$test_dir/session.coda" > "$test_dir/feeds.jsonl"
grep -q '"label":"news"' "$test_dir/feeds.jsonl"
"$codec_bin" extract "$test_dir/session.coda" --feed news \
  --output "$test_dir/extracted.bin" > "$test_dir/extract.json"
grep -q '"fidelity":"source_exact"' "$test_dir/extract.json"
cmp "$test_dir/input.bin" "$test_dir/extracted.bin"

"$codec_bin" extract "$test_dir/session.coda" --feed news \
  --fidelity source-exact --output "$test_dir/explicit-extracted.bin"
cmp "$test_dir/input.bin" "$test_dir/explicit-extracted.bin"

expect_status_2 "$test_dir/list-streams.stdout" \
  "$test_dir/list-streams.stderr" \
  "$codec_bin" list streams "$test_dir/session.coda"
grep -q 'list supports: list feeds' "$test_dir/list-streams.stderr"

printf 'sentinel output\n' > "$test_dir/stream-output.bin"
cp "$test_dir/stream-output.bin" "$test_dir/expected-sentinel.bin"
expect_status_2 "$test_dir/stream.stdout" "$test_dir/stream.stderr" \
  "$codec_bin" extract "$test_dir/session.coda" \
  --stream 00000000-0000-0000-0000-000000000001 \
  --output "$test_dir/stream-output.bin"
cmp "$test_dir/expected-sentinel.bin" "$test_dir/stream-output.bin"

# E.3: follow a selected feed from a still-growing, concurrently multiplexed
# archive. Both FIFO writers keep their source open until the test confirms
# that LABEL1 bytes have already been emitted by the follower.
mkfifo "$test_dir/live-a.fifo" "$test_dir/live-b.fifo"
(
  exec 3>"$test_dir/live-a.fifo"
  printf 'alpha-live\n' >&3
  touch "$test_dir/a-written"
  while [ ! -e "$test_dir/release-live" ]; do sleep 0.02; done
  exec 3>&-
) &
feed_a_pid=$!
(
  exec 4>"$test_dir/live-b.fifo"
  printf 'beta-live\n' >&4
  touch "$test_dir/b-written"
  while [ ! -e "$test_dir/release-live" ]; do sleep 0.02; done
  exec 4>&-
) &
feed_b_pid=$!

"$codec_bin" record --archive "$test_dir/live.coda" \
  --feed "LABEL1=$test_dir/live-a.fifo" \
  --feed "LABEL2=$test_dir/live-b.fifo" \
  > "$test_dir/live-record.json" 2> "$test_dir/live-record.err" &
record_pid=$!

for _ in $(seq 1 200); do
  if [ -e "$test_dir/a-written" ] && [ -e "$test_dir/b-written" ] &&
     [ -e "$test_dir/live.coda" ]; then
    break
  fi
  sleep 0.01
done
[ -e "$test_dir/a-written" ]
[ -e "$test_dir/b-written" ]

"$codec_bin" extract "$test_dir/live.coda" --feed LABEL1 \
  --follow --output "$test_dir/live-alpha.bin" \
  > "$test_dir/live-follow.json" 2> "$test_dir/live-follow.err" &
follow_pid=$!

saw_alpha=0
for _ in $(seq 1 100); do
  if [ -e "$test_dir/live-alpha.bin" ] &&
     grep -q 'alpha-live' "$test_dir/live-alpha.bin"; then
    saw_alpha=1
    break
  fi
  if ! kill -0 "$follow_pid" 2>/dev/null; then
    break
  fi
  sleep 0.01
done
if [ "$saw_alpha" -ne 1 ]; then
  echo "follow extraction did not emit LABEL1 before archive finalization" >&2
  cat "$test_dir/live-follow.err" >&2 || true
  exit 1
fi
if grep -q 'beta-live' "$test_dir/live-alpha.bin"; then
  echo "follow extraction emitted bytes from LABEL2" >&2
  exit 1
fi

# The follower must still be waiting because both producer FIFOs remain open.
kill -0 "$follow_pid"
touch "$test_dir/release-live"
wait "$feed_a_pid"; feed_a_pid=
wait "$feed_b_pid"; feed_b_pid=
wait "$record_pid"; record_pid=
wait "$follow_pid"; follow_pid=
grep -q '"fidelity":"source_exact"' "$test_dir/live-follow.json"
grep -q '"follow":true' "$test_dir/live-follow.json"
printf 'alpha-live\n' > "$test_dir/expected-alpha.bin"
cmp "$test_dir/expected-alpha.bin" "$test_dir/live-alpha.bin"

cp "$test_dir/session.coda" "$test_dir/damaged.coda"
python3 - "$test_dir/damaged.coda" <<'PY'
import os, sys
path = sys.argv[1]
os.truncate(path, os.path.getsize(path) - 20)
PY
"$codec_bin" repair "$test_dir/damaged.coda" --output "$test_dir/repaired.coda"
"$codec_bin" verify "$test_dir/repaired.coda" --level full

expect_status_2 "$test_dir/watermark.stdout" "$test_dir/watermark.stderr" \
  "$codec_bin" watermark keygen --private "$test_dir/issuer.key" \
  --public "$test_dir/issuer.pub"
[ ! -e "$test_dir/issuer.key" ]
[ ! -e "$test_dir/issuer.pub" ]

"$codec_bin" --help > "$test_dir/help.txt"
if grep -Eq 'codec watermark|codec list streams|extract .*--stream' \
    "$test_dir/help.txt"; then
  echo "help still advertises a retired CLI surface" >&2
  exit 1
fi
if grep -Eq 'w0_ed25519|w1_reference|w2_reference|w2_policy' \
    "$test_dir/capabilities.json"; then
  echo "capabilities still advertise watermarking" >&2
  exit 1
fi

bash -x "$script_dir/video_cli_integration.sh" "$codec_bin"
bash "$script_dir/video_cli_concurrency.sh" "$codec_bin"
bash "$script_dir/video_cli_audio_grouped.sh" "$codec_bin"
bash "$script_dir/video_cli_audio_export_all.sh" "$codec_bin"
bash "$script_dir/video_cli_export_all.sh" "$codec_bin"

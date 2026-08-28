#!/usr/bin/env bash
set -euo pipefail

codec_bin=${1:?codec binary path required}
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

printf 'internet audio source bytes\n' > "$test_dir/input.bin"
"$codec_bin" capabilities > "$test_dir/capabilities.json"
grep -q '"neural_separation":false' "$test_dir/capabilities.json"

"$codec_bin" record --archive "$test_dir/session.coda" \
  --feed "news=$test_dir/input.bin"
"$codec_bin" verify "$test_dir/session.coda" --level full
"$codec_bin" list feeds "$test_dir/session.coda" > "$test_dir/feeds.jsonl"
grep -q '"label":"news"' "$test_dir/feeds.jsonl"
"$codec_bin" extract "$test_dir/session.coda" --feed news \
  --fidelity source-exact --output "$test_dir/extracted.bin"
cmp "$test_dir/input.bin" "$test_dir/extracted.bin"

if "$codec_bin" extract "$test_dir/session.coda" \
    --stream 00000000-0000-0000-0000-000000000001 \
    --fidelity source-exact --output "$test_dir/missing.bin" \
    > "$test_dir/missing.stdout" 2> "$test_dir/missing.stderr"; then
  echo "missing stream ID unexpectedly succeeded" >&2
  exit 1
fi
grep -q 'stream ID not found' "$test_dir/missing.stderr"

"$codec_bin" list streams "$test_dir/session.coda" > "$test_dir/streams.jsonl"
grep -q '"label":"news"' "$test_dir/streams.jsonl"
stream_id=$(python3 - "$test_dir/streams.jsonl" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    print(json.loads(next(f))["stream_id"])
PY
)
"$codec_bin" extract "$test_dir/session.coda" --stream "$stream_id" \
  --fidelity source-exact --output "$test_dir/stream-extracted.bin"
cmp "$test_dir/input.bin" "$test_dir/stream-extracted.bin"
if "$codec_bin" extract "$test_dir/session.coda" --stream not-a-stream-id \
    --fidelity source-exact --output "$test_dir/invalid.bin" \
    > "$test_dir/invalid.stdout" 2> "$test_dir/invalid.stderr"; then
  echo "malformed stream ID unexpectedly succeeded" >&2
  exit 1
fi
grep -q 'invalid stream ID' "$test_dir/invalid.stderr"

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
  --fidelity source-exact --follow --output "$test_dir/live-alpha.bin" \
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

python3 - "$test_dir/input.wav" <<'PY'
import math, struct, sys, wave
with wave.open(sys.argv[1], "wb") as f:
    f.setnchannels(1)
    f.setsampwidth(2)
    f.setframerate(48000)
    samples = [int(1000 * math.sin(2 * math.pi * 440 * i / 48000))
               for i in range(48000 * 3)]
    f.writeframes(b"".join(struct.pack("<h", x) for x in samples))
PY

"$codec_bin" watermark keygen --private "$test_dir/issuer.key" \
  --public "$test_dir/issuer.pub"
"$codec_bin" watermark issue "$test_dir/input.wav" \
  --output "$test_dir/marked.wav" --statement "$test_dir/feed.cose" \
  --private-key "$test_dir/issuer.key" \
  --feed-uuid 7c2b2f74-7e31-4a1d-b469-d88d63fc8fcb \
  --code 0x4a31 --issuer integration --key-id integration-1 \
  --issued-at 1000 --not-before 1000 --expires-at 2000 --w1
"$codec_bin" watermark detect "$test_dir/marked.wav" \
  --statement "$test_dir/feed.cose" --public-key "$test_dir/issuer.pub" \
  --at 1500 --format jsonl > "$test_dir/events.jsonl"
grep -q '"state":"signature_bound_candidate"' "$test_dir/events.jsonl"
grep -q '"authoritative":false' "$test_dir/events.jsonl"

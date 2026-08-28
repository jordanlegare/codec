#!/usr/bin/env bash
set -euo pipefail

codec_bin=${1:?codec binary path required}
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT

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

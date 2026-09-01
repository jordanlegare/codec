# AAC leading-trim MP4 passthrough fix

## Scope

Fix H.1 MP4 export for valid EAP1 AAC states whose decoded audio begins before the selected video origin. Preserve the stored AAC packet bytes; do not decode/re-encode audio.

## Root cause

`finalize_ffmpeg_encoded_audio_capture()` records a positive `trim_start_frames` when the first decoded AAC timestamp precedes the video origin. The corresponding EAP1 packets may legitimately have negative PTS/DTS offsets because the first AAC access unit overlaps presentation time zero. Both encoded-audio MP4 mux paths currently reject nonzero trim and negative packet offsets before FFmpeg can represent the preroll.

FFmpeg's MOV/MP4 muxer supports audio preroll through negative timestamps plus an edit list. The fix therefore belongs at the MP4 mux boundary, not in ingest or by dropping/re-encoding AAC packets.

## Proof

1. RED: real AAC fixture -> EAP1 -> synthetic 10 ms leading trim -> verified export currently fails with `model_incompatible`.
2. GREEN: preserve negative preroll packet timestamps, force MP4 edit-list handling when trim is present, and keep packet bytes unchanged.
3. Cover both AAC-with-ASC and ADTS-to-ASC paths.
4. Keep existing zero-trim passthrough, VFR1/PCM16 compatibility, FFmpeg-disabled behavior, and archive truth semantics unchanged.
5. Run GCC, Clang, sanitizers, FFmpeg-disabled, CLI, install, and installed-package consumer CI on the exact final SHA.

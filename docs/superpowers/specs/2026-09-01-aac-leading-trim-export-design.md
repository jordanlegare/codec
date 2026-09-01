# H.1 AAC leading-trim MP4 representation

## Problem

Valid EAP1 AAC can begin before the selected video presentation origin. H.1 records that overlap as `trim_start_frames` and negative packet timestamp offsets. The current exporter rejects this representable state before mux.

## Representation

For MP4 export, preserve all stored AAC packet payloads. When `trim_start_frames > 0`, retain negative preroll PTS/DTS for packets overlapping presentation time zero and require the MP4 muxer to use an edit list. The edit list excludes preroll from presentation without changing compressed AAC bytes.

No ingest semantics, EAP1 wire layout, source truth, or archive record codes change.

## Validation

A negative packet offset is accepted only for an EAP1 state that has nonzero leading trim and only when the packet overlaps presentation time zero. Packets still must satisfy EAP1 ordering/coverage rules. Timestamp addition must remain overflow-safe.

## AAC configuration paths

- Existing AudioSpecificConfig: write stored AAC packets directly with preroll timing.
- Missing config / ADTS: run `aac_adtstoasc` exactly as today, preserving filtered AAC payload semantics while carrying preroll timing into MP4.

## Compatibility

Zero-trim EAP1 behavior remains unchanged. PCM16 fallback and legacy VFR1 export are unchanged. FFmpeg-disabled builds continue returning explicit backend unavailability.

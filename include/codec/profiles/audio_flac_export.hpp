#pragma once

#include <codec/processing.hpp>
#include <codec/profiles/audio_state_reader.hpp>

#include <cstdint>
#include <vector>

namespace codec::profiles::audio {

struct Pcm16FlacExportLimits {
  std::uint64_t maximum_output_bytes{64ULL * 1024ULL * 1024ULL};
};

struct VerifiedPcm16FlacExport {
  ExportResult output;
  RecordInfo state_record;
  RecordInfo source_record;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedPcm16FlacExport>> export_verified_pcm16_flac(
    const CodaArchive& archive, const Pcm16StateQuery& query = {},
    Pcm16FlacExportLimits limits = {});

}  // namespace codec::profiles::audio

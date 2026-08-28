#pragma once

#include <codec/processing.hpp>
#include <codec/profiles/audio_state_reader.hpp>

#include <cstdint>
#include <vector>

namespace codec::profiles::audio {

struct Pcm16WavExportLimits {
  std::uint64_t maximum_output_bytes{64ULL * 1024ULL * 1024ULL};
};

struct VerifiedPcm16WavExport {
  ExportResult output;
  RecordInfo state_record;
  RecordInfo source_record;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedPcm16WavExport>> export_verified_pcm16_wav(
    const CodaArchive& archive, const Pcm16StateQuery& query = {},
    Pcm16WavExportLimits limits = {});

}  // namespace codec::profiles::audio

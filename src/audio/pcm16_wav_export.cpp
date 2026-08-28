#include <codec/profiles/audio_export.hpp>

#include "wav_codec.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace codec::profiles::audio {
namespace {

class Pcm16WavExporter final : public StreamExporter {
 public:
  std::string name() const override { return "audio-pcm16-wav"; }

  Result<ExporterOutput> export_records(
      std::span<const ExtractedRecord> inputs) override {
    if (inputs.size() != 1 ||
        inputs.front().record.type != RecordType::pcm16) {
      return Error{ErrorCode::invalid_argument,
                   "PCM16 WAV export requires exactly one APS1 pcm16 record",
                   false};
    }

    auto state = decode_pcm16_state(inputs.front().payload);
    if (!state) return state.error();
    auto wav = detail::encode_wav_pcm16(state->sample_rate, state->channels,
                                        state->samples);
    if (!wav) return wav.error();
    return ExporterOutput{
        .payload_type = "audio/wav",
        .payload = std::move(*wav),
    };
  }
};

}  // namespace

Result<std::vector<VerifiedPcm16WavExport>> export_verified_pcm16_wav(
    const CodaArchive& archive, const Pcm16StateQuery& query,
    Pcm16WavExportLimits limits) {
  if (limits.maximum_output_bytes == 0) {
    return fail<std::vector<VerifiedPcm16WavExport>>(
        ErrorCode::invalid_argument,
        "PCM16 WAV export output limit must be non-zero");
  }

  auto verified_states = query_verified_pcm16_states(archive, query);
  if (!verified_states) return verified_states.error();

  std::uint64_t total_output_bytes = 0;
  for (const auto& verified : *verified_states) {
    auto encoded_size = detail::encoded_wav_pcm16_size(
        verified.state.sample_rate, verified.state.channels,
        verified.state.samples.size());
    if (!encoded_size) return encoded_size.error();

    const auto next_bytes = static_cast<std::uint64_t>(*encoded_size);
    if (total_output_bytes > limits.maximum_output_bytes ||
        next_bytes > limits.maximum_output_bytes - total_output_bytes) {
      return fail<std::vector<VerifiedPcm16WavExport>>(
          ErrorCode::resource_exhausted,
          "verified PCM16 WAV exports exceed the configured output limit");
    }
    total_output_bytes += next_bytes;
  }

  std::vector<VerifiedPcm16WavExport> output;
  output.reserve(verified_states->size());
  for (auto& verified : *verified_states) {
    auto payload = archive.read_payload(verified.state_record);
    if (!payload) return payload.error();

    const std::array inputs{ExtractedRecord{
        .record = verified.state_record,
        .payload = std::move(*payload),
    }};
    Pcm16WavExporter exporter;
    auto exported = invoke_exporter(
        exporter, inputs,
        ExporterRunLimits{
            .maximum_inputs = 1,
            .maximum_input_bytes = query.maximum_encoded_bytes,
            .maximum_output_bytes = limits.maximum_output_bytes,
            .maximum_payload_type_bytes = 32,
        });
    if (!exported) return exported.error();

    output.push_back(VerifiedPcm16WavExport{
        .output = std::move(*exported),
        .state_record = verified.state_record,
        .source_record = verified.source_record,
        .provenance = std::move(verified.provenance),
    });
  }
  return output;
}

}  // namespace codec::profiles::audio

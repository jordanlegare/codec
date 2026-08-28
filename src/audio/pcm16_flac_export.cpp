#include <codec/profiles/audio_flac_export.hpp>

#include "flac_encoder.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace codec::profiles::audio {
namespace {

class Pcm16FlacExporter final : public StreamExporter {
 public:
  explicit Pcm16FlacExporter(std::uint64_t maximum_output_bytes)
      : maximum_output_bytes_(maximum_output_bytes) {}

  std::string name() const override { return "audio-pcm16-flac"; }

  Result<ExporterOutput> export_records(
      std::span<const ExtractedRecord> inputs) override {
    if (inputs.size() != 1 ||
        inputs.front().record.type != RecordType::pcm16) {
      return Error{ErrorCode::invalid_argument,
                   "PCM16 FLAC export requires exactly one APS1 pcm16 record",
                   false};
    }

    auto state = decode_pcm16_state(inputs.front().payload);
    if (!state) return state.error();
    auto flac = detail::encode_flac_pcm16(*state, maximum_output_bytes_);
    if (!flac) return flac.error();
    return ExporterOutput{
        .payload_type = "audio/flac",
        .payload = std::move(*flac),
    };
  }

 private:
  std::uint64_t maximum_output_bytes_{};
};

}  // namespace

Result<std::vector<VerifiedPcm16FlacExport>> export_verified_pcm16_flac(
    const CodaArchive& archive, const Pcm16StateQuery& query,
    Pcm16FlacExportLimits limits) {
  if (limits.maximum_output_bytes == 0) {
    return fail<std::vector<VerifiedPcm16FlacExport>>(
        ErrorCode::invalid_argument,
        "PCM16 FLAC export output limit must be non-zero");
  }

  auto verified_states = query_verified_pcm16_states(archive, query);
  if (!verified_states) return verified_states.error();

  std::uint64_t total_output_bytes = 0;
  std::vector<VerifiedPcm16FlacExport> output;
  output.reserve(verified_states->size());
  for (auto& verified : *verified_states) {
    if (total_output_bytes >= limits.maximum_output_bytes) {
      return fail<std::vector<VerifiedPcm16FlacExport>>(
          ErrorCode::resource_exhausted,
          "verified PCM16 FLAC exports exceed the configured output limit");
    }
    const auto remaining = limits.maximum_output_bytes - total_output_bytes;

    auto payload = archive.read_payload(verified.state_record);
    if (!payload) return payload.error();

    const std::array inputs{ExtractedRecord{
        .record = verified.state_record,
        .payload = std::move(*payload),
    }};
    Pcm16FlacExporter exporter{remaining};
    auto exported = invoke_exporter(
        exporter, inputs,
        ExporterRunLimits{
            .maximum_inputs = 1,
            .maximum_input_bytes = query.maximum_encoded_bytes,
            .maximum_output_bytes = remaining,
            .maximum_payload_type_bytes = 32,
        });
    if (!exported) return exported.error();

    const auto output_bytes =
        static_cast<std::uint64_t>(exported->payload.size());
    if (output_bytes > remaining) {
      return fail<std::vector<VerifiedPcm16FlacExport>>(
          ErrorCode::resource_exhausted,
          "verified PCM16 FLAC exports exceed the configured output limit");
    }
    total_output_bytes += output_bytes;

    output.push_back(VerifiedPcm16FlacExport{
        .output = std::move(*exported),
        .state_record = verified.state_record,
        .source_record = verified.source_record,
        .provenance = std::move(verified.provenance),
    });
  }
  return output;
}

}  // namespace codec::profiles::audio

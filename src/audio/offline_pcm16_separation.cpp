#include <codec/profiles/audio_offline_separation.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codec::profiles::audio {
namespace {

constexpr std::size_t maximum_source_ceiling = 64;
constexpr std::size_t maximum_identity_bytes = 2048;
constexpr std::size_t maximum_model_bundle_bytes = 4096;
constexpr std::size_t maximum_provenance_text_bytes = 4096;

bool contains_nul(std::string_view value) {
  return value.find('\0') != std::string_view::npos;
}

std::optional<std::uint8_t> hex_nibble(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<std::uint8_t>(10 + value - 'a');
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<std::uint8_t>(10 + value - 'A');
  }
  return std::nullopt;
}

Result<Sha256> parse_model_hash(std::string_view hex) {
  if (hex.size() != Sha256{}.size() * 2) {
    return fail<Sha256>(ErrorCode::inference,
                        "separation backend model hash is not SHA-256");
  }
  Sha256 output{};
  for (std::size_t index = 0; index < output.size(); ++index) {
    const auto high = hex_nibble(hex[index * 2]);
    const auto low = hex_nibble(hex[index * 2 + 1]);
    if (!high || !low) {
      return fail<Sha256>(ErrorCode::inference,
                          "separation backend model hash is not hexadecimal");
    }
    output[index] = static_cast<std::uint8_t>((*high << 4U) | *low);
  }
  return output;
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void put_u64(std::vector<std::byte>& output, std::size_t offset,
             std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    output[offset + shift / 8] =
        static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

Sha256 request_configuration_hash(
    const OfflinePcm16SeparationRequest& request) {
  std::vector<std::byte> encoded{
      std::byte{'D'}, std::byte{'7'}, std::byte{'C'}, std::byte{'1'}};
  append_u64(encoded, static_cast<std::uint64_t>(request.maximum_sources));
  append_u64(encoded, static_cast<std::uint64_t>(request.model_bundle.size()));
  for (const unsigned char value : request.model_bundle) {
    encoded.push_back(static_cast<std::byte>(value));
  }
  return sha256(encoded);
}

MixtureReconstructionMetrics reconstruction_metrics(
    const WavPcm16& mixture, std::span<const WavPcm16> stems,
    const WavPcm16& residual, double backend_reported_error) {
  std::uint64_t maximum_error = 0;
  long double squared_error = 0.0L;
  for (std::size_t sample = 0; sample < mixture.samples.size(); ++sample) {
    std::int64_t reconstructed = residual.samples[sample];
    for (const auto& stem : stems) {
      reconstructed += stem.samples[sample];
    }
    const auto error = static_cast<std::int64_t>(mixture.samples[sample]) -
                       reconstructed;
    const auto absolute = error < 0
                              ? static_cast<std::uint64_t>(-(error + 1)) + 1
                              : static_cast<std::uint64_t>(error);
    if (absolute > maximum_error) maximum_error = absolute;
    const auto wide_error = static_cast<long double>(error);
    squared_error += wide_error * wide_error;
  }
  const auto rms = mixture.samples.empty()
                       ? 0.0
                       : static_cast<double>(std::sqrt(
                             squared_error / mixture.samples.size()));
  return MixtureReconstructionMetrics{
      .maximum_absolute_sample_error = maximum_error,
      .root_mean_square_sample_error = rms,
      .backend_reported_error = backend_reported_error,
  };
}

std::vector<std::byte> process_details(
    OfflinePcm16ArtifactRole role, std::size_t stem_index,
    const MixtureReconstructionMetrics& metrics) {
  std::vector<std::byte> output(40);
  output[0] = std::byte{'A'};
  output[1] = std::byte{'O'};
  output[2] = std::byte{'S'};
  output[3] = std::byte{'1'};
  output[4] = static_cast<std::byte>(role);
  put_u64(output, 8, static_cast<std::uint64_t>(stem_index));
  put_u64(output, 16, metrics.maximum_absolute_sample_error);
  put_u64(output, 24,
          std::bit_cast<std::uint64_t>(
              metrics.root_mean_square_sample_error));
  put_u64(output, 32,
          std::bit_cast<std::uint64_t>(metrics.backend_reported_error));
  return output;
}

ProvenanceRecordLink exact_link(const RecordInfo& record) {
  return ProvenanceRecordLink{
      .stream = record.stream,
      .type = record.type_code(),
      .sequence = record.sequence,
      .hash = record.hash,
  };
}

bool matching_geometry(const WavPcm16& value, const WavPcm16& mixture) {
  return value.sample_rate == mixture.sample_rate &&
         value.channels == mixture.channels &&
         value.samples.size() == mixture.samples.size();
}

struct ProcessedMetadata {
  std::size_t stem_count{};
  MixtureReconstructionMetrics reconstruction;
  std::string provider;
  Sha256 model_hash{};
  Sha256 configuration_hash{};
};

class OfflineSeparationProcessor final : public StreamProcessor {
 public:
  OfflineSeparationProcessor(SeparationBackend& backend,
                             const OfflinePcm16SeparationRequest& request,
                             std::string backend_name,
                             Sha256 configuration_hash)
      : backend_(backend),
        request_(request),
        backend_name_(std::move(backend_name)),
        configuration_hash_(configuration_hash) {}

  std::string name() const override { return "audio-offline-separation"; }

  Result<std::vector<ProcessorOutput>> process(
      std::span<const ExtractedRecord> inputs) override {
    if (inputs.size() != 1) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::invalid_argument,
          "offline PCM16 separation requires one exact S1 input");
    }
    auto state = decode_pcm16_state(inputs.front().payload);
    if (!state) return state.error();
    const WavPcm16 mixture{
        .sample_rate = state->sample_rate,
        .channels = state->channels,
        .samples = state->samples,
    };
    auto separated = backend_.separate(SeparationRequest{
        .mixture = mixture,
        .maximum_sources = request_.maximum_sources,
        .model_bundle = request_.model_bundle,
    });
    if (!separated) return separated.error();

    if (separated->stems.empty()) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::inference,
          "separation backend returned no derived stems");
    }
    if (separated->stems.size() > request_.maximum_sources) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::inference,
          "separation backend exceeded the requested source count");
    }
    for (const auto& stem : separated->stems) {
      if (!matching_geometry(stem, mixture)) {
        return fail<std::vector<ProcessorOutput>>(
            ErrorCode::inference,
            "separation backend stem geometry does not match the mixture");
      }
    }
    if (!matching_geometry(separated->residual, mixture)) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::inference,
          "separation backend residual geometry does not match the mixture");
    }
    if (separated->provider.empty() ||
        separated->provider.size() > maximum_identity_bytes ||
        contains_nul(separated->provider)) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::inference,
          "separation backend provider identity is invalid");
    }
    if (!std::isfinite(separated->mixture_reconstruction_error) ||
        separated->mixture_reconstruction_error < 0.0) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::inference,
          "separation backend reconstruction metric is invalid");
    }
    auto model_hash = parse_model_hash(separated->model_hash);
    if (!model_hash) return model_hash.error();
    const auto implementation_id = backend_name_ + "/" + separated->provider;
    if (implementation_id.size() > maximum_provenance_text_bytes) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::inference,
          "separation implementation identity is too large");
    }

    const auto metrics = reconstruction_metrics(
        mixture, separated->stems, separated->residual,
        separated->mixture_reconstruction_error);
    const auto make_output = [&](const WavPcm16& value,
                                 OfflinePcm16ArtifactRole role,
                                 std::size_t stem_index)
        -> Result<ProcessorOutput> {
      auto encoded = encode_pcm16_state(Pcm16State{
          .sample_rate = value.sample_rate,
          .channels = value.channels,
          .samples = value.samples,
      });
      if (!encoded) {
        return fail<ProcessorOutput>(
            ErrorCode::inference,
            "separation backend returned invalid PCM16 output");
      }
      return ProcessorOutput{
          .stream = inputs.front().record.stream,
          .type = record_type_code(RecordType::pcm16),
          .start_ns = inputs.front().record.start_ns,
          .end_ns = inputs.front().record.end_ns,
          .truth = TruthClass::derived,
          .payload = std::move(*encoded),
          .process = ProvenanceProcess{
              .operation = "audio.offline-separation",
              .implementation_id = implementation_id,
              .implementation_version = "1",
              .implementation_hash = *model_hash,
              .configuration_hash = configuration_hash_,
              .created_utc_ns = request_.created_utc_ns,
              .details_type =
                  "codec.audio.offline-separation-output.v1",
              .details = process_details(role, stem_index, metrics),
          },
      };
    };

    std::vector<ProcessorOutput> outputs;
    outputs.reserve(separated->stems.size() + 1);
    for (std::size_t index = 0; index < separated->stems.size(); ++index) {
      auto output = make_output(separated->stems[index],
                                OfflinePcm16ArtifactRole::stem, index);
      if (!output) return output.error();
      outputs.push_back(std::move(*output));
    }
    auto residual = make_output(separated->residual,
                                OfflinePcm16ArtifactRole::residual, 0);
    if (!residual) return residual.error();
    outputs.push_back(std::move(*residual));

    metadata_ = ProcessedMetadata{
        .stem_count = separated->stems.size(),
        .reconstruction = metrics,
        .provider = separated->provider,
        .model_hash = *model_hash,
        .configuration_hash = configuration_hash_,
    };
    return outputs;
  }

  const ProcessedMetadata& metadata() const { return *metadata_; }

 private:
  SeparationBackend& backend_;
  const OfflinePcm16SeparationRequest& request_;
  std::string backend_name_;
  Sha256 configuration_hash_{};
  std::optional<ProcessedMetadata> metadata_;
};

Result<void> validate_request(const OfflinePcm16SeparationRequest& request,
                              const SeparationBackend& backend,
                              std::string_view backend_name) {
  if (request.states.maximum_results == 0 ||
      request.states.maximum_encoded_bytes == 0) {
    return fail(ErrorCode::invalid_argument,
                "offline PCM16 input limits must be non-zero");
  }
  if (!request.states.time ||
      request.states.time->begin_ns >= request.states.time->end_ns) {
    return fail(ErrorCode::invalid_argument,
                "offline PCM16 separation requires an explicit interval");
  }
  if (request.maximum_sources == 0 ||
      request.maximum_sources > maximum_source_ceiling) {
    return fail(ErrorCode::invalid_argument,
                "offline PCM16 source count is outside the profile bound");
  }
  if (request.limits.maximum_output_bytes == 0) {
    return fail(ErrorCode::invalid_argument,
                "offline PCM16 output limit must be non-zero");
  }
  if (request.model_bundle.empty() ||
      request.model_bundle.size() > maximum_model_bundle_bytes ||
      contains_nul(request.model_bundle)) {
    return fail(ErrorCode::invalid_argument,
                "offline PCM16 model bundle reference is invalid");
  }
  if (backend_name.empty() || backend_name.size() > maximum_identity_bytes ||
      contains_nul(backend_name)) {
    return fail(ErrorCode::invalid_argument,
                "offline PCM16 backend identity is invalid");
  }
  if (!backend.available()) {
    return fail(ErrorCode::model_incompatible,
                "no compatible separation backend is available");
  }
  return {};
}

}  // namespace

Result<std::vector<OfflinePcm16Separation>>
separate_verified_pcm16_offline(
    const CodaArchive& archive, SeparationBackend& backend,
    const OfflinePcm16SeparationRequest& request) {
  const auto backend_name = backend.name();
  auto valid = validate_request(request, backend, backend_name);
  if (!valid) return valid.error();

  auto states = query_verified_pcm16_states(archive, request.states);
  if (!states) return states.error();
  const auto configuration_hash = request_configuration_hash(request);
  auto remaining_bytes = request.limits.maximum_output_bytes;
  std::vector<OfflinePcm16Separation> results;
  results.reserve(states->size());

  for (auto& verified : *states) {
    if (remaining_bytes == 0) {
      return fail<std::vector<OfflinePcm16Separation>>(
          ErrorCode::resource_exhausted,
          "offline PCM16 outputs exhausted the aggregate byte limit");
    }
    auto payload = archive.read_payload(verified.state_record);
    if (!payload) return payload.error();
    const std::vector<ExtractedRecord> inputs{
        ExtractedRecord{.record = verified.state_record,
                        .payload = std::move(*payload)}};
    OfflineSeparationProcessor processor{backend, request, backend_name,
                                         configuration_hash};
    auto outputs = invoke_processor(
        processor, inputs,
        ProcessorRunLimits{
            .maximum_outputs = request.maximum_sources + 1,
            .maximum_output_bytes = remaining_bytes,
        });
    if (!outputs) return outputs.error();
    const auto& metadata = processor.metadata();
    if (outputs->size() != metadata.stem_count + 1) {
      return fail<std::vector<OfflinePcm16Separation>>(
          ErrorCode::internal,
          "offline separation processor returned inconsistent metadata");
    }

    std::uint64_t run_bytes = 0;
    for (const auto& output : *outputs) {
      const auto bytes = static_cast<std::uint64_t>(output.payload.size());
      if (bytes > remaining_bytes - run_bytes) {
        return fail<std::vector<OfflinePcm16Separation>>(
            ErrorCode::resource_exhausted,
            "offline PCM16 outputs exceed the aggregate byte limit");
      }
      run_bytes += bytes;
    }
    remaining_bytes -= run_bytes;

    const auto support = exact_link(verified.state_record);
    std::vector<OfflinePcm16Artifact> stems;
    stems.reserve(metadata.stem_count);
    for (std::size_t index = 0; index < metadata.stem_count; ++index) {
      auto output = std::move(outputs->at(index));
      auto state = decode_pcm16_state(output.payload);
      if (!state) return state.error();
      stems.push_back(OfflinePcm16Artifact{
          .role = OfflinePcm16ArtifactRole::stem,
          .stem_index = index,
          .state = std::move(*state),
          .output = std::move(output),
          .supporting_state = support,
      });
    }
    auto residual_output = std::move(outputs->back());
    auto residual_state = decode_pcm16_state(residual_output.payload);
    if (!residual_state) return residual_state.error();
    OfflinePcm16Artifact residual{
        .role = OfflinePcm16ArtifactRole::residual,
        .stem_index = 0,
        .state = std::move(*residual_state),
        .output = std::move(residual_output),
        .supporting_state = support,
    };

    results.push_back(OfflinePcm16Separation{
        .input = std::move(verified),
        .stems = std::move(stems),
        .residual = std::move(residual),
        .reconstruction = metadata.reconstruction,
        .backend = backend_name,
        .provider = metadata.provider,
        .model_hash = metadata.model_hash,
        .configuration_hash = metadata.configuration_hash,
    });
  }
  return results;
}

}  // namespace codec::profiles::audio

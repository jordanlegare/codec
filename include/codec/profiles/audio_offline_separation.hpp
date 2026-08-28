#pragma once

#include <codec/inference.hpp>
#include <codec/integrity.hpp>
#include <codec/processing.hpp>
#include <codec/profiles/audio_state_reader.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace codec::profiles::audio {

struct OfflinePcm16SeparationLimits {
  std::uint64_t maximum_output_bytes{256ULL * 1024ULL * 1024ULL};
};

struct MixtureReconstructionMetrics {
  std::uint64_t maximum_absolute_sample_error{};
  double root_mean_square_sample_error{};
  double backend_reported_error{};
};

enum class OfflinePcm16ArtifactRole : std::uint8_t {
  stem = 1,
  residual = 2,
};

struct OfflinePcm16Artifact {
  OfflinePcm16ArtifactRole role{OfflinePcm16ArtifactRole::stem};
  std::size_t stem_index{};
  Pcm16State state;
  ProcessorOutput output;
  ProvenanceRecordLink supporting_state;
};

struct OfflinePcm16Separation {
  VerifiedPcm16State input;
  std::vector<OfflinePcm16Artifact> stems;
  OfflinePcm16Artifact residual;
  MixtureReconstructionMetrics reconstruction;
  std::string backend;
  std::string provider;
  Sha256 model_hash{};
  Sha256 configuration_hash{};
};

struct OfflinePcm16SeparationRequest {
  Pcm16StateQuery states;
  std::size_t maximum_sources{8};
  std::string model_bundle;
  std::int64_t created_utc_ns{};
  OfflinePcm16SeparationLimits limits{};
};

Result<std::vector<OfflinePcm16Separation>>
separate_verified_pcm16_offline(
    const CodaArchive& archive, SeparationBackend& backend,
    const OfflinePcm16SeparationRequest& request);

}  // namespace codec::profiles::audio

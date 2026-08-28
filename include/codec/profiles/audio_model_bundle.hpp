#pragma once

#include <codec/integrity.hpp>
#include <codec/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace codec::profiles::audio {

struct SeparationModelManifest {
  std::string model_id;
  std::string model_version;
  std::string license_id;
  std::string quality_domain;
  std::uint32_t input_sample_rate{};
  std::uint16_t input_channels{};
  std::uint32_t window_samples{};
  std::uint32_t hop_samples{};
  std::uint32_t lookahead_samples{};
  std::uint16_t maximum_sources{};
  bool causal{};
  std::string input_tensor_name;
  std::string output_tensor_name;
};

struct SeparationModelBundle {
  SeparationModelManifest manifest;
  std::vector<std::byte> onnx_model;
};

struct VerifiedSeparationModelBundle {
  SeparationModelManifest manifest;
  std::vector<std::byte> onnx_model;
  Sha256 model_hash{};
  Sha256 bundle_hash{};
};

struct SeparationModelBundleLimits {
  std::uint64_t maximum_bundle_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_model_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint16_t maximum_text_bytes{1024};
};

Result<std::vector<std::byte>> encode_separation_model_bundle(
    const SeparationModelBundle& bundle,
    const SeparationModelBundleLimits& limits = {});

Result<VerifiedSeparationModelBundle> decode_separation_model_bundle(
    std::span<const std::byte> encoded,
    const SeparationModelBundleLimits& limits = {});

}  // namespace codec::profiles::audio

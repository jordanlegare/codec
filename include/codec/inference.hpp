#pragma once

#include <codec/audio.hpp>
#include <codec/result.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace codec {

struct SeparationRequest {
  WavPcm16 mixture;
  std::size_t maximum_sources{8};
  std::string model_bundle;
};

struct SeparationResult {
  std::vector<WavPcm16> stems;
  WavPcm16 residual;
  double mixture_reconstruction_error{};
  std::string model_hash;
  std::string provider;
};

class SeparationBackend {
 public:
  virtual ~SeparationBackend() = default;
  virtual std::string name() const = 0;
  virtual bool available() const noexcept = 0;
  virtual Result<SeparationResult> separate(
      const SeparationRequest& request) = 0;
};

std::unique_ptr<SeparationBackend> default_separation_backend();

}  // namespace codec

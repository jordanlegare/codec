#pragma once

#include <codec/inference.hpp>
#include <codec/profiles/audio_model_bundle.hpp>
#include <codec/result.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace codec::profiles::audio {

struct OnnxCpuSeparationLimits {
  std::uint64_t maximum_input_frames{28'800'000};
  std::uint64_t maximum_output_samples{134'217'728};
  std::uint64_t maximum_windows{1'000'000};
};

struct OnnxCpuSeparationOptions {
  std::string runtime_library;
  std::uint32_t intra_op_threads{1};
  std::uint32_t inter_op_threads{1};
  OnnxCpuSeparationLimits limits{};
};

bool onnx_cpu_separation_runtime_compiled() noexcept;

Result<std::unique_ptr<SeparationBackend>>
create_onnx_cpu_separation_backend(
    const VerifiedSeparationModelBundle& bundle,
    const OnnxCpuSeparationOptions& options = {});

}  // namespace codec::profiles::audio

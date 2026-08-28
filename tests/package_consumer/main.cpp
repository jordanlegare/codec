#include <codec/profiles/audio.hpp>

#include <cstddef>
#include <cstdint>

int main() {
  const codec::profiles::audio::Pcm16FlacExportLimits limits{};
  const codec::profiles::audio::OfflinePcm16SeparationRequest offline{};
  const codec::profiles::audio::OfflinePcm16SeparationLimits offline_limits{};
  const codec::profiles::audio::SeparationModelBundle model_bundle{
      .manifest = {
          .model_id = "consumer.separator",
          .model_version = "1",
          .license_id = "Apache-2.0",
          .quality_domain = "test",
          .input_sample_rate = 48000,
          .input_channels = 2,
          .window_samples = 8,
          .hop_samples = 4,
          .lookahead_samples = 0,
          .maximum_sources = 2,
          .causal = true,
          .input_tensor_name = "mixture",
          .output_tensor_name = "sources",
      },
      .onnx_model = {std::byte{0x08}},
  };
  const auto encoded_model_bundle =
      codec::profiles::audio::encode_separation_model_bundle(model_bundle);
  codec::Pcm16State state{
      .sample_rate = 48000,
      .channels = 2,
      .samples = {0, 0},
  };
  return limits.maximum_output_bytes == 0 ||
                 offline.maximum_sources == 0 ||
                 offline_limits.maximum_output_bytes == 0 ||
                 !encoded_model_bundle ||
                 state.frames() != 1
             ? 1
             : 0;
}

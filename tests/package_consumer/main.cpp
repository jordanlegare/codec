#include <codec/profiles/audio.hpp>
#include <codec/transport.hpp>

#include <cstddef>
#include <cstdint>

int main() {
  const codec::profiles::audio::Pcm16FlacExportLimits limits{};
  const codec::profiles::audio::OfflinePcm16SeparationRequest offline{};
  const codec::profiles::audio::OfflinePcm16SeparationLimits offline_limits{};
  const codec::profiles::audio::OnnxCpuSeparationOptions runtime_options{};
  const auto runtime_compiled =
      codec::profiles::audio::onnx_cpu_separation_runtime_compiled();
  (void)runtime_compiled;
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

  codec::MultiplexFrame multiplex;
  multiplex.stream = codec::derive_stream_id("package-consumer/mux");
  multiplex.clock.source_timebase_numerator = 1;
  multiplex.clock.source_timebase_denominator = 1;
  multiplex.payload = {std::byte{0x42}};
  auto encoded_multiplex = codec::encode_multiplex_frame(multiplex);
  if (!encoded_multiplex) return 1;
  codec::MultiplexDecoder decoder;
  auto decoded_multiplex = decoder.push(*encoded_multiplex);
  if (!decoded_multiplex || decoded_multiplex->size() != 1 ||
      decoded_multiplex->front().stream != multiplex.stream ||
      !decoder.finish()) {
    return 1;
  }

  return limits.maximum_output_bytes == 0 ||
                 offline.maximum_sources == 0 ||
                 offline_limits.maximum_output_bytes == 0 ||
                 runtime_options.intra_op_threads == 0 ||
                 runtime_options.limits.maximum_input_frames == 0 ||
                 !encoded_model_bundle ||
                 state.frames() != 1
             ? 1
             : 0;
}

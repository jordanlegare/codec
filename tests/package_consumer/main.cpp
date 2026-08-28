#include <codec/profiles/audio.hpp>

#include <cstdint>

int main() {
  const codec::profiles::audio::Pcm16FlacExportLimits limits{};
  const codec::profiles::audio::OfflinePcm16SeparationRequest offline{};
  const codec::profiles::audio::OfflinePcm16SeparationLimits offline_limits{};
  codec::Pcm16State state{
      .sample_rate = 48000,
      .channels = 2,
      .samples = {0, 0},
  };
  return limits.maximum_output_bytes == 0 ||
                 offline.maximum_sources == 0 ||
                 offline_limits.maximum_output_bytes == 0 ||
                 state.frames() != 1
             ? 1
             : 0;
}

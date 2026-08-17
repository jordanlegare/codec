#pragma once

#include <codec/audio.hpp>
#include <codec/result.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace codec {

enum class CarrierBand { w1, w2 };

const char* carrier_band_name(CarrierBand band) noexcept;

struct WatermarkPolicy {
  double amplitude_dbfs{-42.0};
  std::uint32_t bit_duration_ms{20};
  double w1_zero_hz{17500.0};
  double w1_one_hz{18500.0};
  double w2_zero_hz{26000.0};
  double w2_one_hz{28000.0};
  std::uint32_t w2_minimum_sample_rate{96000};
  double nyquist_guard_hz{2000.0};
  double minimum_confidence{0.30};
};

struct WatermarkEmbedReport {
  CarrierBand band{CarrierBand::w1};
  std::uint16_t code{};
  std::size_t frames_embedded{};
  double amplitude_dbfs{};
  bool path_qualified{};
};

struct WatermarkObservation {
  CarrierBand band{CarrierBand::w1};
  std::uint16_t code{};
  double confidence{};
  std::uint64_t start_frame{};
  std::uint64_t end_frame{};
  std::size_t corrected_preamble_bits{};
};

Result<WatermarkEmbedReport> embed_watermark(
    WavPcm16& audio, std::uint16_t code, CarrierBand band,
    const WatermarkPolicy& policy = {});

Result<std::vector<WatermarkObservation>> detect_watermarks(
    const WavPcm16& audio, const WatermarkPolicy& policy = {});

}  // namespace codec


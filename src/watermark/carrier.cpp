#include <codec/watermark.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>

namespace codec {
namespace {

constexpr std::uint16_t preamble = 0xd5a7U;
constexpr std::size_t message_bits = 40;

bool finite_policy(const WatermarkPolicy& policy) {
  return std::isfinite(policy.amplitude_dbfs) &&
         std::isfinite(policy.w1_zero_hz) &&
         std::isfinite(policy.w1_one_hz) &&
         std::isfinite(policy.w2_zero_hz) &&
         std::isfinite(policy.w2_one_hz) &&
         std::isfinite(policy.nyquist_guard_hz) &&
         std::isfinite(policy.minimum_confidence);
}

bool hard_band_invariants(const WatermarkPolicy& policy) {
  return finite_policy(policy) && policy.w1_zero_hz >= 300.0 &&
         policy.w1_zero_hz < policy.w1_one_hz &&
         policy.w1_one_hz <= 19000.0 && policy.w2_zero_hz >= 24000.0 &&
         policy.w2_zero_hz < policy.w2_one_hz &&
         policy.w2_one_hz <= 40000.0 &&
         policy.w2_minimum_sample_rate >= 96000U &&
         policy.nyquist_guard_hz >= 2000.0;
}

std::uint8_t crc8(std::uint16_t code) {
  std::uint8_t crc = 0;
  for (const std::uint8_t byte : {
           static_cast<std::uint8_t>(code >> 8U),
           static_cast<std::uint8_t>(code & 0xffU)}) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = static_cast<std::uint8_t>(
          (crc & 0x80U) != 0U ? (crc << 1U) ^ 0x07U : crc << 1U);
    }
  }
  return crc;
}

std::array<bool, message_bits> message(std::uint16_t code) {
  const std::uint64_t packed =
      (static_cast<std::uint64_t>(preamble) << 24U) |
      (static_cast<std::uint64_t>(code) << 8U) | crc8(code);
  std::array<bool, message_bits> bits{};
  for (std::size_t index = 0; index < bits.size(); ++index) {
    bits[index] = ((packed >> (bits.size() - 1U - index)) & 1U) != 0U;
  }
  return bits;
}

bool qualified(const WavPcm16& audio, CarrierBand band,
               const WatermarkPolicy& policy) {
  if (audio.channels == 0 || audio.sample_rate == 0 ||
      audio.samples.size() % audio.channels != 0) {
    return false;
  }
  const auto high = band == CarrierBand::w1 ? policy.w1_one_hz
                                             : policy.w2_one_hz;
  if (band == CarrierBand::w2 &&
      (audio.sample_rate < 96000U ||
       audio.sample_rate < policy.w2_minimum_sample_rate)) {
    return false;
  }
  return high + policy.nyquist_guard_hz < audio.sample_rate / 2.0;
}

std::pair<double, double> frequencies(CarrierBand band,
                                      const WatermarkPolicy& policy) {
  if (band == CarrierBand::w1) {
    return {policy.w1_zero_hz, policy.w1_one_hz};
  }
  return {policy.w2_zero_hz, policy.w2_one_hz};
}

double goertzel(const WavPcm16& audio, std::size_t first_frame,
                std::size_t frame_count, double frequency) {
  const double omega =
      2.0 * std::numbers::pi * frequency / audio.sample_rate;
  const double coefficient = 2.0 * std::cos(omega);
  double previous = 0.0;
  double before_previous = 0.0;
  for (std::size_t frame = 0; frame < frame_count; ++frame) {
    double mono = 0.0;
    for (std::size_t channel = 0; channel < audio.channels; ++channel) {
      mono += audio.samples[(first_frame + frame) * audio.channels + channel];
    }
    mono /= audio.channels;
    const double current = mono + coefficient * previous - before_previous;
    before_previous = previous;
    previous = current;
  }
  return before_previous * before_previous + previous * previous -
         coefficient * previous * before_previous;
}

struct Decoded {
  std::uint16_t preamble_value{};
  std::uint16_t code{};
  std::uint8_t checksum{};
  double confidence{};
};

Decoded decode_frame(const WavPcm16& audio, std::size_t start,
                     std::size_t samples_per_bit, CarrierBand band,
                     const WatermarkPolicy& policy) {
  const auto [zero_hz, one_hz] = frequencies(band, policy);
  std::uint64_t packed = 0;
  double confidence_sum = 0.0;
  for (std::size_t bit = 0; bit < message_bits; ++bit) {
    const auto offset = start + bit * samples_per_bit;
    const double zero = goertzel(audio, offset, samples_per_bit, zero_hz);
    const double one = goertzel(audio, offset, samples_per_bit, one_hz);
    packed = (packed << 1U) | static_cast<std::uint64_t>(one > zero);
    confidence_sum += std::abs(one - zero) /
                      std::max(1.0, std::abs(one) + std::abs(zero));
  }
  Decoded result;
  result.preamble_value = static_cast<std::uint16_t>(packed >> 24U);
  result.code = static_cast<std::uint16_t>((packed >> 8U) & 0xffffU);
  result.checksum = static_cast<std::uint8_t>(packed & 0xffU);
  result.confidence = confidence_sum / message_bits;
  return result;
}

std::size_t hamming(std::uint16_t left, std::uint16_t right) {
  auto value = static_cast<std::uint16_t>(left ^ right);
  std::size_t count = 0;
  while (value != 0U) {
    count += value & 1U;
    value >>= 1U;
  }
  return count;
}

}  // namespace

const char* carrier_band_name(CarrierBand band) noexcept {
  return band == CarrierBand::w1 ? "W1" : "W2";
}

Result<WatermarkEmbedReport> embed_watermark(
    WavPcm16& audio, std::uint16_t code, CarrierBand band,
    const WatermarkPolicy& policy) {
  if (policy.bit_duration_ms == 0 || policy.amplitude_dbfs > -6.0 ||
      policy.amplitude_dbfs < -120.0 || !finite_policy(policy)) {
    return fail<WatermarkEmbedReport>(ErrorCode::invalid_argument,
                                      "invalid watermark policy");
  }
  if (!hard_band_invariants(policy)) {
    return fail<WatermarkEmbedReport>(
        ErrorCode::watermark_path_unqualified,
        "watermark policy violates mandatory W1/W2 band safety constraints");
  }
  if (!qualified(audio, band, policy)) {
    return fail<WatermarkEmbedReport>(
        ErrorCode::watermark_path_unqualified,
        band == CarrierBand::w2
            ? "W2 requires at least 96 kHz and a guarded passband above 24 kHz"
            : "W1 carrier exceeds the guarded Nyquist limit");
  }
  const auto samples_per_bit = static_cast<std::size_t>(
      (static_cast<std::uint64_t>(audio.sample_rate) *
       policy.bit_duration_ms) /
      1000U);
  if (samples_per_bit < 16) {
    return fail<WatermarkEmbedReport>(ErrorCode::invalid_argument,
                                      "watermark bit window is too short");
  }
  const auto frame_samples = samples_per_bit * message_bits;
  const auto frame_count = audio.frames() / frame_samples;
  if (frame_count == 0) {
    return fail<WatermarkEmbedReport>(ErrorCode::invalid_argument,
                                      "audio is shorter than one watermark frame");
  }
  const auto [zero_hz, one_hz] = frequencies(band, policy);
  const auto bits = message(code);
  const double amplitude =
      32767.0 * std::pow(10.0, policy.amplitude_dbfs / 20.0);
  for (std::size_t frame = 0; frame < frame_count; ++frame) {
    for (std::size_t bit = 0; bit < bits.size(); ++bit) {
      const double frequency = bits[bit] ? one_hz : zero_hz;
      for (std::size_t sample = 0; sample < samples_per_bit; ++sample) {
        const auto frame_index = frame * frame_samples +
                                 bit * samples_per_bit + sample;
        const double phase = 2.0 * std::numbers::pi * frequency *
                             static_cast<double>(frame_index) /
                             audio.sample_rate;
        const auto carrier = amplitude * std::sin(phase);
        for (std::size_t channel = 0; channel < audio.channels; ++channel) {
          const auto index = frame_index * audio.channels + channel;
          const auto combined = static_cast<long>(
              std::lround(static_cast<double>(audio.samples[index]) + carrier));
          audio.samples[index] = static_cast<std::int16_t>(std::clamp(
              combined, static_cast<long>(std::numeric_limits<std::int16_t>::min()),
              static_cast<long>(std::numeric_limits<std::int16_t>::max())));
        }
      }
    }
  }
  return WatermarkEmbedReport{band, code, frame_count,
                              policy.amplitude_dbfs, true};
}

Result<std::vector<WatermarkObservation>> detect_watermarks(
    const WavPcm16& audio, const WatermarkPolicy& policy) {
  if (audio.channels == 0 || audio.sample_rate == 0 ||
      audio.samples.size() % audio.channels != 0 ||
      policy.bit_duration_ms == 0 || policy.amplitude_dbfs > -6.0 ||
      policy.amplitude_dbfs < -120.0 || !hard_band_invariants(policy) ||
      policy.minimum_confidence < 0.0 || policy.minimum_confidence > 1.0) {
    return fail<std::vector<WatermarkObservation>>(
        ErrorCode::invalid_argument, "invalid PCM16 audio or watermark policy");
  }
  const auto samples_per_bit = static_cast<std::size_t>(
      (static_cast<std::uint64_t>(audio.sample_rate) *
       policy.bit_duration_ms) /
      1000U);
  if (samples_per_bit < 16) {
    return fail<std::vector<WatermarkObservation>>(
        ErrorCode::invalid_argument, "watermark bit window is too short");
  }
  const auto frame_samples = samples_per_bit * message_bits;
  std::vector<WatermarkObservation> output;
  for (const auto band : {CarrierBand::w1, CarrierBand::w2}) {
    if (!qualified(audio, band, policy)) continue;
    const auto phase_step = std::max<std::size_t>(1, samples_per_bit / 10);
    for (std::size_t phase = 0; phase < frame_samples;
         phase += phase_step) {
      for (std::size_t start = phase;
           start + frame_samples <= audio.frames(); start += frame_samples) {
        const auto decoded =
            decode_frame(audio, start, samples_per_bit, band, policy);
        const auto corrected = hamming(decoded.preamble_value, preamble);
        if (corrected <= 2 && decoded.checksum == crc8(decoded.code) &&
            decoded.confidence >= policy.minimum_confidence) {
          const auto duplicate = std::any_of(
              output.begin(), output.end(), [&](const auto& existing) {
                const auto distance = existing.start_frame > start
                                          ? existing.start_frame - start
                                          : start - existing.start_frame;
                return existing.band == band &&
                       existing.code == decoded.code &&
                       distance < samples_per_bit;
              });
          if (!duplicate) {
            output.push_back({band, decoded.code, decoded.confidence, start,
                              start + frame_samples, corrected});
          }
        }
      }
    }
  }
  return output;
}

}  // namespace codec

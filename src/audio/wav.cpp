#include <codec/audio.hpp>

#include "../core/internal.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>

namespace codec {
namespace {

std::uint16_t u16(std::span<const std::byte> data, std::size_t offset) {
  return static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[offset])) |
         (static_cast<std::uint16_t>(
              static_cast<std::uint8_t>(data[offset + 1]))
          << 8U);
}

std::uint32_t u32(std::span<const std::byte> data, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(
                 static_cast<std::uint8_t>(data[offset + index]))
             << (index * 8U);
  }
  return value;
}

void put16(std::span<std::byte> data, std::size_t offset, std::uint16_t value) {
  data[offset] = static_cast<std::byte>(value & 0xffU);
  data[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void put32(std::span<std::byte> data, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    data[offset + index] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

bool fourcc(std::span<const std::byte> data, std::size_t offset,
            const char* value) {
  return offset + 4 <= data.size() &&
         std::memcmp(data.data() + offset, value, 4) == 0;
}

}  // namespace

Result<WavPcm16> WavPcm16::read(const std::filesystem::path& path) {
  auto file = detail::read_file(path, 1024ULL * 1024ULL * 1024ULL);
  if (!file) return file.error();
  const auto data = std::span<const std::byte>{*file};
  if (data.size() < 44 || !fourcc(data, 0, "RIFF") ||
      !fourcc(data, 8, "WAVE")) {
    return fail<WavPcm16>(ErrorCode::decode,
                          "input is not a RIFF/WAVE file");
  }
  bool have_format = false;
  std::uint16_t format = 0;
  std::uint16_t channels = 0;
  std::uint32_t sample_rate = 0;
  std::uint16_t block_align = 0;
  std::uint16_t bits = 0;
  std::span<const std::byte> pcm;
  std::size_t offset = 12;
  while (offset + 8 <= data.size()) {
    const auto chunk_size = static_cast<std::size_t>(u32(data, offset + 4));
    const auto payload_offset = offset + 8;
    if (payload_offset > data.size() ||
        chunk_size > data.size() - payload_offset) {
      return fail<WavPcm16>(ErrorCode::decode, "truncated WAV chunk");
    }
    if (fourcc(data, offset, "fmt ")) {
      if (chunk_size < 16) {
        return fail<WavPcm16>(ErrorCode::decode, "short WAV format chunk");
      }
      format = u16(data, payload_offset);
      channels = u16(data, payload_offset + 2);
      sample_rate = u32(data, payload_offset + 4);
      block_align = u16(data, payload_offset + 12);
      bits = u16(data, payload_offset + 14);
      have_format = true;
    } else if (fourcc(data, offset, "data")) {
      pcm = data.subspan(payload_offset, chunk_size);
    }
    const auto padded = chunk_size + (chunk_size & 1U);
    if (padded > data.size() - payload_offset) break;
    offset = payload_offset + padded;
  }
  if (!have_format || pcm.data() == nullptr) {
    return fail<WavPcm16>(ErrorCode::decode,
                          "WAV requires format and data chunks");
  }
  if (format != 1 || bits != 16 || channels == 0 || sample_rate == 0 ||
      block_align != channels * 2U || pcm.size() % block_align != 0) {
    return fail<WavPcm16>(ErrorCode::decode,
                          "only interleaved integer PCM16 WAV is supported");
  }
  WavPcm16 output;
  output.sample_rate = sample_rate;
  output.channels = channels;
  output.samples.resize(pcm.size() / 2);
  for (std::size_t index = 0; index < output.samples.size(); ++index) {
    output.samples[index] =
        static_cast<std::int16_t>(u16(pcm, index * 2));
  }
  return output;
}

Result<void> WavPcm16::write(const std::filesystem::path& path) const {
  if (channels == 0 || sample_rate == 0 || samples.size() % channels != 0) {
    return fail(ErrorCode::invalid_argument,
                "PCM16 audio requires a rate, channels, and complete frames");
  }
  const auto data_bytes = samples.size() * sizeof(std::int16_t);
  if (data_bytes > std::numeric_limits<std::uint32_t>::max() - 36U) {
    return fail(ErrorCode::resource_exhausted,
                "WAV exceeds the RIFF 32-bit size limit");
  }
  std::vector<std::byte> output(44 + data_bytes);
  std::memcpy(output.data(), "RIFF", 4);
  put32(output, 4, static_cast<std::uint32_t>(36 + data_bytes));
  std::memcpy(output.data() + 8, "WAVEfmt ", 8);
  put32(output, 16, 16);
  put16(output, 20, 1);
  put16(output, 22, channels);
  put32(output, 24, sample_rate);
  put32(output, 28, sample_rate * channels * 2U);
  put16(output, 32, static_cast<std::uint16_t>(channels * 2U));
  put16(output, 34, 16);
  std::memcpy(output.data() + 36, "data", 4);
  put32(output, 40, static_cast<std::uint32_t>(data_bytes));
  for (std::size_t index = 0; index < samples.size(); ++index) {
    put16(output, 44 + index * 2,
          static_cast<std::uint16_t>(samples[index]));
  }
  return detail::write_file(path, output);
}

}  // namespace codec


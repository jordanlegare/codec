#pragma once

#include <codec/result.hpp>
#include <codec/stream.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace codec {

inline constexpr std::uint16_t multiplex_frame_version = 1;

struct MultiplexFrame {
  StreamId stream{};
  std::uint64_t sequence{};
  StreamEpoch epoch{};
  StreamClock clock{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::vector<std::byte> payload;
};

struct MultiplexLimits {
  std::uint64_t maximum_payload_bytes{16ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_buffered_bytes{32ULL * 1024ULL * 1024ULL};
  std::size_t maximum_frames_per_push{4096};
};

Result<std::vector<std::byte>> encode_multiplex_frame(
    const MultiplexFrame& frame, MultiplexLimits limits = {});

class MultiplexDecoder {
 public:
  explicit MultiplexDecoder(MultiplexLimits limits = {});
  MultiplexDecoder(MultiplexDecoder&&) noexcept;
  MultiplexDecoder& operator=(MultiplexDecoder&&) noexcept;
  ~MultiplexDecoder();

  MultiplexDecoder(const MultiplexDecoder&) = delete;
  MultiplexDecoder& operator=(const MultiplexDecoder&) = delete;

  Result<std::vector<MultiplexFrame>> push(
      std::span<const std::byte> bytes);
  Result<void> finish();
  std::size_t buffered_bytes() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace codec

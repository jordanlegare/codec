#include <codec/transport.hpp>

#include <codec/integrity.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace codec {
namespace {

constexpr std::size_t header_size = 164;
constexpr std::size_t hash_offset = 132;
constexpr std::array<std::byte, 4> frame_magic{
    std::byte{'C'}, std::byte{'M'}, std::byte{'X'}, std::byte{'1'}};

template <typename Integer>
void put_le(std::span<std::byte> output, std::size_t offset, Integer value) {
  using Unsigned = std::make_unsigned_t<Integer>;
  auto bits = static_cast<Unsigned>(value);
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    output[offset + index] = static_cast<std::byte>(bits & 0xffU);
    bits >>= 8U;
  }
}

template <typename Integer>
Integer get_le(std::span<const std::byte> input, std::size_t offset) {
  using Unsigned = std::make_unsigned_t<Integer>;
  Unsigned bits = 0;
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    bits |= static_cast<Unsigned>(
                static_cast<std::uint8_t>(input[offset + index]))
            << (index * 8U);
  }
  return static_cast<Integer>(bits);
}

Result<void> validate_limits(const MultiplexLimits& limits) {
  if (limits.maximum_payload_bytes == 0 ||
      limits.maximum_buffered_bytes == 0 ||
      limits.maximum_frames_per_push == 0) {
    return fail(ErrorCode::invalid_argument,
                "multiplex limits must be non-zero");
  }
  if (limits.maximum_buffered_bytes < header_size) {
    return fail(ErrorCode::invalid_argument,
                "multiplex buffer limit is smaller than the frame header");
  }
  if (limits.maximum_payload_bytes >
      limits.maximum_buffered_bytes - header_size) {
    return fail(ErrorCode::invalid_argument,
                "multiplex buffer limit cannot hold one permitted frame");
  }
  if (limits.maximum_buffered_bytes >
          std::numeric_limits<std::size_t>::max() ||
      limits.maximum_payload_bytes >
          std::numeric_limits<std::size_t>::max()) {
    return fail(ErrorCode::invalid_argument,
                "multiplex limits exceed addressable memory");
  }
  return {};
}

Result<void> validate_frame_metadata(const MultiplexFrame& frame) {
  if (frame.end_ns < frame.start_ns) {
    return fail(ErrorCode::invalid_argument,
                "multiplex frame end time precedes start time");
  }
  if (frame.clock.source_timebase_numerator <= 0 ||
      frame.clock.source_timebase_denominator <= 0) {
    return fail(ErrorCode::invalid_argument,
                "multiplex source timebase must be positive");
  }
  return {};
}

Result<std::vector<std::byte>> frame_hash_input(
    std::span<const std::byte> header_prefix,
    std::span<const std::byte> payload) {
  try {
    std::vector<std::byte> input;
    input.reserve(header_prefix.size() + payload.size());
    input.insert(input.end(), header_prefix.begin(), header_prefix.end());
    input.insert(input.end(), payload.begin(), payload.end());
    return input;
  } catch (const std::bad_alloc&) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "multiplex integrity input allocation failed");
  }
}

bool digest_matches(std::span<const std::byte> header,
                    const Sha256& digest) {
  for (std::size_t index = 0; index < digest.size(); ++index) {
    if (static_cast<std::uint8_t>(header[hash_offset + index]) !=
        digest[index]) {
      return false;
    }
  }
  return true;
}

Result<MultiplexFrame> decode_complete_frame(
    std::span<const std::byte> bytes, const MultiplexLimits& limits) {
  if (bytes.size() < header_size) {
    return fail<MultiplexFrame>(ErrorCode::protocol,
                                "multiplex frame header is incomplete");
  }
  if (!std::equal(frame_magic.begin(), frame_magic.end(), bytes.begin())) {
    return fail<MultiplexFrame>(ErrorCode::protocol,
                                "invalid multiplex frame magic");
  }
  if (get_le<std::uint16_t>(bytes, 4) != multiplex_frame_version) {
    return fail<MultiplexFrame>(ErrorCode::protocol,
                                "unsupported multiplex frame version");
  }
  if (get_le<std::uint16_t>(bytes, 6) != 0) {
    return fail<MultiplexFrame>(ErrorCode::protocol,
                                "unsupported multiplex frame flags");
  }
  if (get_le<std::uint32_t>(bytes, 8) != header_size) {
    return fail<MultiplexFrame>(ErrorCode::protocol,
                                "invalid multiplex frame header size");
  }

  const auto total_size = get_le<std::uint64_t>(bytes, 12);
  const auto payload_size = get_le<std::uint64_t>(bytes, 124);
  if (total_size < header_size || payload_size != total_size - header_size) {
    return fail<MultiplexFrame>(ErrorCode::protocol,
                                "inconsistent multiplex frame lengths");
  }
  if (payload_size > limits.maximum_payload_bytes ||
      total_size > limits.maximum_buffered_bytes) {
    return fail<MultiplexFrame>(ErrorCode::resource_exhausted,
                                "multiplex frame exceeds configured limits");
  }
  if (total_size != bytes.size()) {
    return fail<MultiplexFrame>(ErrorCode::protocol,
                                "multiplex frame size does not match input");
  }

  const auto start_ns = get_le<std::int64_t>(bytes, 60);
  const auto end_ns = get_le<std::int64_t>(bytes, 68);
  const auto timebase_numerator = get_le<std::int64_t>(bytes, 108);
  const auto timebase_denominator = get_le<std::int64_t>(bytes, 116);
  if (end_ns < start_ns) {
    return fail<MultiplexFrame>(ErrorCode::protocol,
                                "multiplex frame interval is inverted");
  }
  if (timebase_numerator <= 0 || timebase_denominator <= 0) {
    return fail<MultiplexFrame>(ErrorCode::protocol,
                                "multiplex frame timebase is invalid");
  }

  const auto payload = bytes.subspan(header_size);
  auto hash_input = frame_hash_input(bytes.first(hash_offset), payload);
  if (!hash_input) return hash_input.error();
  const auto digest = sha256(*hash_input);
  if (!digest_matches(bytes, digest)) {
    return fail<MultiplexFrame>(ErrorCode::protocol,
                                "multiplex frame SHA-256 mismatch");
  }

  MultiplexFrame frame;
  for (std::size_t index = 0; index < frame.stream.bytes.size(); ++index) {
    frame.stream.bytes[index] =
        static_cast<std::uint8_t>(bytes[20 + index]);
  }
  frame.sequence = get_le<std::uint64_t>(bytes, 36);
  frame.epoch.connection = get_le<std::uint64_t>(bytes, 44);
  frame.epoch.format = get_le<std::uint64_t>(bytes, 52);
  frame.start_ns = start_ns;
  frame.end_ns = end_ns;
  frame.clock.monotonic_ns = get_le<std::int64_t>(bytes, 76);
  frame.clock.observed_utc_ns = get_le<std::int64_t>(bytes, 84);
  frame.clock.observed_utc_uncertainty_ns =
      get_le<std::uint64_t>(bytes, 92);
  frame.clock.source_timestamp = get_le<std::int64_t>(bytes, 100);
  frame.clock.source_timebase_numerator = timebase_numerator;
  frame.clock.source_timebase_denominator = timebase_denominator;
  try {
    frame.payload.assign(payload.begin(), payload.end());
  } catch (const std::bad_alloc&) {
    return fail<MultiplexFrame>(ErrorCode::resource_exhausted,
                                "multiplex payload allocation failed");
  }
  return frame;
}

}  // namespace

Result<std::vector<std::byte>> encode_multiplex_frame(
    const MultiplexFrame& frame, MultiplexLimits limits) {
  auto valid_limits = validate_limits(limits);
  if (!valid_limits) return valid_limits.error();
  auto valid_frame = validate_frame_metadata(frame);
  if (!valid_frame) return valid_frame.error();
  if (frame.payload.size() > limits.maximum_payload_bytes) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "multiplex payload exceeds configured limit");
  }

  const auto payload_size = static_cast<std::uint64_t>(frame.payload.size());
  if (payload_size >
      std::numeric_limits<std::uint64_t>::max() - header_size) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "multiplex frame size overflows");
  }
  const auto total_size = static_cast<std::uint64_t>(header_size) + payload_size;
  if (total_size > limits.maximum_buffered_bytes ||
      total_size > std::numeric_limits<std::size_t>::max()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "multiplex frame exceeds configured buffer limit");
  }

  try {
    std::vector<std::byte> output(static_cast<std::size_t>(total_size));
    std::copy(frame_magic.begin(), frame_magic.end(), output.begin());
    put_le<std::uint16_t>(output, 4, multiplex_frame_version);
    put_le<std::uint16_t>(output, 6, 0);
    put_le<std::uint32_t>(output, 8, static_cast<std::uint32_t>(header_size));
    put_le<std::uint64_t>(output, 12, total_size);
    for (std::size_t index = 0; index < frame.stream.bytes.size(); ++index) {
      output[20 + index] = static_cast<std::byte>(frame.stream.bytes[index]);
    }
    put_le<std::uint64_t>(output, 36, frame.sequence);
    put_le<std::uint64_t>(output, 44, frame.epoch.connection);
    put_le<std::uint64_t>(output, 52, frame.epoch.format);
    put_le<std::int64_t>(output, 60, frame.start_ns);
    put_le<std::int64_t>(output, 68, frame.end_ns);
    put_le<std::int64_t>(output, 76, frame.clock.monotonic_ns);
    put_le<std::int64_t>(output, 84, frame.clock.observed_utc_ns);
    put_le<std::uint64_t>(output, 92,
                          frame.clock.observed_utc_uncertainty_ns);
    put_le<std::int64_t>(output, 100, frame.clock.source_timestamp);
    put_le<std::int64_t>(output, 108,
                         frame.clock.source_timebase_numerator);
    put_le<std::int64_t>(output, 116,
                         frame.clock.source_timebase_denominator);
    put_le<std::uint64_t>(output, 124, payload_size);
    std::copy(frame.payload.begin(), frame.payload.end(),
              output.begin() + static_cast<std::ptrdiff_t>(header_size));

    auto hash_input = frame_hash_input(
        std::span<const std::byte>{output}.first(hash_offset),
        std::span<const std::byte>{output}.subspan(header_size));
    if (!hash_input) return hash_input.error();
    const auto digest = sha256(*hash_input);
    for (std::size_t index = 0; index < digest.size(); ++index) {
      output[hash_offset + index] = static_cast<std::byte>(digest[index]);
    }
    return output;
  } catch (const std::bad_alloc&) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "multiplex frame allocation failed");
  }
}

struct MultiplexDecoder::Impl {
  explicit Impl(MultiplexLimits configured) : limits(configured) {}

  MultiplexLimits limits{};
  std::vector<std::byte> buffer;
  bool finished{false};
  bool failed{false};
};

MultiplexDecoder::MultiplexDecoder(MultiplexLimits limits)
    : impl_(new (std::nothrow) Impl{limits}) {}

MultiplexDecoder::MultiplexDecoder(MultiplexDecoder&&) noexcept = default;
MultiplexDecoder& MultiplexDecoder::operator=(MultiplexDecoder&&) noexcept =
    default;
MultiplexDecoder::~MultiplexDecoder() = default;

Result<std::vector<MultiplexFrame>> MultiplexDecoder::push(
    std::span<const std::byte> bytes) {
  if (!impl_) {
    return fail<std::vector<MultiplexFrame>>(
        ErrorCode::resource_exhausted,
        "multiplex decoder allocation failed");
  }
  if (impl_->finished || impl_->failed) {
    return fail<std::vector<MultiplexFrame>>(
        ErrorCode::invalid_argument,
        "multiplex decoder is no longer accepting input");
  }
  auto valid_limits = validate_limits(impl_->limits);
  if (!valid_limits) {
    impl_->failed = true;
    return valid_limits.error();
  }
  if (bytes.size() > impl_->limits.maximum_buffered_bytes -
                         static_cast<std::uint64_t>(impl_->buffer.size())) {
    impl_->failed = true;
    return fail<std::vector<MultiplexFrame>>(
        ErrorCode::resource_exhausted,
        "multiplex decoder buffer limit exceeded");
  }

  try {
    impl_->buffer.insert(impl_->buffer.end(), bytes.begin(), bytes.end());
    std::vector<MultiplexFrame> staged;
    staged.reserve(std::min<std::size_t>(impl_->limits.maximum_frames_per_push,
                                         std::size_t{64}));
    std::size_t consumed = 0;

    while (staged.size() < impl_->limits.maximum_frames_per_push) {
      const auto remaining = impl_->buffer.size() - consumed;
      if (remaining < header_size) break;

      const auto view =
          std::span<const std::byte>{impl_->buffer}.subspan(consumed);
      const auto total_size = get_le<std::uint64_t>(view, 12);
      if (total_size > impl_->limits.maximum_buffered_bytes) {
        impl_->failed = true;
        return fail<std::vector<MultiplexFrame>>(
            ErrorCode::resource_exhausted,
            "multiplex frame exceeds decoder buffer limit");
      }
      if (total_size > std::numeric_limits<std::size_t>::max()) {
        impl_->failed = true;
        return fail<std::vector<MultiplexFrame>>(
            ErrorCode::resource_exhausted,
            "multiplex frame is not addressable");
      }
      const auto frame_size = static_cast<std::size_t>(total_size);
      if (remaining < frame_size) break;

      auto decoded = decode_complete_frame(view.first(frame_size),
                                           impl_->limits);
      if (!decoded) {
        impl_->failed = true;
        return decoded.error();
      }
      staged.push_back(std::move(*decoded));
      consumed += frame_size;
    }

    if (consumed != 0) {
      impl_->buffer.erase(
          impl_->buffer.begin(),
          impl_->buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    return staged;
  } catch (const std::bad_alloc&) {
    impl_->failed = true;
    return fail<std::vector<MultiplexFrame>>(
        ErrorCode::resource_exhausted,
        "multiplex decoder allocation failed");
  }
}

Result<void> MultiplexDecoder::finish() {
  if (!impl_) {
    return fail(ErrorCode::resource_exhausted,
                "multiplex decoder allocation failed");
  }
  if (impl_->failed) {
    return fail(ErrorCode::invalid_argument,
                "multiplex decoder is no longer usable");
  }
  if (!impl_->buffer.empty()) {
    impl_->failed = true;
    return fail(ErrorCode::protocol,
                "multiplex stream ended with an incomplete frame");
  }
  impl_->finished = true;
  return {};
}

std::size_t MultiplexDecoder::buffered_bytes() const noexcept {
  return impl_ ? impl_->buffer.size() : 0;
}

}  // namespace codec

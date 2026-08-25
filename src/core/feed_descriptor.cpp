#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace codec::detail {
namespace {

void put16(std::span<std::byte> output, std::size_t offset,
           std::uint16_t value) {
  output[offset] = static_cast<std::byte>(value & 0xffU);
  output[offset + 1] = static_cast<std::byte>(value >> 8U);
}

void put32(std::span<std::byte> output, std::size_t offset,
           std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    output[offset + index] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

std::uint16_t get16(std::span<const std::byte> input, std::size_t offset) {
  return static_cast<std::uint16_t>(static_cast<std::uint8_t>(input[offset])) |
         static_cast<std::uint16_t>(
             static_cast<std::uint8_t>(input[offset + 1]) << 8U);
}

std::uint32_t get32(std::span<const std::byte> input, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(
                 static_cast<std::uint8_t>(input[offset + index]))
             << (index * 8U);
  }
  return value;
}

}  // namespace

std::vector<std::byte> encode_feed_descriptor(const FeedInfo& feed) {
  if (feed.label.size() > std::numeric_limits<std::uint16_t>::max() ||
      feed.uri.size() > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  std::vector<std::byte> output(16 + feed.label.size() + feed.uri.size());
  std::memcpy(output.data(), "FDS1", 4);
  put16(output, 4, 1);
  put16(output, 6, static_cast<std::uint16_t>(feed.label.size()));
  put32(output, 8, static_cast<std::uint32_t>(feed.uri.size()));
  put32(output, 12, feed.preserve_source ? 1U : 0U);
  std::memcpy(output.data() + 16, feed.label.data(), feed.label.size());
  std::memcpy(output.data() + 16 + feed.label.size(), feed.uri.data(),
              feed.uri.size());
  return output;
}

Result<FeedInfo> decode_feed_descriptor(std::span<const std::byte> payload,
                                        const StreamId& stream) {
  if (payload.size() < 16 || std::memcmp(payload.data(), "FDS1", 4) != 0 ||
      get16(payload, 4) != 1) {
    return fail<FeedInfo>(ErrorCode::archive_corrupt,
                          "invalid feed descriptor record");
  }
  const auto label_size = static_cast<std::size_t>(get16(payload, 6));
  const auto uri_size = static_cast<std::size_t>(get32(payload, 8));
  if (label_size == 0 || label_size > payload.size() - 16 ||
      uri_size != payload.size() - 16 - label_size) {
    return fail<FeedInfo>(ErrorCode::archive_corrupt,
                          "invalid feed descriptor lengths");
  }
  FeedInfo feed;
  feed.stream = stream;
  feed.label.assign(reinterpret_cast<const char*>(payload.data() + 16),
                    label_size);
  feed.uri.assign(
      reinterpret_cast<const char*>(payload.data() + 16 + label_size),
      uri_size);
  feed.preserve_source = (get32(payload, 12) & 1U) != 0U;
  return feed;
}

Result<std::vector<std::byte>> encode_stream_descriptor(
    const StreamDescriptor& descriptor) {
  constexpr std::uint64_t header_size = 20;
  constexpr auto maximum_length =
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
  if (descriptor.source_id.empty() || descriptor.payload_type.empty()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "stream descriptor source ID and payload type must not be empty");
  }
  if (descriptor.label.size() > maximum_length ||
      descriptor.source_id.size() > maximum_length ||
      descriptor.payload_type.size() > maximum_length) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "stream descriptor field exceeds the development profile limit");
  }
  const auto output_size =
      header_size + static_cast<std::uint64_t>(descriptor.label.size()) +
      static_cast<std::uint64_t>(descriptor.source_id.size()) +
      static_cast<std::uint64_t>(descriptor.payload_type.size());
  if (output_size > std::numeric_limits<std::size_t>::max()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "stream descriptor exceeds the process address space");
  }
  std::vector<std::byte> output(static_cast<std::size_t>(output_size));
  std::memcpy(output.data(), "SDS1", 4);
  put16(output, 4, 1);
  put16(output, 6, static_cast<std::uint16_t>(descriptor.type));
  put32(output, 8, static_cast<std::uint32_t>(descriptor.label.size()));
  put32(output, 12, static_cast<std::uint32_t>(descriptor.source_id.size()));
  put32(output, 16,
        static_cast<std::uint32_t>(descriptor.payload_type.size()));
  auto* cursor = output.data() + header_size;
  std::memcpy(cursor, descriptor.label.data(), descriptor.label.size());
  cursor += descriptor.label.size();
  std::memcpy(cursor, descriptor.source_id.data(), descriptor.source_id.size());
  cursor += descriptor.source_id.size();
  std::memcpy(cursor, descriptor.payload_type.data(),
              descriptor.payload_type.size());
  return output;
}

Result<StreamDescriptor> decode_stream_descriptor(
    std::span<const std::byte> payload, const StreamId& stream) {
  constexpr std::size_t header_size = 20;
  if (payload.size() < header_size ||
      std::memcmp(payload.data(), "SDS1", 4) != 0 ||
      get16(payload, 4) != 1) {
    return fail<StreamDescriptor>(ErrorCode::archive_corrupt,
                                  "invalid stream descriptor record");
  }
  const auto label_size = static_cast<std::uint64_t>(get32(payload, 8));
  const auto source_size = static_cast<std::uint64_t>(get32(payload, 12));
  const auto payload_type_size =
      static_cast<std::uint64_t>(get32(payload, 16));
  const auto expected_size = static_cast<std::uint64_t>(header_size) +
                             label_size + source_size + payload_type_size;
  if (source_size == 0 || payload_type_size == 0 ||
      expected_size != payload.size()) {
    return fail<StreamDescriptor>(ErrorCode::archive_corrupt,
                                  "invalid stream descriptor lengths");
  }
  StreamDescriptor descriptor;
  descriptor.id = stream;
  descriptor.type = static_cast<StreamType>(get16(payload, 6));
  auto* cursor = reinterpret_cast<const char*>(payload.data() + header_size);
  descriptor.label.assign(cursor, static_cast<std::size_t>(label_size));
  cursor += label_size;
  descriptor.source_id.assign(cursor, static_cast<std::size_t>(source_size));
  cursor += source_size;
  descriptor.payload_type.assign(cursor,
                                 static_cast<std::size_t>(payload_type_size));
  return descriptor;
}

}  // namespace codec::detail

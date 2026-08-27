#pragma once

#include <codec/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <codec/archive.hpp>

namespace codec::detail {

std::uint32_t crc32c(std::span<const std::byte> data) noexcept;
Result<std::vector<std::byte>> read_file(const std::filesystem::path& path,
                                        std::uint64_t maximum_bytes =
                                            static_cast<std::uint64_t>(-1));
Result<void> write_file(const std::filesystem::path& path,
                        std::span<const std::byte> data);
Result<void> write_private_file(const std::filesystem::path& path,
                                std::span<const std::byte> data);
std::string json_escape(std::string_view input);
std::vector<std::byte> encode_feed_descriptor(const FeedInfo& feed);
Result<FeedInfo> decode_feed_descriptor(std::span<const std::byte> payload,
                                        const StreamId& stream);
Result<std::vector<std::byte>> encode_stream_descriptor(
    const StreamDescriptor& descriptor);
Result<StreamDescriptor> decode_stream_descriptor(
    std::span<const std::byte> payload, const StreamId& stream);
std::vector<std::byte> encode_stream_timing(const StreamTiming& timing);
Result<StreamTiming> decode_stream_timing(std::span<const std::byte> payload,
                                          const StreamId& stream);
std::vector<std::byte> encode_stream_gap(const StreamGap& gap);
Result<StreamGap> decode_stream_gap(std::span<const std::byte> payload,
                                    const StreamId& stream);
Result<std::vector<std::byte>> encode_stream_provenance(
    const StreamProvenance& provenance);
Result<StreamProvenance> decode_stream_provenance(
    std::span<const std::byte> payload);

struct StreamContinuityState {
  std::uint64_t next_sequence{};
  StreamEpoch epoch{};
  std::int64_t last_monotonic_ns{};
  bool initialized{false};
  bool has_monotonic{false};
};

Result<void> validate_and_advance(StreamContinuityState& state,
                                  const StreamTiming& timing);
Result<void> validate_and_advance(StreamContinuityState& state,
                                  const StreamGap& gap);

}  // namespace codec::detail

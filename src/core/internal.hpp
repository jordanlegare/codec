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

}  // namespace codec::detail

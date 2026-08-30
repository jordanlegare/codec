#pragma once

#include <codec/result.hpp>
#include <codec/stream.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace codec::profiles::video::detail {

struct HlsOrigin {
  std::string scheme;
  std::string host;
  std::uint16_t port{};

  bool operator==(const HlsOrigin&) const = default;
};

bool looks_like_hls_manifest(std::span<const std::byte> bytes) noexcept;
Result<void> validate_hls_manifest_security(std::span<const std::byte> bytes);
Result<HlsOrigin> parse_hls_http_origin(std::string_view uri);
Result<void> require_same_hls_origin(const HlsOrigin& primary,
                                     std::string_view child_uri);
StreamId derive_hls_child_stream_id(const StreamId& parent,
                                    std::size_t ordinal);
std::string hls_child_label(std::string_view parent_label,
                            std::size_t ordinal);

}  // namespace codec::profiles::video::detail

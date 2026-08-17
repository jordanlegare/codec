#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace codec {

using Sha256 = std::array<std::uint8_t, 32>;

Sha256 sha256(std::span<const std::byte> data);
std::string sha256_hex(std::span<const std::byte> data);

}  // namespace codec


#include <codec/integrity.hpp>
#include <codec/stream.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace codec {

StreamId derive_stream_id(std::string_view value) {
  std::vector<std::byte> input(value.size());
  if (!value.empty()) {
    std::memcpy(input.data(), value.data(), value.size());
  }
  const auto digest = sha256(input);
  StreamId id;
  std::copy_n(digest.begin(), id.bytes.size(), id.bytes.begin());
  id.bytes[6] = static_cast<std::uint8_t>((id.bytes[6] & 0x0fU) | 0x50U);
  id.bytes[8] = static_cast<std::uint8_t>((id.bytes[8] & 0x3fU) | 0x80U);
  return id;
}

std::string to_string(const StreamId& value) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string output;
  output.reserve(36);
  for (std::size_t index = 0; index < value.bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      output.push_back('-');
    }
    output.push_back(hex[value.bytes[index] >> 4U]);
    output.push_back(hex[value.bytes[index] & 0x0fU]);
  }
  return output;
}

}  // namespace codec

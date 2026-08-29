#include "internal.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace codec::detail {
namespace {

void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>(value & 0xffU));
  out.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u64(std::vector<std::byte>& out, std::uint64_t value) {
  for (unsigned int shift = 0; shift < 64U; shift += 8U) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

}  // namespace

ProvenanceRecordLink distributed_exact_link(const RecordInfo& record) {
  return ProvenanceRecordLink{
      .stream = record.stream,
      .type = record.type_code(),
      .sequence = record.sequence,
      .hash = record.hash,
  };
}

ProvenanceRecordLink distributed_exact_link(const ExtractedRecord& input) {
  return distributed_exact_link(input.record);
}

Sha256 distributed_partition_identity(
    StreamId stream,
    std::span<const ProvenanceRecordLink> records) {
  std::vector<std::byte> encoded;
  constexpr std::string_view domain{"CDP1"};
  for (const auto character : domain) {
    encoded.push_back(static_cast<std::byte>(
        static_cast<unsigned char>(character)));
  }
  for (const auto byte : stream.bytes) {
    encoded.push_back(static_cast<std::byte>(byte));
  }
  append_u64(encoded, static_cast<std::uint64_t>(records.size()));
  for (const auto& record : records) {
    append_u16(encoded, record.type);
    append_u64(encoded, record.sequence);
    for (const auto byte : record.hash) {
      encoded.push_back(static_cast<std::byte>(byte));
    }
  }
  return sha256(encoded);
}

}  // namespace codec::detail

#pragma once

#include <codec/integrity.hpp>
#include <codec/recovery.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace codec {

inline constexpr std::uint16_t xor_repair_symbol_version = 1;

struct XorRepairLimits {
  MultiplexLimits multiplex{};
  std::size_t maximum_source_frames{256};
  std::uint64_t maximum_total_encoded_source_bytes{64ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_symbol_bytes{32ULL * 1024ULL * 1024ULL};
};

struct XorRepairSymbol {
  RecoveryGroupDescriptor descriptor{};
  std::vector<std::uint64_t> encoded_frame_sizes;
  std::vector<Sha256> encoded_frame_hashes;
  std::vector<std::byte> parity;
};

struct XorRecoveredFrame {
  MultiplexFrame frame{};
  std::vector<std::byte> encoded_frame;
  Sha256 encoded_frame_hash{};
};

Result<XorRepairSymbol> create_xor_repair_symbol(
    const RecoveryGroupDescriptor& descriptor,
    std::span<const MultiplexFrame> source_frames,
    XorRepairLimits limits = {});

Result<std::vector<std::byte>> encode_xor_repair_symbol(
    const XorRepairSymbol& symbol, XorRepairLimits limits = {});

Result<XorRepairSymbol> decode_xor_repair_symbol(
    std::span<const std::byte> bytes, XorRepairLimits limits = {});

Result<XorRecoveredFrame> recover_xor_single_erasure(
    const XorRepairSymbol& symbol,
    std::span<const MultiplexFrame> observed_source_frames,
    XorRepairLimits limits = {});

}  // namespace codec

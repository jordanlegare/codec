#pragma once

#include <codec/xor_recovery.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace codec {

enum class StreamingRepairFrameKind : std::uint8_t {
  observed = 1,
  recovered = 2,
};

struct StreamingRepairFrame {
  StreamingRepairFrameKind kind{StreamingRepairFrameKind::observed};
  MultiplexFrame frame{};
  std::vector<std::byte> encoded_frame;
  Sha256 encoded_frame_hash{};
};

struct StreamingRepairLimits {
  SequenceLossLimits loss{};
  RecoveryGroupLimits groups{};
  XorRepairLimits xor_repair{};
  std::uint64_t maximum_buffered_encoded_source_bytes{
      64ULL * 1024ULL * 1024ULL};
};

struct StreamingRepairBatch {
  std::vector<StreamingRepairFrame> frames;
  std::optional<SequenceObservation> sequence_observation;
  std::optional<RecoveryGroupFrameObservation> group_observation;
  std::optional<RecoveryGroupReport> group_report;
};

class StreamingRepairSession {
 public:
  explicit StreamingRepairSession(StreamingRepairLimits limits = {});
  StreamingRepairSession(StreamingRepairSession&&) noexcept;
  StreamingRepairSession& operator=(StreamingRepairSession&&) noexcept;
  ~StreamingRepairSession();

  StreamingRepairSession(const StreamingRepairSession&) = delete;
  StreamingRepairSession& operator=(const StreamingRepairSession&) = delete;

  Result<void> begin_group(const RecoveryGroupDescriptor& descriptor);
  Result<StreamingRepairBatch> push_frame(
      std::span<const std::byte> encoded_cmx1);
  Result<StreamingRepairBatch> push_repair_symbol(
      std::span<const std::byte> encoded_xrf1);
  Result<StreamingRepairBatch> seal_group(const RecoveryGroupKey& key);

  Result<std::vector<TransportSequenceRange>> missing(
      const StreamId& stream, const StreamEpoch& epoch) const;
  Result<RecoveryGroupReport> group_status(
      const RecoveryGroupKey& key) const;
  std::uint64_t buffered_encoded_source_bytes() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace codec

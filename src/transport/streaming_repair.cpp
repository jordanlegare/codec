#include <codec/streaming_repair.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace codec {
namespace {

bool same_epoch(const StreamEpoch& lhs, const StreamEpoch& rhs) noexcept {
  return lhs.connection == rhs.connection && lhs.format == rhs.format;
}

bool same_descriptor(const RecoveryGroupDescriptor& lhs,
                     const RecoveryGroupDescriptor& rhs) noexcept {
  return lhs.key == rhs.key && lhs.first_sequence == rhs.first_sequence &&
         lhs.source_count == rhs.source_count;
}

bool same_symbol(const XorRepairSymbol& lhs,
                 const XorRepairSymbol& rhs) noexcept {
  return same_descriptor(lhs.descriptor, rhs.descriptor) &&
         lhs.encoded_frame_sizes == rhs.encoded_frame_sizes &&
         lhs.encoded_frame_hashes == rhs.encoded_frame_hashes &&
         lhs.parity == rhs.parity;
}

Result<void> validate_session_limits(const StreamingRepairLimits& limits) {
  if (limits.maximum_buffered_encoded_source_bytes == 0 ||
      limits.loss.maximum_tracks == 0 ||
      limits.loss.maximum_missing_ranges_per_track == 0 ||
      limits.groups.maximum_groups == 0 ||
      limits.groups.maximum_source_frames_per_group == 0 ||
      limits.groups.maximum_tracked_source_slots == 0 ||
      limits.xor_repair.maximum_source_frames == 0 ||
      limits.xor_repair.maximum_total_encoded_source_bytes == 0 ||
      limits.xor_repair.maximum_symbol_bytes == 0 ||
      limits.xor_repair.multiplex.maximum_payload_bytes == 0 ||
      limits.xor_repair.multiplex.maximum_buffered_bytes == 0 ||
      limits.xor_repair.multiplex.maximum_frames_per_push == 0) {
    return fail(ErrorCode::invalid_argument,
                "streaming-repair limits must be non-zero");
  }
  if (limits.maximum_buffered_encoded_source_bytes >
          std::numeric_limits<std::size_t>::max() ||
      limits.xor_repair.multiplex.maximum_payload_bytes >
          std::numeric_limits<std::size_t>::max() ||
      limits.xor_repair.multiplex.maximum_buffered_bytes >
          std::numeric_limits<std::size_t>::max() ||
      limits.xor_repair.maximum_total_encoded_source_bytes >
          std::numeric_limits<std::size_t>::max() ||
      limits.xor_repair.maximum_symbol_bytes >
          std::numeric_limits<std::size_t>::max()) {
    return fail(ErrorCode::invalid_argument,
                "streaming-repair limits exceed addressable memory");
  }
  return {};
}

Result<std::uint64_t> validate_group_descriptor(
    const RecoveryGroupDescriptor& descriptor,
    const StreamingRepairLimits& limits) {
  if (descriptor.source_count < 2) {
    return fail<std::uint64_t>(
        ErrorCode::invalid_argument,
        "streaming repair groups require at least two source frames");
  }
  if (descriptor.source_count > limits.xor_repair.maximum_source_frames ||
      descriptor.source_count >
          limits.groups.maximum_source_frames_per_group ||
      descriptor.source_count >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return fail<std::uint64_t>(
        ErrorCode::resource_exhausted,
        "streaming repair group exceeds configured source-frame limits");
  }
  if (descriptor.source_count >
      std::numeric_limits<std::uint64_t>::max() - descriptor.first_sequence) {
    return fail<std::uint64_t>(
        ErrorCode::invalid_argument,
        "streaming repair group sequence range overflows");
  }
  return descriptor.first_sequence + descriptor.source_count;
}

struct DecodedFrame {
  MultiplexFrame frame{};
  std::vector<std::byte> encoded;
  Sha256 encoded_hash{};
  std::uint64_t encoded_size{};
};

Result<DecodedFrame> decode_exact_cmx1(
    std::span<const std::byte> bytes, const XorRepairLimits& limits) {
  if (bytes.empty()) {
    return fail<DecodedFrame>(ErrorCode::protocol,
                              "streaming repair CMX1 frame is empty");
  }
  if (bytes.size() > limits.multiplex.maximum_buffered_bytes) {
    return fail<DecodedFrame>(
        ErrorCode::resource_exhausted,
        "streaming repair CMX1 frame exceeds the multiplex byte limit");
  }

  MultiplexDecoder decoder{limits.multiplex};
  auto decoded = decoder.push(bytes);
  if (!decoded) return decoded.error();
  if (decoded->size() != 1) {
    return fail<DecodedFrame>(
        ErrorCode::protocol,
        "streaming repair input must contain exactly one CMX1 frame");
  }
  auto finished = decoder.finish();
  if (!finished) return finished.error();
  if (decoder.buffered_bytes() != 0) {
    return fail<DecodedFrame>(
        ErrorCode::protocol,
        "streaming repair CMX1 decoder retained trailing bytes");
  }

  auto canonical = encode_multiplex_frame(decoded->front(), limits.multiplex);
  if (!canonical) return canonical.error();
  if (canonical->size() != bytes.size() ||
      !std::equal(canonical->begin(), canonical->end(), bytes.begin())) {
    return fail<DecodedFrame>(
        ErrorCode::protocol,
        "streaming repair CMX1 input is not the canonical frame encoding");
  }

  DecodedFrame result;
  result.frame = std::move(decoded->front());
  result.encoded_size = static_cast<std::uint64_t>(canonical->size());
  result.encoded_hash = sha256(*canonical);
  result.encoded = std::move(*canonical);
  return result;
}

struct BufferedSlot {
  bool present{false};
  bool emitted{false};
  bool recovered{false};
  MultiplexFrame frame{};
  std::uint64_t encoded_size{};
  Sha256 encoded_hash{};
};

struct SessionGroup {
  RecoveryGroupDescriptor descriptor{};
  std::vector<BufferedSlot> slots;
  std::optional<XorRepairSymbol> symbol;
  std::optional<RecoveryGroupReport> sealed_report;
};

SessionGroup* find_group(std::vector<SessionGroup>& groups,
                         const RecoveryGroupKey& key) noexcept {
  for (auto& group : groups) {
    if (group.descriptor.key == key) return &group;
  }
  return nullptr;
}

SessionGroup* find_group_for_frame(std::vector<SessionGroup>& groups,
                                   const MultiplexFrame& frame) noexcept {
  for (auto& group : groups) {
    if (group.descriptor.key.stream != frame.stream ||
        !same_epoch(group.descriptor.key.epoch, frame.epoch)) {
      continue;
    }
    const auto end =
        group.descriptor.first_sequence + group.descriptor.source_count;
    if (frame.sequence >= group.descriptor.first_sequence &&
        frame.sequence < end) {
      return &group;
    }
  }
  return nullptr;
}

Result<void> verify_commitment(const XorRepairSymbol& symbol,
                               std::size_t index,
                               std::uint64_t encoded_size,
                               const Sha256& encoded_hash) {
  if (index >= symbol.encoded_frame_sizes.size() ||
      index >= symbol.encoded_frame_hashes.size()) {
    return fail(ErrorCode::internal,
                "streaming repair symbol slot table is inconsistent");
  }
  if (symbol.encoded_frame_sizes[index] != encoded_size ||
      symbol.encoded_frame_hashes[index] != encoded_hash) {
    return fail(ErrorCode::protocol,
                "streaming repair CMX1 frame conflicts with XRF1 commitment");
  }
  return {};
}

Result<StreamingRepairFrame> make_observed_output(
    const BufferedSlot& slot, const XorRepairLimits& limits) {
  auto encoded = encode_multiplex_frame(slot.frame, limits.multiplex);
  if (!encoded) return encoded.error();
  if (encoded->size() != slot.encoded_size ||
      sha256(*encoded) != slot.encoded_hash) {
    return fail<StreamingRepairFrame>(
        ErrorCode::internal,
        "streaming repair buffered CMX1 frame changed after verification");
  }

  try {
    StreamingRepairFrame output;
    output.kind = StreamingRepairFrameKind::observed;
    output.frame = slot.frame;
    output.encoded_frame = std::move(*encoded);
    output.encoded_frame_hash = slot.encoded_hash;
    return output;
  } catch (const std::bad_alloc&) {
    return fail<StreamingRepairFrame>(
        ErrorCode::resource_exhausted,
        "streaming repair observed-frame output allocation failed");
  }
}

std::optional<std::size_t> single_missing_index(
    const SessionGroup& group) noexcept {
  if (!group.sealed_report.has_value()) return std::nullopt;
  const auto& report = *group.sealed_report;
  if (report.state != RecoveryGroupState::sealed_incomplete ||
      report.observed_source_count != group.descriptor.source_count - 1 ||
      report.missing_ranges.size() != 1) {
    return std::nullopt;
  }
  const auto& range = report.missing_ranges.front();
  if (range.end <= range.begin || range.end - range.begin != 1 ||
      range.begin < group.descriptor.first_sequence) {
    return std::nullopt;
  }
  const auto offset = range.begin - group.descriptor.first_sequence;
  if (offset >= group.descriptor.source_count ||
      offset > std::numeric_limits<std::size_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(offset);
}

struct StagedRecovery {
  std::size_t slot_index{};
  std::uint64_t encoded_size{};
  XorRecoveredFrame recovered{};
  StreamingRepairFrame output{};
};

Result<std::optional<StagedRecovery>> stage_single_recovery(
    const SessionGroup& group, const XorRepairSymbol& symbol,
    const StreamingRepairLimits& limits,
    std::uint64_t currently_buffered_bytes) {
  const auto missing_index = single_missing_index(group);
  if (!missing_index.has_value() || group.slots[*missing_index].present) {
    return std::optional<StagedRecovery>{};
  }

  try {
    std::vector<MultiplexFrame> observed;
    observed.reserve(static_cast<std::size_t>(group.descriptor.source_count - 1));
    for (std::size_t index = 0; index < group.slots.size(); ++index) {
      if (index == *missing_index) continue;
      if (!group.slots[index].present) {
        return fail<std::optional<StagedRecovery>>(
            ErrorCode::internal,
            "streaming repair sealed group is missing unexpected local state");
      }
      observed.push_back(group.slots[index].frame);
    }

    auto recovered = recover_xor_single_erasure(symbol, observed,
                                                 limits.xor_repair);
    if (!recovered) return recovered.error();
    const auto recovered_size =
        static_cast<std::uint64_t>(recovered->encoded_frame.size());
    if (recovered_size >
            limits.maximum_buffered_encoded_source_bytes -
                std::min(currently_buffered_bytes,
                         limits.maximum_buffered_encoded_source_bytes)) {
      return fail<std::optional<StagedRecovery>>(
          ErrorCode::resource_exhausted,
          "streaming repair recovered frame exceeds the session byte limit");
    }

    StagedRecovery staged;
    staged.slot_index = *missing_index;
    staged.encoded_size = recovered_size;
    staged.output.kind = StreamingRepairFrameKind::recovered;
    staged.output.frame = recovered->frame;
    staged.output.encoded_frame = std::move(recovered->encoded_frame);
    staged.output.encoded_frame_hash = recovered->encoded_frame_hash;
    staged.recovered = std::move(*recovered);
    return std::optional<StagedRecovery>{std::move(staged)};
  } catch (const std::bad_alloc&) {
    return fail<std::optional<StagedRecovery>>(
        ErrorCode::resource_exhausted,
        "streaming repair recovery staging allocation failed");
  }
}

void commit_recovery(SessionGroup& group, StagedRecovery& staged,
                     std::uint64_t& buffered_bytes) noexcept {
  auto& slot = group.slots[staged.slot_index];
  slot.present = true;
  slot.emitted = true;
  slot.recovered = true;
  slot.frame = std::move(staged.recovered.frame);
  slot.encoded_size = staged.encoded_size;
  slot.encoded_hash = staged.recovered.encoded_frame_hash;
  buffered_bytes += staged.encoded_size;
}

bool has_buffer_capacity(std::uint64_t current, std::uint64_t additional,
                         std::uint64_t maximum) noexcept {
  return current <= maximum && additional <= maximum - current;
}

}  // namespace

struct StreamingRepairSession::Impl {
  explicit Impl(StreamingRepairLimits configured)
      : limits(std::move(configured)),
        loss(limits.loss),
        groups(limits.groups) {}

  StreamingRepairLimits limits{};
  SequenceLossObserver loss;
  RecoveryGroupTracker groups;
  std::vector<SessionGroup> session_groups;
  std::uint64_t buffered_encoded_source_bytes{};
};

StreamingRepairSession::StreamingRepairSession(StreamingRepairLimits limits)
    : impl_(new (std::nothrow) Impl{std::move(limits)}) {}
StreamingRepairSession::StreamingRepairSession(
    StreamingRepairSession&&) noexcept = default;
StreamingRepairSession& StreamingRepairSession::operator=(
    StreamingRepairSession&&) noexcept = default;
StreamingRepairSession::~StreamingRepairSession() = default;

Result<void> StreamingRepairSession::begin_group(
    const RecoveryGroupDescriptor& descriptor) {
  if (!impl_) {
    return fail(ErrorCode::resource_exhausted,
                "streaming repair session allocation failed");
  }
  auto valid_limits = validate_session_limits(impl_->limits);
  if (!valid_limits) return valid_limits.error();
  auto end = validate_group_descriptor(descriptor, impl_->limits);
  if (!end) return end.error();
  (void)end;
  if (impl_->session_groups.size() >= impl_->limits.groups.maximum_groups) {
    return fail(ErrorCode::resource_exhausted,
                "streaming repair group-count limit exceeded");
  }

  try {
    SessionGroup staged;
    staged.descriptor = descriptor;
    staged.slots.resize(static_cast<std::size_t>(descriptor.source_count));
    impl_->session_groups.push_back(std::move(staged));
  } catch (const std::bad_alloc&) {
    return fail(ErrorCode::resource_exhausted,
                "streaming repair group allocation failed");
  }

  auto begun = impl_->groups.begin(descriptor);
  if (!begun) {
    impl_->session_groups.pop_back();
    return begun.error();
  }
  return {};
}

Result<StreamingRepairBatch> StreamingRepairSession::push_frame(
    std::span<const std::byte> encoded_cmx1) {
  if (!impl_) {
    return fail<StreamingRepairBatch>(
        ErrorCode::resource_exhausted,
        "streaming repair session allocation failed");
  }
  auto valid_limits = validate_session_limits(impl_->limits);
  if (!valid_limits) return valid_limits.error();

  auto decoded = decode_exact_cmx1(encoded_cmx1, impl_->limits.xor_repair);
  if (!decoded) return decoded.error();
  auto* group = find_group_for_frame(impl_->session_groups, decoded->frame);

  try {
    StreamingRepairBatch batch;
    if (!group) {
      auto observation = impl_->groups.observe(decoded->frame);
      if (!observation) return observation.error();
      batch.group_observation = *observation;
      return batch;
    }

    const auto index = static_cast<std::size_t>(
        decoded->frame.sequence - group->descriptor.first_sequence);
    auto& slot = group->slots[index];

    if (slot.present) {
      if (slot.encoded_size != decoded->encoded_size ||
          slot.encoded_hash != decoded->encoded_hash) {
        return fail<StreamingRepairBatch>(
            ErrorCode::protocol,
            "streaming repair duplicate conflicts with the accepted source slot");
      }
      if (group->symbol.has_value()) {
        auto committed = verify_commitment(*group->symbol, index,
                                           decoded->encoded_size,
                                           decoded->encoded_hash);
        if (!committed) return committed.error();
      }

      auto sequence_observation = impl_->loss.observe(decoded->frame);
      if (!sequence_observation) return sequence_observation.error();
      batch.sequence_observation = *sequence_observation;

      RecoveryGroupFrameObservation duplicate;
      duplicate.kind = RecoveryGroupFrameKind::duplicate;
      duplicate.group = group->descriptor.key;
      duplicate.sequence = decoded->frame.sequence;
      if (!group->sealed_report.has_value()) {
        auto group_observation = impl_->groups.observe(decoded->frame);
        if (!group_observation) return group_observation.error();
        duplicate = *group_observation;
      }
      batch.group_observation = duplicate;
      return batch;
    }

    if (group->sealed_report.has_value()) {
      return fail<StreamingRepairBatch>(
          ErrorCode::invalid_argument,
          "sealed streaming repair group cannot accept a new source member");
    }
    if (!has_buffer_capacity(
            impl_->buffered_encoded_source_bytes, decoded->encoded_size,
            impl_->limits.maximum_buffered_encoded_source_bytes)) {
      return fail<StreamingRepairBatch>(
          ErrorCode::resource_exhausted,
          "streaming repair buffered-source byte limit exceeded");
    }

    const bool committed_now = group->symbol.has_value();
    if (committed_now) {
      auto committed = verify_commitment(*group->symbol, index,
                                         decoded->encoded_size,
                                         decoded->encoded_hash);
      if (!committed) return committed.error();

      StreamingRepairFrame output;
      output.kind = StreamingRepairFrameKind::observed;
      output.frame = decoded->frame;
      output.encoded_frame = decoded->encoded;
      output.encoded_frame_hash = decoded->encoded_hash;
      batch.frames.push_back(std::move(output));
    }

    auto sequence_observation = impl_->loss.observe(decoded->frame);
    if (!sequence_observation) return sequence_observation.error();
    auto group_observation = impl_->groups.observe(decoded->frame);
    if (!group_observation) return group_observation.error();

    slot.present = true;
    slot.emitted = committed_now;
    slot.frame = std::move(decoded->frame);
    slot.encoded_size = decoded->encoded_size;
    slot.encoded_hash = decoded->encoded_hash;
    impl_->buffered_encoded_source_bytes += decoded->encoded_size;

    batch.sequence_observation = *sequence_observation;
    batch.group_observation = *group_observation;
    return batch;
  } catch (const std::bad_alloc&) {
    return fail<StreamingRepairBatch>(
        ErrorCode::resource_exhausted,
        "streaming repair frame staging allocation failed");
  }
}

Result<StreamingRepairBatch> StreamingRepairSession::push_repair_symbol(
    std::span<const std::byte> encoded_xrf1) {
  if (!impl_) {
    return fail<StreamingRepairBatch>(
        ErrorCode::resource_exhausted,
        "streaming repair session allocation failed");
  }
  auto valid_limits = validate_session_limits(impl_->limits);
  if (!valid_limits) return valid_limits.error();

  auto decoded = decode_xor_repair_symbol(encoded_xrf1,
                                          impl_->limits.xor_repair);
  if (!decoded) return decoded.error();
  auto* group = find_group(impl_->session_groups, decoded->descriptor.key);
  if (!group) {
    return fail<StreamingRepairBatch>(
        ErrorCode::invalid_argument,
        "XRF1 symbol does not name a registered streaming repair group");
  }
  if (!same_descriptor(group->descriptor, decoded->descriptor)) {
    return fail<StreamingRepairBatch>(
        ErrorCode::protocol,
        "XRF1 symbol conflicts with the registered recovery-group geometry");
  }

  try {
    StreamingRepairBatch batch;
    if (group->sealed_report.has_value()) {
      batch.group_report = *group->sealed_report;
    }

    if (group->symbol.has_value()) {
      if (!same_symbol(*group->symbol, *decoded)) {
        return fail<StreamingRepairBatch>(
            ErrorCode::protocol,
            "streaming repair group received a conflicting XRF1 symbol");
      }
      auto staged = stage_single_recovery(
          *group, *group->symbol, impl_->limits,
          impl_->buffered_encoded_source_bytes);
      if (!staged) return staged.error();
      if (staged->has_value()) {
        batch.frames.push_back(std::move((*staged)->output));
        commit_recovery(*group, **staged,
                        impl_->buffered_encoded_source_bytes);
      }
      return batch;
    }

    std::vector<std::size_t> emitted_indices;
    emitted_indices.reserve(group->slots.size());
    for (std::size_t index = 0; index < group->slots.size(); ++index) {
      const auto& slot = group->slots[index];
      if (!slot.present) continue;
      auto committed = verify_commitment(*decoded, index, slot.encoded_size,
                                         slot.encoded_hash);
      if (!committed) return committed.error();
      if (slot.emitted) continue;
      auto output = make_observed_output(slot, impl_->limits.xor_repair);
      if (!output) return output.error();
      batch.frames.push_back(std::move(*output));
      emitted_indices.push_back(index);
    }

    auto staged = stage_single_recovery(
        *group, *decoded, impl_->limits,
        impl_->buffered_encoded_source_bytes);
    if (!staged) return staged.error();
    if (staged->has_value()) {
      batch.frames.push_back(std::move((*staged)->output));
    }

    group->symbol.emplace(std::move(*decoded));
    for (const auto index : emitted_indices) {
      group->slots[index].emitted = true;
    }
    if (staged->has_value()) {
      commit_recovery(*group, **staged,
                      impl_->buffered_encoded_source_bytes);
    }
    return batch;
  } catch (const std::bad_alloc&) {
    return fail<StreamingRepairBatch>(
        ErrorCode::resource_exhausted,
        "streaming repair symbol staging allocation failed");
  }
}

Result<StreamingRepairBatch> StreamingRepairSession::seal_group(
    const RecoveryGroupKey& key) {
  if (!impl_) {
    return fail<StreamingRepairBatch>(
        ErrorCode::resource_exhausted,
        "streaming repair session allocation failed");
  }
  auto valid_limits = validate_session_limits(impl_->limits);
  if (!valid_limits) return valid_limits.error();
  auto* group = find_group(impl_->session_groups, key);
  if (!group) {
    return fail<StreamingRepairBatch>(ErrorCode::invalid_argument,
                                      "streaming repair group was not found");
  }

  try {
    if (!group->sealed_report.has_value()) {
      auto report = impl_->groups.seal(key);
      if (!report) return report.error();
      group->sealed_report = std::move(*report);
    }

    StreamingRepairBatch batch;
    batch.group_report = *group->sealed_report;
    if (!group->symbol.has_value()) return batch;

    auto staged = stage_single_recovery(
        *group, *group->symbol, impl_->limits,
        impl_->buffered_encoded_source_bytes);
    if (!staged) return staged.error();
    if (staged->has_value()) {
      batch.frames.push_back(std::move((*staged)->output));
      commit_recovery(*group, **staged,
                      impl_->buffered_encoded_source_bytes);
    }
    return batch;
  } catch (const std::bad_alloc&) {
    return fail<StreamingRepairBatch>(
        ErrorCode::resource_exhausted,
        "streaming repair seal staging allocation failed");
  }
}

Result<std::vector<TransportSequenceRange>> StreamingRepairSession::missing(
    const StreamId& stream, const StreamEpoch& epoch) const {
  if (!impl_) {
    return fail<std::vector<TransportSequenceRange>>(
        ErrorCode::resource_exhausted,
        "streaming repair session allocation failed");
  }
  auto valid_limits = validate_session_limits(impl_->limits);
  if (!valid_limits) return valid_limits.error();
  return impl_->loss.missing(stream, epoch);
}

Result<RecoveryGroupReport> StreamingRepairSession::group_status(
    const RecoveryGroupKey& key) const {
  if (!impl_) {
    return fail<RecoveryGroupReport>(
        ErrorCode::resource_exhausted,
        "streaming repair session allocation failed");
  }
  auto valid_limits = validate_session_limits(impl_->limits);
  if (!valid_limits) return valid_limits.error();
  return impl_->groups.status(key);
}

std::uint64_t StreamingRepairSession::buffered_encoded_source_bytes() const
    noexcept {
  if (!impl_) return 0;
  return impl_->buffered_encoded_source_bytes;
}

}  // namespace codec

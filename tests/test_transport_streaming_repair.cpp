#include "test.hpp"

#include <codec/integrity.hpp>
#include <codec/streaming_repair.hpp>
#include <codec/xor_recovery.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

codec::MultiplexFrame source_frame(std::uint64_t sequence,
                                   std::size_t payload_size,
                                   std::string_view stream_name =
                                       "recovery/stream/e5") {
  codec::MultiplexFrame frame;
  frame.stream = codec::derive_stream_id(stream_name);
  frame.sequence = sequence;
  frame.epoch = {.connection = 11, .format = 5};
  frame.clock = {
      .monotonic_ns = 1000 + static_cast<std::int64_t>(sequence),
      .observed_utc_ns = 2000 + static_cast<std::int64_t>(sequence),
      .observed_utc_uncertainty_ns = 7,
      .source_timestamp = static_cast<std::int64_t>(sequence * 960),
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 48000,
  };
  frame.start_ns = static_cast<std::int64_t>(sequence * 20);
  frame.end_ns = frame.start_ns + 20;
  frame.payload.resize(payload_size);
  for (std::size_t index = 0; index < payload_size; ++index) {
    frame.payload[index] = static_cast<std::byte>(
        static_cast<unsigned char>((sequence * 3U + index * 19U) & 0xffU));
  }
  return frame;
}

std::vector<codec::MultiplexFrame> source_group(
    std::uint64_t first_sequence = 40,
    std::string_view stream_name = "recovery/stream/e5") {
  return {
      source_frame(first_sequence, 0, stream_name),
      source_frame(first_sequence + 1, 3, stream_name),
      source_frame(first_sequence + 2, 31, stream_name),
      source_frame(first_sequence + 3, 257, stream_name),
  };
}

codec::RecoveryGroupDescriptor descriptor_for(
    const std::vector<codec::MultiplexFrame>& frames,
    std::uint64_t group_sequence = 21) {
  codec::RecoveryGroupDescriptor descriptor;
  descriptor.key.stream = frames.front().stream;
  descriptor.key.epoch = frames.front().epoch;
  descriptor.key.group_sequence = group_sequence;
  descriptor.first_sequence = frames.front().sequence;
  descriptor.source_count = frames.size();
  return descriptor;
}

std::vector<std::byte> exact_encoding(const codec::MultiplexFrame& frame) {
  auto encoded = codec::encode_multiplex_frame(frame);
  if (!encoded) throw std::runtime_error("test CMX1 frame failed to encode");
  return *encoded;
}

std::vector<std::byte> exact_symbol_encoding(
    const codec::RecoveryGroupDescriptor& descriptor,
    const std::vector<codec::MultiplexFrame>& frames) {
  auto symbol = codec::create_xor_repair_symbol(descriptor, frames);
  if (!symbol) throw std::runtime_error("test XRF1 creation failed");
  auto encoded = codec::encode_xor_repair_symbol(*symbol);
  if (!encoded) throw std::runtime_error("test XRF1 encoding failed");
  return *encoded;
}

void expect_same_frame(const codec::MultiplexFrame& actual,
                       const codec::MultiplexFrame& expected) {
  EXPECT_EQ(actual.stream, expected.stream);
  EXPECT_EQ(actual.sequence, expected.sequence);
  EXPECT_EQ(actual.epoch.connection, expected.epoch.connection);
  EXPECT_EQ(actual.epoch.format, expected.epoch.format);
  EXPECT_EQ(actual.clock.monotonic_ns, expected.clock.monotonic_ns);
  EXPECT_EQ(actual.clock.observed_utc_ns, expected.clock.observed_utc_ns);
  EXPECT_EQ(actual.clock.observed_utc_uncertainty_ns,
            expected.clock.observed_utc_uncertainty_ns);
  EXPECT_EQ(actual.clock.source_timestamp, expected.clock.source_timestamp);
  EXPECT_EQ(actual.clock.source_timebase_numerator,
            expected.clock.source_timebase_numerator);
  EXPECT_EQ(actual.clock.source_timebase_denominator,
            expected.clock.source_timebase_denominator);
  EXPECT_EQ(actual.start_ns, expected.start_ns);
  EXPECT_EQ(actual.end_ns, expected.end_ns);
  EXPECT_EQ(actual.payload, expected.payload);
}

const codec::StreamingRepairFrame* find_emitted(
    const codec::StreamingRepairBatch& batch, std::uint64_t sequence) {
  for (const auto& emitted : batch.frames) {
    if (emitted.frame.sequence == sequence) return &emitted;
  }
  return nullptr;
}

}  // namespace

TEST(streaming_repair_buffers_until_commitment_and_recovers_one_sealed_erasure) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);
  codec::StreamingRepairSession session;
  EXPECT_TRUE(session.begin_group(descriptor));

  for (const auto index : {std::size_t{2}, std::size_t{0}, std::size_t{3}}) {
    auto pushed = session.push_frame(exact_encoding(frames[index]));
    EXPECT_TRUE(pushed);
    EXPECT_TRUE(pushed->frames.empty());
  }
  EXPECT_TRUE(session.buffered_encoded_source_bytes() > 0);

  auto committed = session.push_repair_symbol(
      exact_symbol_encoding(descriptor, frames));
  EXPECT_TRUE(committed);
  EXPECT_EQ(committed->frames.size(), std::size_t{3});
  for (const auto index : {std::size_t{0}, std::size_t{2}, std::size_t{3}}) {
    const auto* emitted = find_emitted(*committed, frames[index].sequence);
    EXPECT_TRUE(emitted != nullptr);
    EXPECT_EQ(emitted->kind, codec::StreamingRepairFrameKind::observed);
    expect_same_frame(emitted->frame, frames[index]);
    EXPECT_EQ(emitted->encoded_frame, exact_encoding(frames[index]));
    EXPECT_EQ(emitted->encoded_frame_hash,
              codec::sha256(emitted->encoded_frame));
  }

  auto sealed = session.seal_group(descriptor.key);
  EXPECT_TRUE(sealed);
  EXPECT_TRUE(sealed->group_report.has_value());
  EXPECT_EQ(sealed->group_report->state,
            codec::RecoveryGroupState::sealed_incomplete);
  EXPECT_EQ(sealed->group_report->missing_ranges.size(), std::size_t{1});
  EXPECT_EQ(sealed->group_report->missing_ranges.front().begin,
            frames[1].sequence);
  EXPECT_EQ(sealed->group_report->missing_ranges.front().end,
            frames[1].sequence + 1);
  EXPECT_EQ(sealed->frames.size(), std::size_t{1});
  EXPECT_EQ(sealed->frames.front().kind,
            codec::StreamingRepairFrameKind::recovered);
  expect_same_frame(sealed->frames.front().frame, frames[1]);
  EXPECT_EQ(sealed->frames.front().encoded_frame, exact_encoding(frames[1]));
}

TEST(streaming_repair_symbol_first_tolerates_reordering_and_duplicates) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);
  codec::StreamingRepairSession session;
  EXPECT_TRUE(session.begin_group(descriptor));

  auto symbol = session.push_repair_symbol(
      exact_symbol_encoding(descriptor, frames));
  EXPECT_TRUE(symbol);
  EXPECT_TRUE(symbol->frames.empty());

  auto third = session.push_frame(exact_encoding(frames[2]));
  EXPECT_TRUE(third);
  EXPECT_EQ(third->frames.size(), std::size_t{1});
  EXPECT_EQ(third->frames.front().frame.sequence, frames[2].sequence);

  auto first = session.push_frame(exact_encoding(frames[0]));
  EXPECT_TRUE(first);
  EXPECT_EQ(first->frames.size(), std::size_t{1});

  auto duplicate = session.push_frame(exact_encoding(frames[0]));
  EXPECT_TRUE(duplicate);
  EXPECT_TRUE(duplicate->frames.empty());
  EXPECT_TRUE(duplicate->group_observation.has_value());
  EXPECT_EQ(duplicate->group_observation->kind,
            codec::RecoveryGroupFrameKind::duplicate);

  auto fourth = session.push_frame(exact_encoding(frames[3]));
  EXPECT_TRUE(fourth);
  EXPECT_EQ(fourth->frames.size(), std::size_t{1});

  auto sealed = session.seal_group(descriptor.key);
  EXPECT_TRUE(sealed);
  EXPECT_EQ(sealed->frames.size(), std::size_t{1});
  EXPECT_EQ(sealed->frames.front().kind,
            codec::StreamingRepairFrameKind::recovered);
  EXPECT_EQ(sealed->frames.front().frame.sequence, frames[1].sequence);
}

TEST(streaming_repair_late_arrival_fills_provisional_loss_before_seal) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);
  codec::StreamingRepairSession session;
  EXPECT_TRUE(session.begin_group(descriptor));
  EXPECT_TRUE(session.push_repair_symbol(
      exact_symbol_encoding(descriptor, frames)));

  auto first = session.push_frame(exact_encoding(frames[0]));
  EXPECT_TRUE(first);
  EXPECT_TRUE(first->sequence_observation.has_value());
  EXPECT_EQ(first->sequence_observation->kind,
            codec::SequenceObservationKind::baseline);

  auto gap = session.push_frame(exact_encoding(frames[2]));
  EXPECT_TRUE(gap);
  EXPECT_TRUE(gap->sequence_observation.has_value());
  EXPECT_EQ(gap->sequence_observation->kind,
            codec::SequenceObservationKind::gap_opened);
  auto missing = session.missing(frames[0].stream, frames[0].epoch);
  EXPECT_TRUE(missing);
  EXPECT_EQ(missing->size(), std::size_t{1});
  EXPECT_EQ(missing->front().begin, frames[1].sequence);
  EXPECT_EQ(missing->front().end, frames[2].sequence);

  auto late = session.push_frame(exact_encoding(frames[1]));
  EXPECT_TRUE(late);
  EXPECT_TRUE(late->sequence_observation.has_value());
  EXPECT_EQ(late->sequence_observation->kind,
            codec::SequenceObservationKind::gap_filled);
  missing = session.missing(frames[0].stream, frames[0].epoch);
  EXPECT_TRUE(missing);
  EXPECT_TRUE(missing->empty());

  EXPECT_TRUE(session.push_frame(exact_encoding(frames[3])));
  auto sealed = session.seal_group(descriptor.key);
  EXPECT_TRUE(sealed);
  EXPECT_TRUE(sealed->group_report.has_value());
  EXPECT_EQ(sealed->group_report->state,
            codec::RecoveryGroupState::sealed_complete);
  EXPECT_TRUE(sealed->frames.empty());
}

TEST(streaming_repair_never_attempts_zero_or_multiple_erasure_recovery) {
  const auto complete_frames = source_group();
  const auto complete_descriptor = descriptor_for(complete_frames);
  codec::StreamingRepairSession complete;
  EXPECT_TRUE(complete.begin_group(complete_descriptor));
  EXPECT_TRUE(complete.push_repair_symbol(
      exact_symbol_encoding(complete_descriptor, complete_frames)));
  for (const auto& frame : complete_frames) {
    auto pushed = complete.push_frame(exact_encoding(frame));
    EXPECT_TRUE(pushed);
    EXPECT_EQ(pushed->frames.size(), std::size_t{1});
  }
  auto complete_seal = complete.seal_group(complete_descriptor.key);
  EXPECT_TRUE(complete_seal);
  EXPECT_EQ(complete_seal->group_report->state,
            codec::RecoveryGroupState::sealed_complete);
  EXPECT_TRUE(complete_seal->frames.empty());

  const auto sparse_frames = source_group(80, "recovery/stream/e5/sparse");
  const auto sparse_descriptor = descriptor_for(sparse_frames, 22);
  codec::StreamingRepairSession sparse;
  EXPECT_TRUE(sparse.begin_group(sparse_descriptor));
  EXPECT_TRUE(sparse.push_repair_symbol(
      exact_symbol_encoding(sparse_descriptor, sparse_frames)));
  EXPECT_TRUE(sparse.push_frame(exact_encoding(sparse_frames[0])));
  EXPECT_TRUE(sparse.push_frame(exact_encoding(sparse_frames[3])));
  auto sparse_seal = sparse.seal_group(sparse_descriptor.key);
  EXPECT_TRUE(sparse_seal);
  EXPECT_EQ(sparse_seal->group_report->state,
            codec::RecoveryGroupState::sealed_incomplete);
  EXPECT_EQ(sparse_seal->group_report->observed_source_count,
            std::uint64_t{2});
  EXPECT_TRUE(sparse_seal->frames.empty());
}

TEST(streaming_repair_rejects_corrupt_cmx1_without_poisoning_group_state) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);
  codec::StreamingRepairSession session;
  EXPECT_TRUE(session.begin_group(descriptor));

  auto corrupt = exact_encoding(frames[0]);
  corrupt.back() ^= std::byte{1};
  auto result = session.push_frame(corrupt);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::protocol);

  auto status = session.group_status(descriptor.key);
  EXPECT_TRUE(status);
  EXPECT_EQ(status->observed_source_count, std::uint64_t{0});
  EXPECT_EQ(session.buffered_encoded_source_bytes(), std::uint64_t{0});
}

TEST(streaming_repair_rejects_valid_cmx1_that_conflicts_with_xrf1_commitment) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);
  codec::StreamingRepairSession session;
  EXPECT_TRUE(session.begin_group(descriptor));
  EXPECT_TRUE(session.push_repair_symbol(
      exact_symbol_encoding(descriptor, frames)));

  auto changed = frames[0];
  changed.payload.push_back(std::byte{0x55});
  auto result = session.push_frame(exact_encoding(changed));
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::protocol);

  auto status = session.group_status(descriptor.key);
  EXPECT_TRUE(status);
  EXPECT_EQ(status->observed_source_count, std::uint64_t{0});
  EXPECT_EQ(session.buffered_encoded_source_bytes(), std::uint64_t{0});
}

TEST(streaming_repair_does_not_emit_unregistered_members) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);
  codec::StreamingRepairSession session;
  EXPECT_TRUE(session.begin_group(descriptor));
  EXPECT_TRUE(session.push_repair_symbol(
      exact_symbol_encoding(descriptor, frames)));

  const auto outsider = source_frame(40, 4, "recovery/stream/e5/other");
  auto result = session.push_frame(exact_encoding(outsider));
  EXPECT_TRUE(result);
  EXPECT_TRUE(result->frames.empty());
  EXPECT_TRUE(result->group_observation.has_value());
  EXPECT_EQ(result->group_observation->kind,
            codec::RecoveryGroupFrameKind::not_member);
}

TEST(streaming_repair_rejects_conflicting_duplicate_transactionally) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);
  codec::StreamingRepairSession session;
  EXPECT_TRUE(session.begin_group(descriptor));

  const auto original_bytes = exact_encoding(frames[0]);
  auto original = session.push_frame(original_bytes);
  EXPECT_TRUE(original);
  EXPECT_TRUE(original->frames.empty());
  const auto buffered = session.buffered_encoded_source_bytes();

  auto changed = frames[0];
  changed.payload.push_back(std::byte{0x77});
  auto conflict = session.push_frame(exact_encoding(changed));
  EXPECT_FALSE(conflict);
  EXPECT_EQ(conflict.error().code, codec::ErrorCode::protocol);
  EXPECT_EQ(session.buffered_encoded_source_bytes(), buffered);

  auto status = session.group_status(descriptor.key);
  EXPECT_TRUE(status);
  EXPECT_EQ(status->observed_source_count, std::uint64_t{1});
}

TEST(streaming_repair_rejects_corrupt_xrf1_without_releasing_buffered_frames) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);
  codec::StreamingRepairSession session;
  EXPECT_TRUE(session.begin_group(descriptor));
  EXPECT_TRUE(session.push_frame(exact_encoding(frames[0])));
  EXPECT_TRUE(session.push_frame(exact_encoding(frames[2])));
  EXPECT_TRUE(session.push_frame(exact_encoding(frames[3])));

  auto corrupt = exact_symbol_encoding(descriptor, frames);
  corrupt.back() ^= std::byte{1};
  auto rejected = session.push_repair_symbol(corrupt);
  EXPECT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, codec::ErrorCode::protocol);

  auto committed = session.push_repair_symbol(
      exact_symbol_encoding(descriptor, frames));
  EXPECT_TRUE(committed);
  EXPECT_EQ(committed->frames.size(), std::size_t{3});
  auto sealed = session.seal_group(descriptor.key);
  EXPECT_TRUE(sealed);
  EXPECT_EQ(sealed->frames.size(), std::size_t{1});
  EXPECT_EQ(sealed->frames.front().frame.sequence, frames[1].sequence);
}

TEST(streaming_repair_enforces_session_buffer_limit_before_tracking_member) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);
  const auto large_frame = exact_encoding(frames[3]);

  codec::StreamingRepairLimits limits;
  limits.maximum_buffered_encoded_source_bytes = large_frame.size() - 1;
  codec::StreamingRepairSession session{limits};
  EXPECT_TRUE(session.begin_group(descriptor));

  auto rejected = session.push_frame(large_frame);
  EXPECT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, codec::ErrorCode::resource_exhausted);
  auto status = session.group_status(descriptor.key);
  EXPECT_TRUE(status);
  EXPECT_EQ(status->observed_source_count, std::uint64_t{0});
  EXPECT_EQ(session.buffered_encoded_source_bytes(), std::uint64_t{0});
}

TEST(streaming_repair_can_commit_and_recover_after_exact_one_erasure_is_sealed) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);
  codec::StreamingRepairSession session;
  EXPECT_TRUE(session.begin_group(descriptor));
  EXPECT_TRUE(session.push_frame(exact_encoding(frames[0])));
  EXPECT_TRUE(session.push_frame(exact_encoding(frames[2])));
  EXPECT_TRUE(session.push_frame(exact_encoding(frames[3])));

  auto sealed = session.seal_group(descriptor.key);
  EXPECT_TRUE(sealed);
  EXPECT_TRUE(sealed->frames.empty());
  EXPECT_EQ(sealed->group_report->state,
            codec::RecoveryGroupState::sealed_incomplete);

  auto committed = session.push_repair_symbol(
      exact_symbol_encoding(descriptor, frames));
  EXPECT_TRUE(committed);
  EXPECT_TRUE(committed->group_report.has_value());
  EXPECT_EQ(committed->frames.size(), std::size_t{4});
  std::size_t recovered_count = 0;
  for (const auto& emitted : committed->frames) {
    if (emitted.kind == codec::StreamingRepairFrameKind::recovered) {
      ++recovered_count;
      EXPECT_EQ(emitted.frame.sequence, frames[1].sequence);
      EXPECT_EQ(emitted.encoded_frame, exact_encoding(frames[1]));
    }
  }
  EXPECT_EQ(recovered_count, std::size_t{1});
}

TEST(streaming_repair_requires_xrf1_compatible_group_geometry) {
  const auto frames = source_group();
  auto descriptor = descriptor_for(frames);
  descriptor.source_count = 1;
  codec::StreamingRepairSession session;
  auto result = session.begin_group(descriptor);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
}

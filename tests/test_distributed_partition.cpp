#include "test.hpp"

#include <codec/distributed.hpp>
#include <codec/integrity.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

codec::ExtractedRecord exact_record(std::string_view stream_name,
                                    std::uint64_t sequence,
                                    std::size_t payload_size) {
  codec::ExtractedRecord out;
  out.record.type = codec::RecordType::source_bytes;
  out.record.sequence = sequence;
  out.record.stream = codec::derive_stream_id(stream_name);
  out.record.start_ns = static_cast<std::int64_t>(sequence * 10);
  out.record.end_ns = out.record.start_ns + 10;
  out.payload.resize(payload_size);
  for (std::size_t index = 0; index < payload_size; ++index) {
    out.payload[index] = static_cast<std::byte>(
        static_cast<unsigned char>((sequence + index * 17U) & 0xffU));
  }
  out.record.payload_size = static_cast<std::uint64_t>(out.payload.size());
  out.record.hash = codec::sha256(out.payload);
  return out;
}

void expect_link(const codec::ProvenanceRecordLink& link,
                 const codec::ExtractedRecord& input) {
  EXPECT_EQ(link.stream, input.record.stream);
  EXPECT_EQ(link.type, input.record.type_code());
  EXPECT_EQ(link.sequence, input.record.sequence);
  EXPECT_EQ(link.hash, input.record.hash);
}

void expect_same_partition(const codec::DistributedPartition& lhs,
                           const codec::DistributedPartition& rhs) {
  EXPECT_EQ(lhs.identity, rhs.identity);
  EXPECT_EQ(lhs.stream, rhs.stream);
  EXPECT_EQ(lhs.payload_bytes, rhs.payload_bytes);
  EXPECT_EQ(lhs.records.size(), rhs.records.size());
  for (std::size_t index = 0; index < lhs.records.size(); ++index) {
    EXPECT_EQ(lhs.records[index].stream, rhs.records[index].stream);
    EXPECT_EQ(lhs.records[index].type, rhs.records[index].type);
    EXPECT_EQ(lhs.records[index].sequence, rhs.records[index].sequence);
    EXPECT_EQ(lhs.records[index].hash, rhs.records[index].hash);
  }
}

}  // namespace

TEST(distributed_partition_is_deterministic_and_preserves_exact_links) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f1/a", 1, 3), exact_record("f1/b", 2, 4),
      exact_record("f1/a", 3, 5), exact_record("f1/a", 4, 6),
      exact_record("f1/b", 5, 7)};
  codec::DistributedPartitionLimits limits;
  limits.maximum_records_per_partition = 2;
  limits.maximum_payload_bytes_per_partition = 64;

  auto first = codec::partition_exact_records(inputs, limits);
  auto second = codec::partition_exact_records(inputs, limits);
  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
  EXPECT_EQ(first->size(), std::size_t{3});
  EXPECT_EQ(second->size(), first->size());

  EXPECT_EQ((*first)[0].stream, inputs[0].record.stream);
  EXPECT_EQ((*first)[0].records.size(), std::size_t{2});
  expect_link((*first)[0].records[0], inputs[0]);
  expect_link((*first)[0].records[1], inputs[2]);
  EXPECT_EQ((*first)[0].payload_bytes, std::uint64_t{8});

  EXPECT_EQ((*first)[1].stream, inputs[1].record.stream);
  EXPECT_EQ((*first)[1].records.size(), std::size_t{2});
  expect_link((*first)[1].records[0], inputs[1]);
  expect_link((*first)[1].records[1], inputs[4]);
  EXPECT_EQ((*first)[1].payload_bytes, std::uint64_t{11});

  EXPECT_EQ((*first)[2].stream, inputs[3].record.stream);
  EXPECT_EQ((*first)[2].records.size(), std::size_t{1});
  expect_link((*first)[2].records[0], inputs[3]);
  EXPECT_EQ((*first)[2].payload_bytes, std::uint64_t{6});

  for (std::size_t index = 0; index < first->size(); ++index) {
    expect_same_partition((*first)[index], (*second)[index]);
  }
}

TEST(distributed_partition_splits_on_payload_bytes_without_splitting_records) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f1/bytes", 1, 5), exact_record("f1/bytes", 2, 6),
      exact_record("f1/bytes", 3, 4)};
  codec::DistributedPartitionLimits limits;
  limits.maximum_records_per_partition = 8;
  limits.maximum_payload_bytes_per_partition = 10;

  auto result = codec::partition_exact_records(inputs, limits);
  EXPECT_TRUE(result);
  EXPECT_EQ(result->size(), std::size_t{2});
  EXPECT_EQ((*result)[0].records.size(), std::size_t{1});
  expect_link((*result)[0].records.front(), inputs[0]);
  EXPECT_EQ((*result)[0].payload_bytes, std::uint64_t{5});
  EXPECT_EQ((*result)[1].records.size(), std::size_t{2});
  expect_link((*result)[1].records[0], inputs[1]);
  expect_link((*result)[1].records[1], inputs[2]);
  EXPECT_EQ((*result)[1].payload_bytes, std::uint64_t{10});
}

TEST(distributed_partition_identity_changes_with_ordered_membership) {
  const auto first_record = exact_record("f1/id", 1, 2);
  const auto second_record = exact_record("f1/id", 2, 2);
  const std::vector<codec::ExtractedRecord> forward{first_record, second_record};
  const std::vector<codec::ExtractedRecord> reverse{second_record, first_record};

  auto lhs = codec::partition_exact_records(forward);
  auto rhs = codec::partition_exact_records(reverse);
  EXPECT_TRUE(lhs);
  EXPECT_TRUE(rhs);
  EXPECT_EQ(lhs->size(), std::size_t{1});
  EXPECT_EQ(rhs->size(), std::size_t{1});
  EXPECT_FALSE((*lhs)[0].identity == (*rhs)[0].identity);
}

TEST(distributed_partition_empty_input_is_valid) {
  const std::vector<codec::ExtractedRecord> inputs;
  auto result = codec::partition_exact_records(inputs);
  EXPECT_TRUE(result);
  EXPECT_TRUE(result->empty());
}

TEST(distributed_partition_rejects_zero_limits) {
  codec::DistributedPartitionLimits limits;
  limits.maximum_partitions = 0;
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f1/zero", 1, 1)};

  auto result = codec::partition_exact_records(inputs, limits);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
}

TEST(distributed_partition_rejects_inconsistent_exact_input) {
  auto malformed = exact_record("f1/malformed", 1, 3);
  malformed.record.payload_size += 1;
  const std::vector<codec::ExtractedRecord> inputs{malformed};

  auto result = codec::partition_exact_records(inputs);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
}

TEST(distributed_partition_enforces_aggregate_input_bytes) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f1/aggregate", 1, 6),
      exact_record("f1/aggregate", 2, 6)};
  codec::DistributedPartitionLimits limits;
  limits.maximum_input_bytes = 11;

  auto result = codec::partition_exact_records(inputs, limits);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
}

TEST(distributed_partition_rejects_oversized_individual_record) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f1/oversized", 1, 6)};
  codec::DistributedPartitionLimits limits;
  limits.maximum_payload_bytes_per_partition = 5;

  auto result = codec::partition_exact_records(inputs, limits);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
}

TEST(distributed_partition_enforces_partition_count) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f1/count", 1, 1), exact_record("f1/count", 2, 1)};
  codec::DistributedPartitionLimits limits;
  limits.maximum_records_per_partition = 1;
  limits.maximum_partitions = 1;

  auto result = codec::partition_exact_records(inputs, limits);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
}

TEST(distributed_partition_enforces_input_record_count) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f1/input-count", 1, 1),
      exact_record("f1/input-count", 2, 1)};
  codec::DistributedPartitionLimits limits;
  limits.maximum_input_records = 1;

  auto result = codec::partition_exact_records(inputs, limits);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
}

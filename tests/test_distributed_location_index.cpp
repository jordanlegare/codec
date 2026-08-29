#include "test.hpp"

#include <codec/distributed.hpp>
#include <codec/integrity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::vector<std::byte> bytes(std::string_view value) {
  const auto chars = std::span{value.data(), value.size()};
  const auto raw = std::as_bytes(chars);
  return {raw.begin(), raw.end()};
}

codec::ExtractedRecord exact_record(std::string_view stream_name,
                                    std::uint64_t sequence,
                                    std::string_view payload) {
  codec::ExtractedRecord out;
  out.record.type = codec::RecordType::source_bytes;
  out.record.sequence = sequence;
  out.record.stream = codec::derive_stream_id(stream_name);
  out.record.start_ns = static_cast<std::int64_t>(sequence * 10);
  out.record.end_ns = out.record.start_ns + 10;
  out.record.file_offset = 1000 + sequence;
  out.payload = bytes(payload);
  out.record.payload_size = static_cast<std::uint64_t>(out.payload.size());
  out.record.hash = codec::sha256(out.payload);
  return out;
}

codec::DistributedPartition one_partition(
    const std::vector<codec::ExtractedRecord>& inputs) {
  auto partitions = codec::partition_exact_records(inputs);
  EXPECT_TRUE(partitions);
  EXPECT_EQ(partitions->size(), std::size_t{1});
  return partitions->front();
}

codec::DistributedRecordLocation location_for(
    const codec::ExtractedRecord& input,
    std::string store,
    std::string key,
    std::string version,
    std::uint64_t offset) {
  return codec::DistributedRecordLocation{
      .record = input.record,
      .object = {.store = std::move(store),
                 .key = std::move(key),
                 .version = std::move(version)},
      .offset = offset,
      .length = input.record.payload_size,
  };
}

void expect_same_record(const codec::RecordInfo& actual,
                        const codec::RecordInfo& expected) {
  EXPECT_EQ(actual.type_code(), expected.type_code());
  EXPECT_EQ(actual.sequence, expected.sequence);
  EXPECT_EQ(actual.stream, expected.stream);
  EXPECT_EQ(actual.start_ns, expected.start_ns);
  EXPECT_EQ(actual.end_ns, expected.end_ns);
  EXPECT_EQ(actual.payload_size, expected.payload_size);
  EXPECT_EQ(actual.file_offset, expected.file_offset);
  EXPECT_EQ(actual.hash, expected.hash);
}

void expect_same_location(const codec::DistributedRecordLocation& actual,
                          const codec::DistributedRecordLocation& expected) {
  expect_same_record(actual.record, expected.record);
  EXPECT_EQ(actual.object.store, expected.object.store);
  EXPECT_EQ(actual.object.key, expected.object.key);
  EXPECT_EQ(actual.object.version, expected.object.version);
  EXPECT_EQ(actual.offset, expected.offset);
  EXPECT_EQ(actual.length, expected.length);
}

}  // namespace

TEST(distributed_location_index_empty_build_is_valid) {
  const std::vector<codec::DistributedRecordLocation> locations;
  auto index = codec::build_distributed_location_index(locations);
  EXPECT_TRUE(index);
  EXPECT_EQ(index->record_count(), std::size_t{0});
  EXPECT_EQ(index->location_count(), std::size_t{0});
  EXPECT_TRUE(index->entries().empty());
}

TEST(distributed_location_index_canonicalizes_and_deduplicates) {
  const auto record = exact_record("index/canonical", 7, "payload");
  const auto later = location_for(record, "store-b", "z-key", "v2", 80);
  const auto earlier = location_for(record, "store-a", "a-key", "v1", 20);
  const std::vector<codec::DistributedRecordLocation> locations{
      later, earlier, earlier};

  auto index = codec::build_distributed_location_index(locations);
  EXPECT_TRUE(index);
  EXPECT_EQ(index->record_count(), std::size_t{1});
  EXPECT_EQ(index->location_count(), std::size_t{2});
  EXPECT_EQ(index->entries().size(), std::size_t{1});
  expect_same_record(index->entries()[0].record, record.record);
  EXPECT_EQ(index->entries()[0].candidates.size(), std::size_t{2});
  expect_same_location(index->entries()[0].candidates[0], earlier);
  expect_same_location(index->entries()[0].candidates[1], later);
}

TEST(distributed_location_index_is_independent_of_input_order) {
  const auto a = exact_record("index/order", 1, "a");
  const auto b = exact_record("index/order", 2, "bb");
  const auto a1 = location_for(a, "store-b", "a-2", "", 50);
  const auto a0 = location_for(a, "store-a", "a-1", "v1", 10);
  const auto b0 = location_for(b, "store-a", "b-1", "v1", 70);

  const std::vector<codec::DistributedRecordLocation> forward{b0, a1, a0};
  const std::vector<codec::DistributedRecordLocation> reverse{a0, a1, b0};
  auto lhs = codec::build_distributed_location_index(forward);
  auto rhs = codec::build_distributed_location_index(reverse);
  EXPECT_TRUE(lhs);
  EXPECT_TRUE(rhs);
  EXPECT_EQ(lhs->record_count(), rhs->record_count());
  EXPECT_EQ(lhs->location_count(), rhs->location_count());
  EXPECT_EQ(lhs->entries().size(), rhs->entries().size());
  for (std::size_t i = 0; i < lhs->entries().size(); ++i) {
    expect_same_record(lhs->entries()[i].record, rhs->entries()[i].record);
    EXPECT_EQ(lhs->entries()[i].candidates.size(),
              rhs->entries()[i].candidates.size());
    for (std::size_t j = 0; j < lhs->entries()[i].candidates.size(); ++j) {
      expect_same_location(lhs->entries()[i].candidates[j],
                           rhs->entries()[i].candidates[j]);
    }
  }
}

TEST(distributed_location_index_rejects_conflicting_record_metadata) {
  const auto record = exact_record("index/conflict", 3, "payload");
  auto first = location_for(record, "store-a", "one", "v1", 0);
  auto second = location_for(record, "store-b", "two", "v1", 100);
  second.record.file_offset += 1;

  const std::vector<codec::DistributedRecordLocation> locations{first, second};
  auto index = codec::build_distributed_location_index(locations);
  EXPECT_FALSE(index);
  EXPECT_EQ(index.error().code, codec::ErrorCode::invalid_argument);
}

TEST(distributed_location_index_preserves_unknown_raw_type) {
  auto record = exact_record("index/unknown", 4, "opaque");
  constexpr codec::RecordTypeCode raw_type = 0x4321;
  record.record.type = static_cast<codec::RecordType>(raw_type);
  const auto location = location_for(record, "store", "opaque", "v7", 9);

  const std::vector<codec::DistributedRecordLocation> locations{location};
  auto index = codec::build_distributed_location_index(locations);
  EXPECT_TRUE(index);
  EXPECT_EQ(index->entries().size(), std::size_t{1});
  EXPECT_EQ(index->entries()[0].record.type_code(), raw_type);
  EXPECT_EQ(index->entries()[0].candidates[0].record.type_code(), raw_type);
}

TEST(distributed_location_index_rejects_malformed_locations) {
  const auto record = exact_record("index/malformed", 5, "abcd");

  auto interval = location_for(record, "store", "key", "", 0);
  interval.record.end_ns = interval.record.start_ns - 1;
  auto result = codec::build_distributed_location_index(
      std::vector<codec::DistributedRecordLocation>{interval});
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);

  auto length = location_for(record, "store", "key", "", 0);
  ++length.length;
  result = codec::build_distributed_location_index(
      std::vector<codec::DistributedRecordLocation>{length});
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);

  auto overflow = location_for(record, "store", "key", "", 0);
  overflow.offset = std::numeric_limits<std::uint64_t>::max();
  result = codec::build_distributed_location_index(
      std::vector<codec::DistributedRecordLocation>{overflow});
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);

  auto empty_store = location_for(record, "", "key", "", 0);
  result = codec::build_distributed_location_index(
      std::vector<codec::DistributedRecordLocation>{empty_store});
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);

  auto empty_key = location_for(record, "store", "", "", 0);
  result = codec::build_distributed_location_index(
      std::vector<codec::DistributedRecordLocation>{empty_key});
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
}

TEST(distributed_location_index_enforces_build_limits) {
  const auto a = exact_record("index/limits", 1, "a");
  const auto b = exact_record("index/limits", 2, "b");
  const auto a0 = location_for(a, "s", "a0", "", 0);
  const auto a1 = location_for(a, "s", "a1", "", 1);
  const auto b0 = location_for(b, "s", "b0", "", 2);
  const std::vector<codec::DistributedRecordLocation> all{a0, a1, b0};

  codec::DistributedLocationIndexLimits zero;
  zero.maximum_input_locations = 0;
  auto result = codec::build_distributed_location_index(all, zero);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);

  codec::DistributedLocationIndexLimits input_limit;
  input_limit.maximum_input_locations = 2;
  result = codec::build_distributed_location_index(all, input_limit);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);

  codec::DistributedLocationIndexLimits record_limit;
  record_limit.maximum_records = 1;
  result = codec::build_distributed_location_index(all, record_limit);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);

  codec::DistributedLocationIndexLimits candidate_limit;
  candidate_limit.maximum_locations_per_record = 1;
  result = codec::build_distributed_location_index(all, candidate_limit);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);

  codec::DistributedLocationIndexLimits metadata_limit;
  metadata_limit.maximum_metadata_bytes = 2;
  result = codec::build_distributed_location_index(all, metadata_limit);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);

  codec::DistributedLocationIndexLimits key_limit;
  key_limit.maximum_key_bytes = 1;
  result = codec::build_distributed_location_index(all, key_limit);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
}

TEST(distributed_location_index_resolves_partition_order_and_missing_members) {
  const auto a = exact_record("index/resolve", 1, "a");
  const auto b = exact_record("index/resolve", 2, "bb");
  const auto c = exact_record("index/resolve", 3, "ccc");
  const std::vector<codec::ExtractedRecord> inputs{a, b, c};
  const auto partition = one_partition(inputs);

  const auto c0 = location_for(c, "store", "c", "v1", 30);
  const auto a0 = location_for(a, "store", "a", "v1", 10);
  const std::vector<codec::DistributedRecordLocation> locations{c0, a0};
  auto index = codec::build_distributed_location_index(locations);
  EXPECT_TRUE(index);

  auto resolved = codec::resolve_partition_location_candidates(*index, partition);
  EXPECT_TRUE(resolved);
  EXPECT_EQ(resolved->partition_identity, partition.identity);
  EXPECT_EQ(resolved->stream, partition.stream);
  EXPECT_FALSE(resolved->complete);
  EXPECT_EQ(resolved->records.size(), std::size_t{3});
  EXPECT_EQ(resolved->records[0].record.sequence, std::uint64_t{1});
  EXPECT_EQ(resolved->records[1].record.sequence, std::uint64_t{2});
  EXPECT_EQ(resolved->records[2].record.sequence, std::uint64_t{3});
  EXPECT_EQ(resolved->records[0].candidates.size(), std::size_t{1});
  EXPECT_TRUE(resolved->records[1].candidates.empty());
  EXPECT_EQ(resolved->records[2].candidates.size(), std::size_t{1});
  expect_same_location(resolved->records[0].candidates[0], a0);
  expect_same_location(resolved->records[2].candidates[0], c0);
}

TEST(distributed_location_index_complete_resolution_preserves_unknown_type) {
  auto a = exact_record("index/complete", 1, "a");
  constexpr codec::RecordTypeCode raw_type = 0x3456;
  a.record.type = static_cast<codec::RecordType>(raw_type);
  const std::vector<codec::ExtractedRecord> inputs{a};
  const auto partition = one_partition(inputs);
  const auto a0 = location_for(a, "store", "a", "v1", 10);
  auto index = codec::build_distributed_location_index(
      std::vector<codec::DistributedRecordLocation>{a0});
  EXPECT_TRUE(index);

  auto resolved = codec::resolve_partition_location_candidates(*index, partition);
  EXPECT_TRUE(resolved);
  EXPECT_TRUE(resolved->complete);
  EXPECT_EQ(resolved->records.size(), std::size_t{1});
  EXPECT_EQ(resolved->records[0].record.type, raw_type);
  EXPECT_EQ(resolved->records[0].candidates[0].record.type_code(), raw_type);
}

TEST(distributed_location_index_requires_exact_type_and_hash_match) {
  const auto wanted = exact_record("index/exact-match", 9, "wanted");
  auto other = wanted;
  other.record.type = codec::RecordType::stream_descriptor;
  const auto other_location = location_for(other, "store", "other", "", 0);
  auto index = codec::build_distributed_location_index(
      std::vector<codec::DistributedRecordLocation>{other_location});
  EXPECT_TRUE(index);
  const auto partition = one_partition(
      std::vector<codec::ExtractedRecord>{wanted});

  auto resolved = codec::resolve_partition_location_candidates(*index, partition);
  EXPECT_TRUE(resolved);
  EXPECT_FALSE(resolved->complete);
  EXPECT_TRUE(resolved->records[0].candidates.empty());

  auto other_hash = wanted;
  other_hash.record.hash = codec::sha256(bytes("different"));
  const auto hash_location = location_for(other_hash, "store", "hash", "", 0);
  index = codec::build_distributed_location_index(
      std::vector<codec::DistributedRecordLocation>{hash_location});
  EXPECT_TRUE(index);
  resolved = codec::resolve_partition_location_candidates(*index, partition);
  EXPECT_TRUE(resolved);
  EXPECT_FALSE(resolved->complete);
  EXPECT_TRUE(resolved->records[0].candidates.empty());
}

TEST(distributed_location_index_rejects_tampered_partition) {
  const auto a = exact_record("index/tamper", 1, "a");
  const auto a0 = location_for(a, "store", "a", "", 0);
  auto index = codec::build_distributed_location_index(
      std::vector<codec::DistributedRecordLocation>{a0});
  EXPECT_TRUE(index);
  auto partition = one_partition(std::vector<codec::ExtractedRecord>{a});
  partition.identity[0] ^= 0xffU;

  auto resolved = codec::resolve_partition_location_candidates(*index, partition);
  EXPECT_FALSE(resolved);
  EXPECT_EQ(resolved.error().code, codec::ErrorCode::invalid_argument);
}

TEST(distributed_location_index_enforces_query_limits_without_truncation) {
  const auto a = exact_record("index/query-limit", 1, "a");
  const auto b = exact_record("index/query-limit", 2, "b");
  const auto a0 = location_for(a, "store", "a0", "", 0);
  const auto a1 = location_for(a, "store", "a1", "", 1);
  const auto b0 = location_for(b, "store", "b0", "", 2);
  auto index = codec::build_distributed_location_index(
      std::vector<codec::DistributedRecordLocation>{a0, a1, b0});
  EXPECT_TRUE(index);
  const auto partition = one_partition(
      std::vector<codec::ExtractedRecord>{a, b});

  codec::DistributedLocationQueryLimits zero;
  zero.maximum_records = 0;
  auto resolved = codec::resolve_partition_location_candidates(
      *index, partition, zero);
  EXPECT_FALSE(resolved);
  EXPECT_EQ(resolved.error().code, codec::ErrorCode::invalid_argument);

  codec::DistributedLocationQueryLimits record_limit;
  record_limit.maximum_records = 1;
  resolved = codec::resolve_partition_location_candidates(
      *index, partition, record_limit);
  EXPECT_FALSE(resolved);
  EXPECT_EQ(resolved.error().code, codec::ErrorCode::resource_exhausted);

  codec::DistributedLocationQueryLimits per_record_limit;
  per_record_limit.maximum_candidates_per_record = 1;
  resolved = codec::resolve_partition_location_candidates(
      *index, partition, per_record_limit);
  EXPECT_FALSE(resolved);
  EXPECT_EQ(resolved.error().code, codec::ErrorCode::resource_exhausted);

  codec::DistributedLocationQueryLimits aggregate_limit;
  aggregate_limit.maximum_candidates = 2;
  resolved = codec::resolve_partition_location_candidates(
      *index, partition, aggregate_limit);
  EXPECT_FALSE(resolved);
  EXPECT_EQ(resolved.error().code, codec::ErrorCode::resource_exhausted);
}

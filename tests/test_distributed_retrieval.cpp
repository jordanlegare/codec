#include "test.hpp"

#include <codec/distributed.hpp>
#include <codec/integrity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
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
    std::string key,
    std::uint64_t offset) {
  return codec::DistributedRecordLocation{
      .record = input.record,
      .object = {.store = "store-a", .key = std::move(key), .version = "v1"},
      .offset = offset,
      .length = input.record.payload_size,
  };
}

struct RangeCall {
  codec::ObjectStoreObjectRef object;
  std::uint64_t offset{};
  std::uint64_t length{};
};

enum class BackendMode {
  normal,
  short_result,
  long_result,
  wrong_same_length,
};

std::string object_id(const codec::ObjectStoreObjectRef& object) {
  return object.store + "\n" + object.key + "\n" + object.version;
}

class MemoryRangeBackend final : public codec::ObjectStoreBackend {
 public:
  std::string name() const override { return backend_name; }

  codec::Result<std::vector<std::byte>> read_range(
      const codec::ObjectStoreObjectRef& object,
      std::uint64_t offset,
      std::uint64_t length) override {
    calls.push_back(RangeCall{object, offset, length});
    if (failure.has_value()) return *failure;

    if (mode == BackendMode::short_result) {
      const auto count = length == 0 ? 0 : length - 1;
      return std::vector<std::byte>(static_cast<std::size_t>(count),
                                    std::byte{0x11});
    }
    if (mode == BackendMode::long_result) {
      return std::vector<std::byte>(static_cast<std::size_t>(length + 1),
                                    std::byte{0x22});
    }
    if (mode == BackendMode::wrong_same_length) {
      return std::vector<std::byte>(static_cast<std::size_t>(length),
                                    std::byte{0x33});
    }

    const auto found = objects.find(object_id(object));
    if (found == objects.end()) {
      return codec::fail<std::vector<std::byte>>(
          codec::ErrorCode::network, "object missing", true);
    }
    const auto object_size = static_cast<std::uint64_t>(found->second.size());
    if (offset > object_size || length > object_size - offset) {
      return codec::fail<std::vector<std::byte>>(
          codec::ErrorCode::network, "object range unavailable", true);
    }
    const auto begin =
        found->second.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = begin + static_cast<std::ptrdiff_t>(length);
    return std::vector<std::byte>{begin, end};
  }

  void put(const codec::ObjectStoreObjectRef& object,
           std::vector<std::byte> payload) {
    objects[object_id(object)] = std::move(payload);
  }

  std::string backend_name{"memory-range"};
  std::map<std::string, std::vector<std::byte>> objects;
  std::optional<codec::Error> failure;
  BackendMode mode{BackendMode::normal};
  std::vector<RangeCall> calls;
};

codec::ProvenanceProcess process_identity() {
  return codec::ProvenanceProcess{
      .operation = "f3-test",
      .implementation_id = "codec-test",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 1,
      .details_type = {},
      .details = {},
  };
}

class CountingProcessor final : public codec::StreamProcessor {
 public:
  std::string name() const override { return "f3-consumer"; }

  codec::Result<std::vector<codec::ProcessorOutput>> process(
      std::span<const codec::ExtractedRecord> inputs) override {
    ++calls;
    return std::vector<codec::ProcessorOutput>{codec::ProcessorOutput{
        .stream = inputs.front().record.stream,
        .type = 0x7b01,
        .start_ns = inputs.front().record.start_ns,
        .end_ns = inputs.back().record.end_ns,
        .truth = codec::TruthClass::derived,
        .payload = bytes("ok"),
        .process = process_identity(),
    }};
  }

  std::size_t calls{};
};

void expect_record_equal(const codec::ExtractedRecord& actual,
                         const codec::ExtractedRecord& expected) {
  EXPECT_EQ(actual.record.type_code(), expected.record.type_code());
  EXPECT_EQ(actual.record.sequence, expected.record.sequence);
  EXPECT_EQ(actual.record.stream, expected.record.stream);
  EXPECT_EQ(actual.record.start_ns, expected.record.start_ns);
  EXPECT_EQ(actual.record.end_ns, expected.record.end_ns);
  EXPECT_EQ(actual.record.payload_size, expected.record.payload_size);
  EXPECT_EQ(actual.record.file_offset, expected.record.file_offset);
  EXPECT_EQ(actual.record.hash, expected.record.hash);
  EXPECT_EQ(actual.payload, expected.payload);
}

void expect_pre_read_error(
    MemoryRangeBackend& backend,
    const codec::DistributedPartition& partition,
    std::span<const codec::DistributedRecordLocation> locations,
    codec::ErrorCode expected,
    codec::DistributedRetrievalLimits limits = {}) {
  const auto before = backend.calls.size();
  auto result = codec::retrieve_partition_records(
      backend, partition, locations, limits);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, expected);
  EXPECT_EQ(backend.calls.size(), before);
}

}  // namespace

TEST(distributed_retrieval_materializes_exact_partition_and_hands_off_to_f2) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f3/valid", 1, "alpha"),
      exact_record("f3/valid", 2, "beta")};
  const auto partition = one_partition(inputs);
  std::vector<codec::DistributedRecordLocation> locations{
      location_for(inputs[0], "alpha-object", 2),
      location_for(inputs[1], "beta-object", 2)};

  MemoryRangeBackend backend;
  backend.put(locations[0].object, bytes("xxalpha-tail"));
  backend.put(locations[1].object, bytes("yybeta-tail"));

  auto retrieved = codec::retrieve_partition_records(
      backend, partition, locations);
  EXPECT_TRUE(retrieved);
  EXPECT_EQ(retrieved->partition_identity, partition.identity);
  EXPECT_EQ(retrieved->stream, partition.stream);
  EXPECT_EQ(retrieved->backend_name, std::string{"memory-range"});
  EXPECT_EQ(retrieved->records.size(), inputs.size());
  expect_record_equal(retrieved->records[0], inputs[0]);
  expect_record_equal(retrieved->records[1], inputs[1]);

  CountingProcessor processor;
  codec::LocalProcessorWorker worker{processor, "local-f3-worker"};
  auto execution = codec::execute_partition(
      worker, partition, retrieved->records);
  EXPECT_TRUE(execution);
  EXPECT_EQ(processor.calls, std::size_t{1});
  EXPECT_EQ(execution->partition_identity, partition.identity);
  EXPECT_EQ(execution->outputs.size(), std::size_t{1});
  EXPECT_EQ(execution->outputs.front().payload, bytes("ok"));
}

TEST(distributed_retrieval_preserves_record_metadata_and_provider_ranges) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f3/metadata", 7, "record")};
  const auto partition = one_partition(inputs);
  std::vector<codec::DistributedRecordLocation> locations{
      location_for(inputs[0], "metadata-object", 5)};
  locations[0].object.store = "archive-store";
  locations[0].object.version = "generation-42";

  MemoryRangeBackend backend;
  backend.put(locations[0].object, bytes("12345record-trailer"));

  auto retrieved = codec::retrieve_partition_records(
      backend, partition, locations);
  EXPECT_TRUE(retrieved);
  EXPECT_EQ(backend.calls.size(), std::size_t{1});
  EXPECT_EQ(backend.calls[0].object.store, std::string{"archive-store"});
  EXPECT_EQ(backend.calls[0].object.key, std::string{"metadata-object"});
  EXPECT_EQ(backend.calls[0].object.version, std::string{"generation-42"});
  EXPECT_EQ(backend.calls[0].offset, std::uint64_t{5});
  EXPECT_EQ(backend.calls[0].length, inputs[0].record.payload_size);
  expect_record_equal(retrieved->records[0], inputs[0]);
  EXPECT_EQ(retrieved->records[0].record.file_offset,
            inputs[0].record.file_offset);
  EXPECT_FALSE(retrieved->records[0].record.file_offset == locations[0].offset);
}

TEST(distributed_retrieval_rejects_tampered_partition_before_reads) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f3/partition-tamper", 1, "one"),
      exact_record("f3/partition-tamper", 2, "two")};
  const auto original = one_partition(inputs);
  const std::vector<codec::DistributedRecordLocation> locations{
      location_for(inputs[0], "one", 0),
      location_for(inputs[1], "two", 0)};
  MemoryRangeBackend backend;

  auto wrong_identity = original;
  wrong_identity.identity[0] ^= 0x01U;
  expect_pre_read_error(backend, wrong_identity, locations,
                        codec::ErrorCode::invalid_argument);

  auto wrong_link_stream = original;
  wrong_link_stream.records.front().stream =
      codec::derive_stream_id("f3/other");
  expect_pre_read_error(backend, wrong_link_stream, locations,
                        codec::ErrorCode::invalid_argument);

  auto wrong_payload_total = original;
  ++wrong_payload_total.payload_bytes;
  expect_pre_read_error(backend, wrong_payload_total, locations,
                        codec::ErrorCode::invalid_argument);
}

TEST(distributed_retrieval_rejects_wrong_or_reordered_locations_before_reads) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f3/order", 1, "one"),
      exact_record("f3/order", 2, "two")};
  const auto partition = one_partition(inputs);
  const std::vector<codec::DistributedRecordLocation> original{
      location_for(inputs[0], "one", 0),
      location_for(inputs[1], "two", 0)};
  MemoryRangeBackend backend;

  auto reordered = original;
  std::reverse(reordered.begin(), reordered.end());
  expect_pre_read_error(backend, partition, reordered,
                        codec::ErrorCode::invalid_argument);

  std::vector<codec::DistributedRecordLocation> missing{original.front()};
  expect_pre_read_error(backend, partition, missing,
                        codec::ErrorCode::invalid_argument);

  auto extra = original;
  extra.push_back(original.back());
  expect_pre_read_error(backend, partition, extra,
                        codec::ErrorCode::invalid_argument);

  auto wrong_sequence = original;
  ++wrong_sequence.front().record.sequence;
  expect_pre_read_error(backend, partition, wrong_sequence,
                        codec::ErrorCode::invalid_argument);

  auto wrong_hash = original;
  wrong_hash.front().record.hash[0] ^= 0x01U;
  expect_pre_read_error(backend, partition, wrong_hash,
                        codec::ErrorCode::invalid_argument);

  auto wrong_stream = original;
  wrong_stream.front().record.stream = codec::derive_stream_id("f3/other");
  expect_pre_read_error(backend, partition, wrong_stream,
                        codec::ErrorCode::invalid_argument);
}

TEST(distributed_retrieval_rejects_invalid_location_shape_before_reads) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f3/location-shape", 1, "abcd")};
  const auto partition = one_partition(inputs);
  const std::vector<codec::DistributedRecordLocation> original{
      location_for(inputs[0], "object", 0)};
  MemoryRangeBackend backend;

  auto bad_interval = original;
  bad_interval.front().record.start_ns = 20;
  bad_interval.front().record.end_ns = 10;
  expect_pre_read_error(backend, partition, bad_interval,
                        codec::ErrorCode::invalid_argument);

  auto bad_length = original;
  --bad_length.front().length;
  expect_pre_read_error(backend, partition, bad_length,
                        codec::ErrorCode::invalid_argument);

  auto overflow = original;
  overflow.front().offset = std::numeric_limits<std::uint64_t>::max() - 1;
  expect_pre_read_error(backend, partition, overflow,
                        codec::ErrorCode::invalid_argument);

  auto empty_store = original;
  empty_store.front().object.store.clear();
  expect_pre_read_error(backend, partition, empty_store,
                        codec::ErrorCode::invalid_argument);

  auto empty_key = original;
  empty_key.front().object.key.clear();
  expect_pre_read_error(backend, partition, empty_key,
                        codec::ErrorCode::invalid_argument);
}

TEST(distributed_retrieval_validates_limits_and_labels_before_reads) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f3/limits", 1, "abcd")};
  const auto partition = one_partition(inputs);
  const std::vector<codec::DistributedRecordLocation> original{
      location_for(inputs[0], "object", 0)};

  {
    MemoryRangeBackend backend;
    codec::DistributedRetrievalLimits limits;
    limits.maximum_records = 0;
    expect_pre_read_error(backend, partition, original,
                          codec::ErrorCode::invalid_argument, limits);
  }
  {
    MemoryRangeBackend backend;
    codec::DistributedRetrievalLimits limits;
    limits.maximum_bytes = 0;
    expect_pre_read_error(backend, partition, original,
                          codec::ErrorCode::invalid_argument, limits);
  }
  {
    MemoryRangeBackend backend;
    codec::DistributedRetrievalLimits limits;
    limits.maximum_backend_name_bytes = 0;
    expect_pre_read_error(backend, partition, original,
                          codec::ErrorCode::invalid_argument, limits);
  }
  {
    MemoryRangeBackend backend;
    codec::DistributedRetrievalLimits limits;
    limits.maximum_store_bytes = 0;
    expect_pre_read_error(backend, partition, original,
                          codec::ErrorCode::invalid_argument, limits);
  }
  {
    MemoryRangeBackend backend;
    codec::DistributedRetrievalLimits limits;
    limits.maximum_key_bytes = 0;
    expect_pre_read_error(backend, partition, original,
                          codec::ErrorCode::invalid_argument, limits);
  }
  {
    MemoryRangeBackend backend;
    codec::DistributedRetrievalLimits limits;
    limits.maximum_version_bytes = 0;
    expect_pre_read_error(backend, partition, original,
                          codec::ErrorCode::invalid_argument, limits);
  }
  {
    const std::vector<codec::ExtractedRecord> two_inputs{
        exact_record("f3/count", 1, "a"),
        exact_record("f3/count", 2, "b")};
    const auto two_partition = one_partition(two_inputs);
    const std::vector<codec::DistributedRecordLocation> two_locations{
        location_for(two_inputs[0], "a", 0),
        location_for(two_inputs[1], "b", 0)};
    MemoryRangeBackend backend;
    codec::DistributedRetrievalLimits limits;
    limits.maximum_records = 1;
    expect_pre_read_error(backend, two_partition, two_locations,
                          codec::ErrorCode::resource_exhausted, limits);
  }
  {
    MemoryRangeBackend backend;
    codec::DistributedRetrievalLimits limits;
    limits.maximum_bytes = 3;
    expect_pre_read_error(backend, partition, original,
                          codec::ErrorCode::resource_exhausted, limits);
  }
  {
    MemoryRangeBackend backend;
    backend.backend_name.clear();
    expect_pre_read_error(backend, partition, original,
                          codec::ErrorCode::invalid_argument);
  }
  {
    MemoryRangeBackend backend;
    backend.backend_name = "backend-name";
    codec::DistributedRetrievalLimits limits;
    limits.maximum_backend_name_bytes = 4;
    expect_pre_read_error(backend, partition, original,
                          codec::ErrorCode::resource_exhausted, limits);
  }
  {
    MemoryRangeBackend backend;
    auto locations = original;
    locations.front().object.store = "store-too-long";
    codec::DistributedRetrievalLimits limits;
    limits.maximum_store_bytes = 4;
    expect_pre_read_error(backend, partition, locations,
                          codec::ErrorCode::resource_exhausted, limits);
  }
  {
    MemoryRangeBackend backend;
    auto locations = original;
    locations.front().object.key = "key-too-long";
    codec::DistributedRetrievalLimits limits;
    limits.maximum_key_bytes = 4;
    expect_pre_read_error(backend, partition, locations,
                          codec::ErrorCode::resource_exhausted, limits);
  }
  {
    MemoryRangeBackend backend;
    auto locations = original;
    locations.front().object.version = "version-too-long";
    codec::DistributedRetrievalLimits limits;
    limits.maximum_version_bytes = 4;
    expect_pre_read_error(backend, partition, locations,
                          codec::ErrorCode::resource_exhausted, limits);
  }
}

TEST(distributed_retrieval_propagates_provider_error_without_retry) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f3/provider-error", 1, "one")};
  const auto partition = one_partition(inputs);
  const std::vector<codec::DistributedRecordLocation> locations{
      location_for(inputs[0], "one", 0)};
  MemoryRangeBackend backend;
  backend.failure = codec::Error{codec::ErrorCode::network,
                                 "remote unavailable", true};

  auto result = codec::retrieve_partition_records(backend, partition, locations);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::network);
  EXPECT_EQ(result.error().message, std::string{"remote unavailable"});
  EXPECT_TRUE(result.error().retryable);
  EXPECT_EQ(backend.calls.size(), std::size_t{1});
}

TEST(distributed_retrieval_rejects_short_or_long_provider_ranges) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f3/range-size", 1, "abcd")};
  const auto partition = one_partition(inputs);
  const std::vector<codec::DistributedRecordLocation> locations{
      location_for(inputs[0], "object", 0)};

  {
    MemoryRangeBackend backend;
    backend.mode = BackendMode::short_result;
    auto result = codec::retrieve_partition_records(backend, partition, locations);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::protocol);
    EXPECT_EQ(backend.calls.size(), std::size_t{1});
  }
  {
    MemoryRangeBackend backend;
    backend.mode = BackendMode::long_result;
    auto result = codec::retrieve_partition_records(backend, partition, locations);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::protocol);
    EXPECT_EQ(backend.calls.size(), std::size_t{1});
  }
}

TEST(distributed_retrieval_rejects_same_length_wrong_payload_hash) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f3/hash", 1, "abcd")};
  const auto partition = one_partition(inputs);
  const std::vector<codec::DistributedRecordLocation> locations{
      location_for(inputs[0], "object", 0)};
  MemoryRangeBackend backend;
  backend.mode = BackendMode::wrong_same_length;

  auto result = codec::retrieve_partition_records(backend, partition, locations);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::archive_corrupt);
  EXPECT_EQ(backend.calls.size(), std::size_t{1});
}

TEST(distributed_retrieval_supports_zero_length_exact_record) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f3/empty", 1, "")};
  const auto partition = one_partition(inputs);
  const std::vector<codec::DistributedRecordLocation> locations{
      location_for(inputs[0], "empty-object", 0)};
  MemoryRangeBackend backend;
  backend.put(locations[0].object, {});

  auto result = codec::retrieve_partition_records(backend, partition, locations);
  EXPECT_TRUE(result);
  EXPECT_EQ(result->records.size(), std::size_t{1});
  EXPECT_TRUE(result->records.front().payload.empty());
  EXPECT_EQ(backend.calls.size(), std::size_t{1});
  EXPECT_EQ(backend.calls.front().length, std::uint64_t{0});
}

TEST(distributed_retrieval_preserves_unknown_record_type_code) {
  std::vector<codec::ExtractedRecord> inputs{
      exact_record("f3/unknown", 1, "opaque")};
  inputs[0].record.type = static_cast<codec::RecordType>(0x7d01);
  const auto partition = one_partition(inputs);
  const std::vector<codec::DistributedRecordLocation> locations{
      location_for(inputs[0], "opaque-object", 0)};
  MemoryRangeBackend backend;
  backend.put(locations[0].object, inputs[0].payload);

  auto result = codec::retrieve_partition_records(backend, partition, locations);
  EXPECT_TRUE(result);
  EXPECT_EQ(result->records.front().record.type_code(),
            codec::RecordTypeCode{0x7d01});
  EXPECT_EQ(result->records.front().payload, inputs[0].payload);
}

#include <codec/distributed.hpp>

#include "internal.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <vector>

namespace codec {
namespace {

Result<void> validate_limits(const DistributedPartitionLimits& limits) {
  if (limits.maximum_input_records == 0 ||
      limits.maximum_input_bytes == 0 || limits.maximum_partitions == 0 ||
      limits.maximum_records_per_partition == 0 ||
      limits.maximum_payload_bytes_per_partition == 0) {
    return fail(ErrorCode::invalid_argument,
                "distributed partition limits must be non-zero");
  }
  return {};
}

}  // namespace

Result<std::vector<DistributedPartition>> partition_exact_records(
    std::span<const ExtractedRecord> inputs, DistributedPartitionLimits limits) {
  auto valid_limits = validate_limits(limits);
  if (!valid_limits) return valid_limits.error();
  if (inputs.size() > limits.maximum_input_records) {
    return fail<std::vector<DistributedPartition>>(
        ErrorCode::resource_exhausted,
        "distributed partition input-count limit exceeded");
  }

  std::uint64_t total_input_bytes = 0;
  for (const auto& input : inputs) {
    if (input.payload.size() != input.record.payload_size) {
      return fail<std::vector<DistributedPartition>>(
          ErrorCode::invalid_argument,
          "distributed partition input payload does not match its exact record size");
    }
    const auto input_bytes = static_cast<std::uint64_t>(input.payload.size());
    if (input_bytes > limits.maximum_payload_bytes_per_partition) {
      return fail<std::vector<DistributedPartition>>(
          ErrorCode::resource_exhausted,
          "distributed partition record exceeds the per-partition byte limit");
    }
    if (input_bytes > limits.maximum_input_bytes - total_input_bytes) {
      return fail<std::vector<DistributedPartition>>(
          ErrorCode::resource_exhausted,
          "distributed partition aggregate input-byte limit exceeded");
    }
    total_input_bytes += input_bytes;
  }

  if (inputs.empty()) return std::vector<DistributedPartition>{};

  try {
    std::vector<DistributedPartition> partitions;
    std::map<StreamId, std::size_t> open_partitions;

    for (const auto& input : inputs) {
      const auto input_bytes = static_cast<std::uint64_t>(input.payload.size());
      auto open = open_partitions.find(input.record.stream);
      bool needs_new_partition = open == open_partitions.end();

      if (!needs_new_partition) {
        const auto& current = partitions[open->second];
        needs_new_partition =
            current.records.size() >= limits.maximum_records_per_partition ||
            input_bytes >
                limits.maximum_payload_bytes_per_partition -
                    current.payload_bytes;
      }

      if (needs_new_partition) {
        if (partitions.size() >= limits.maximum_partitions) {
          return fail<std::vector<DistributedPartition>>(
              ErrorCode::resource_exhausted,
              "distributed partition count limit exceeded");
        }
        DistributedPartition created;
        created.stream = input.record.stream;
        partitions.push_back(std::move(created));
        const auto partition_index = partitions.size() - 1;
        if (open == open_partitions.end()) {
          open_partitions.emplace(input.record.stream, partition_index);
        } else {
          open->second = partition_index;
        }
        open = open_partitions.find(input.record.stream);
      }

      auto& target = partitions[open->second];
      target.records.push_back(detail::distributed_exact_link(input));
      target.payload_bytes += input_bytes;
    }

    for (auto& partition : partitions) {
      partition.identity = detail::distributed_partition_identity(
          partition.stream, partition.records);
    }
    return partitions;
  } catch (const std::bad_alloc&) {
    return fail<std::vector<DistributedPartition>>(
        ErrorCode::resource_exhausted,
        "distributed partition allocation failed");
  }
}

}  // namespace codec

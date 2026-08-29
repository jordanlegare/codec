#include <codec/distributed.hpp>

#include "internal.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <set>
#include <utility>
#include <vector>

namespace codec {
namespace {

Result<void> validate_scheduling_limits(
    const DistributedSchedulingLimits& limits) {
  if (limits.maximum_partitions == 0 || limits.maximum_workers == 0 ||
      limits.maximum_total_records == 0 ||
      limits.maximum_total_payload_bytes == 0) {
    return fail(ErrorCode::invalid_argument,
                "distributed scheduling limits must be non-zero");
  }
  return {};
}

}  // namespace

Result<DistributedScheduleResult> schedule_partitions(
    std::span<DistributedWorker* const> workers,
    ObjectStoreBackend& backend,
    const DistributedLocationIndex& index,
    std::span<const DistributedPartition> partitions,
    DistributedSchedulingLimits limits) {
  try {
    auto valid_limits = validate_scheduling_limits(limits);
    if (!valid_limits) return valid_limits.error();

    if (partitions.empty()) return DistributedScheduleResult{};
    if (workers.empty()) {
      return fail<DistributedScheduleResult>(
          ErrorCode::invalid_argument,
          "distributed scheduling requires at least one worker");
    }
    if (partitions.size() > limits.maximum_partitions) {
      return fail<DistributedScheduleResult>(
          ErrorCode::resource_exhausted,
          "distributed scheduling partition-count limit exceeded");
    }
    if (workers.size() > limits.maximum_workers) {
      return fail<DistributedScheduleResult>(
          ErrorCode::resource_exhausted,
          "distributed scheduling worker-count limit exceeded");
    }
    for (auto* worker : workers) {
      if (worker == nullptr) {
        return fail<DistributedScheduleResult>(
            ErrorCode::invalid_argument,
            "distributed scheduling worker pool contains a null worker");
      }
    }

    std::size_t total_records = 0;
    std::uint64_t total_payload_bytes = 0;
    std::set<Sha256> partition_identities;
    for (const auto& partition : partitions) {
      if (partition.records.empty()) {
        return fail<DistributedScheduleResult>(
            ErrorCode::invalid_argument,
            "distributed scheduling requires non-empty partitions");
      }
      for (const auto& link : partition.records) {
        if (link.stream != partition.stream) {
          return fail<DistributedScheduleResult>(
              ErrorCode::invalid_argument,
              "distributed scheduling partition contains another stream");
        }
      }
      if (detail::distributed_partition_identity(partition.stream,
                                                 partition.records) !=
          partition.identity) {
        return fail<DistributedScheduleResult>(
            ErrorCode::invalid_argument,
            "distributed scheduling partition identity does not match membership");
      }
      if (!partition_identities.insert(partition.identity).second) {
        return fail<DistributedScheduleResult>(
            ErrorCode::invalid_argument,
            "distributed scheduling batch contains a duplicate partition identity");
      }
      if (partition.records.size() >
          limits.maximum_total_records - total_records) {
        return fail<DistributedScheduleResult>(
            ErrorCode::resource_exhausted,
            "distributed scheduling aggregate record-count limit exceeded");
      }
      total_records += partition.records.size();
      if (partition.payload_bytes >
          limits.maximum_total_payload_bytes - total_payload_bytes) {
        return fail<DistributedScheduleResult>(
            ErrorCode::resource_exhausted,
            "distributed scheduling aggregate payload-byte limit exceeded");
      }
      total_payload_bytes += partition.payload_bytes;
    }

    DistributedScheduleResult result;
    result.partitions.reserve(partitions.size());

    for (std::size_t partition_index = 0;
         partition_index < partitions.size(); ++partition_index) {
      const auto& partition = partitions[partition_index];
      const auto worker_index = partition_index % workers.size();
      auto* worker = workers[worker_index];

      DistributedPartitionOutcome outcome{
          .partition_identity = partition.identity,
          .stream = partition.stream,
          .partition_index = partition_index,
          .worker_index = worker_index,
          .status = DistributedPartitionOutcomeStatus::location_unavailable,
          .selected_locations = {},
          .execution = std::nullopt,
          .error = std::nullopt,
      };

      auto resolved = resolve_partition_location_candidates(
          index, partition, limits.location_query);
      if (!resolved) {
        outcome.status =
            DistributedPartitionOutcomeStatus::location_unavailable;
        outcome.error = resolved.error();
        result.partitions.push_back(std::move(outcome));
        continue;
      }
      if (!resolved->complete) {
        outcome.status =
            DistributedPartitionOutcomeStatus::location_unavailable;
        outcome.error = Error{
            ErrorCode::network,
            "distributed scheduling has no complete indexed placement for the partition",
            false,
        };
        result.partitions.push_back(std::move(outcome));
        continue;
      }
      if (resolved->records.size() != partition.records.size()) {
        return fail<DistributedScheduleResult>(
            ErrorCode::internal,
            "distributed location resolution returned inconsistent partition membership");
      }

      outcome.selected_locations.reserve(resolved->records.size());
      for (const auto& candidate_set : resolved->records) {
        if (candidate_set.candidates.empty()) {
          return fail<DistributedScheduleResult>(
              ErrorCode::internal,
              "complete distributed location resolution returned an empty candidate set");
        }
        outcome.selected_locations.push_back(candidate_set.candidates.front());
      }

      auto retrieved = retrieve_partition_records(
          backend, partition, outcome.selected_locations, limits.retrieval);
      if (!retrieved) {
        outcome.status = DistributedPartitionOutcomeStatus::retrieval_failed;
        outcome.error = retrieved.error();
        result.partitions.push_back(std::move(outcome));
        continue;
      }

      auto executed = execute_partition(
          *worker, partition, retrieved->records, limits.execution);
      if (!executed) {
        outcome.status = DistributedPartitionOutcomeStatus::execution_failed;
        outcome.error = executed.error();
        result.partitions.push_back(std::move(outcome));
        continue;
      }

      outcome.status = DistributedPartitionOutcomeStatus::succeeded;
      outcome.execution = std::move(*executed);
      ++result.succeeded;
      result.partitions.push_back(std::move(outcome));
    }

    return result;
  } catch (const std::bad_alloc&) {
    return fail<DistributedScheduleResult>(
        ErrorCode::resource_exhausted,
        "distributed scheduling allocation failed");
  }
}

}  // namespace codec

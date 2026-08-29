#include <codec/distributed.hpp>

#include "internal.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <utility>

namespace codec {
namespace {

bool same_link(const ProvenanceRecordLink& lhs,
               const ProvenanceRecordLink& rhs) {
  return lhs.stream == rhs.stream && lhs.type == rhs.type &&
         lhs.sequence == rhs.sequence && lhs.hash == rhs.hash;
}

class WorkerProcessorProxy final : public StreamProcessor {
 public:
  WorkerProcessorProxy(DistributedWorker& worker, std::string processor_name)
      : worker_(worker), processor_name_(std::move(processor_name)) {}

  std::string name() const override { return processor_name_; }

  Result<std::vector<ProcessorOutput>> process(
      std::span<const ExtractedRecord> inputs) override {
    return worker_.execute(inputs);
  }

 private:
  DistributedWorker& worker_;
  std::string processor_name_;
};

Result<void> validate_execution_limits(const DistributedExecutionLimits& limits) {
  if (limits.maximum_input_records == 0 || limits.maximum_input_bytes == 0 ||
      limits.maximum_worker_name_bytes == 0 ||
      limits.maximum_processor_name_bytes == 0 ||
      limits.processor.maximum_outputs == 0 ||
      limits.processor.maximum_output_bytes == 0) {
    return fail(ErrorCode::invalid_argument,
                "distributed execution limits must be non-zero");
  }
  return {};
}

}  // namespace

LocalProcessorWorker::LocalProcessorWorker(StreamProcessor& processor,
                                           std::string worker_name)
    : processor_(&processor), worker_name_(std::move(worker_name)) {}

std::string LocalProcessorWorker::name() const { return worker_name_; }

std::string LocalProcessorWorker::processor_name() const {
  return processor_->name();
}

Result<std::vector<ProcessorOutput>> LocalProcessorWorker::execute(
    std::span<const ExtractedRecord> inputs) {
  return processor_->process(inputs);
}

Result<DistributedExecutionResult> execute_partition(
    DistributedWorker& worker,
    const DistributedPartition& partition,
    std::span<const ExtractedRecord> inputs,
    DistributedExecutionLimits limits) {
  auto valid_limits = validate_execution_limits(limits);
  if (!valid_limits) return valid_limits.error();

  if (partition.records.empty() || inputs.empty() ||
      partition.records.size() != inputs.size()) {
    return fail<DistributedExecutionResult>(
        ErrorCode::invalid_argument,
        "distributed execution requires one non-empty exact partition input set");
  }
  if (inputs.size() > limits.maximum_input_records) {
    return fail<DistributedExecutionResult>(
        ErrorCode::resource_exhausted,
        "distributed execution input-count limit exceeded");
  }

  std::string worker_name = worker.name();
  std::string processor_name = worker.processor_name();
  if (worker_name.empty() || processor_name.empty()) {
    return fail<DistributedExecutionResult>(
        ErrorCode::invalid_argument,
        "distributed worker and processor names must be non-empty");
  }
  if (worker_name.size() > limits.maximum_worker_name_bytes ||
      processor_name.size() > limits.maximum_processor_name_bytes) {
    return fail<DistributedExecutionResult>(
        ErrorCode::resource_exhausted,
        "distributed worker or processor name exceeds the configured limit");
  }

  for (const auto& link : partition.records) {
    if (link.stream != partition.stream) {
      return fail<DistributedExecutionResult>(
          ErrorCode::invalid_argument,
          "distributed partition contains a link from another stream");
    }
  }

  try {
    if (detail::distributed_partition_identity(partition.stream,
                                               partition.records) !=
        partition.identity) {
      return fail<DistributedExecutionResult>(
          ErrorCode::invalid_argument,
          "distributed partition identity does not match ordered membership");
    }
  } catch (const std::bad_alloc&) {
    return fail<DistributedExecutionResult>(
        ErrorCode::resource_exhausted,
        "distributed partition identity allocation failed");
  }

  std::uint64_t total_input_bytes = 0;
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    const auto& input = inputs[index];
    if (input.payload.size() != input.record.payload_size) {
      return fail<DistributedExecutionResult>(
          ErrorCode::invalid_argument,
          "distributed execution input payload size is inconsistent");
    }
    if (input.record.stream != partition.stream) {
      return fail<DistributedExecutionResult>(
          ErrorCode::invalid_argument,
          "distributed execution input belongs to another stream");
    }
    if (!same_link(detail::distributed_exact_link(input),
                   partition.records[index])) {
      return fail<DistributedExecutionResult>(
          ErrorCode::invalid_argument,
          "distributed execution input does not match ordered partition membership");
    }
    if (sha256(input.payload) != input.record.hash) {
      return fail<DistributedExecutionResult>(
          ErrorCode::archive_corrupt,
          "distributed execution input payload hash mismatch");
    }

    const auto input_bytes = static_cast<std::uint64_t>(input.payload.size());
    if (input_bytes > limits.maximum_input_bytes - total_input_bytes) {
      return fail<DistributedExecutionResult>(
          ErrorCode::resource_exhausted,
          "distributed execution input-byte limit exceeded");
    }
    total_input_bytes += input_bytes;
  }

  if (total_input_bytes != partition.payload_bytes) {
    return fail<DistributedExecutionResult>(
        ErrorCode::invalid_argument,
        "distributed partition payload-byte total is inconsistent");
  }

  WorkerProcessorProxy proxy{worker, processor_name};
  auto outputs = invoke_processor(proxy, inputs, limits.processor);
  if (!outputs) return outputs.error();

  return DistributedExecutionResult{
      .partition_identity = partition.identity,
      .stream = partition.stream,
      .worker_name = std::move(worker_name),
      .processor_name = std::move(processor_name),
      .outputs = std::move(*outputs),
  };
}

}  // namespace codec

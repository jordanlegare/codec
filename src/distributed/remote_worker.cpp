#include <codec/distributed.hpp>

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codec {
namespace {

Result<void> validate_remote_limits(
    const DistributedRemoteWorkerLimits& limits) {
  if (limits.maximum_input_records == 0 ||
      limits.maximum_input_bytes == 0 ||
      limits.maximum_transport_name_bytes == 0 ||
      limits.maximum_worker_name_bytes == 0 ||
      limits.maximum_processor_name_bytes == 0 ||
      limits.processor.maximum_outputs == 0 ||
      limits.processor.maximum_output_bytes == 0) {
    return fail(ErrorCode::invalid_argument,
                "distributed remote worker limits must be non-zero");
  }
  return {};
}

Result<void> validate_label(std::string_view value,
                            std::size_t maximum_bytes,
                            std::string_view kind) {
  if (value.empty()) {
    return fail(ErrorCode::invalid_argument,
                std::string{"distributed remote worker "} +
                    std::string{kind} + " must be non-empty");
  }
  if (value.size() > maximum_bytes) {
    return fail(ErrorCode::resource_exhausted,
                std::string{"distributed remote worker "} +
                    std::string{kind} + " exceeds configured limit");
  }
  return {};
}

}  // namespace

RemoteDistributedWorker::RemoteDistributedWorker(
    DistributedWorkerTransport& transport,
    std::string worker_name,
    std::string processor_name,
    DistributedRemoteWorkerLimits limits)
    : transport_(&transport),
      worker_name_(std::move(worker_name)),
      processor_name_(std::move(processor_name)),
      limits_(limits) {}

std::string RemoteDistributedWorker::name() const { return worker_name_; }

std::string RemoteDistributedWorker::processor_name() const {
  return processor_name_;
}

Result<std::vector<ProcessorOutput>> RemoteDistributedWorker::execute(
    std::span<const ExtractedRecord> inputs) {
  try {
    auto valid_limits = validate_remote_limits(limits_);
    if (!valid_limits) return valid_limits.error();
    if (transport_ == nullptr) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::internal,
          "distributed remote worker transport is unavailable");
    }

    const auto transport_name = transport_->name();
    auto valid_transport = validate_label(
        transport_name, limits_.maximum_transport_name_bytes,
        "transport name");
    if (!valid_transport) return valid_transport.error();
    auto valid_worker = validate_label(
        worker_name_, limits_.maximum_worker_name_bytes,
        "worker name");
    if (!valid_worker) return valid_worker.error();
    auto valid_processor = validate_label(
        processor_name_, limits_.maximum_processor_name_bytes,
        "processor name");
    if (!valid_processor) return valid_processor.error();

    if (inputs.empty()) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::invalid_argument,
          "distributed remote worker requires at least one input");
    }
    if (inputs.size() > limits_.maximum_input_records) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::resource_exhausted,
          "distributed remote worker input-count limit exceeded");
    }

    std::uint64_t total_input_bytes = 0;
    for (const auto& input : inputs) {
      const auto payload_size =
          static_cast<std::uint64_t>(input.payload.size());
      if (payload_size != input.record.payload_size) {
        return fail<std::vector<ProcessorOutput>>(
            ErrorCode::invalid_argument,
            "distributed remote worker input payload size does not match record metadata");
      }
      if (payload_size > limits_.maximum_input_bytes - total_input_bytes) {
        return fail<std::vector<ProcessorOutput>>(
            ErrorCode::resource_exhausted,
            "distributed remote worker aggregate input-byte limit exceeded");
      }
      total_input_bytes += payload_size;
    }

    auto response =
        transport_->dispatch(worker_name_, processor_name_, inputs);
    if (!response) return response.error();

    if (response->worker_name != worker_name_ ||
        response->processor_name != processor_name_) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::protocol,
          "distributed remote worker response identity mismatch");
    }
    if (response->outputs.size() > limits_.processor.maximum_outputs) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::resource_exhausted,
          "distributed remote worker output-count limit exceeded");
    }

    std::uint64_t total_output_bytes = 0;
    for (const auto& output : response->outputs) {
      const auto payload_size =
          static_cast<std::uint64_t>(output.payload.size());
      if (payload_size >
          limits_.processor.maximum_output_bytes - total_output_bytes) {
        return fail<std::vector<ProcessorOutput>>(
            ErrorCode::resource_exhausted,
            "distributed remote worker aggregate output-byte limit exceeded");
      }
      total_output_bytes += payload_size;
    }

    return std::move(response->outputs);
  } catch (const std::bad_alloc&) {
    return fail<std::vector<ProcessorOutput>>(
        ErrorCode::resource_exhausted,
        "distributed remote worker allocation failed");
  }
}

}  // namespace codec

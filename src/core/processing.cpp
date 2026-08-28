#include <codec/processing.hpp>

#include "internal.hpp"

#include <utility>

namespace codec {
namespace {

bool prohibited_artifact_type(RecordTypeCode type) {
  return type == record_type_code(RecordType::stream_provenance) ||
         type == record_type_code(RecordType::final_index);
}

constexpr std::size_t maximum_provenance_inputs = 4096;

}  // namespace

Result<std::optional<AdapterRecord>> pull_adapter_record(
    StreamAdapter& adapter, AdapterReadLimits limits) {
  if (limits.maximum_payload_bytes == 0) {
    return fail<std::optional<AdapterRecord>>(
        ErrorCode::invalid_argument,
        "adapter payload limit must be non-zero");
  }
  auto record = adapter.next();
  if (!record) return record.error();
  if (!*record) return std::optional<AdapterRecord>{};
  const auto& value = **record;
  if (value.end_ns < value.start_ns) {
    return fail<std::optional<AdapterRecord>>(
        ErrorCode::invalid_argument,
        "adapter record end time precedes start time");
  }
  if (prohibited_artifact_type(value.type)) {
    return fail<std::optional<AdapterRecord>>(
        ErrorCode::invalid_argument,
        "adapter record type is reserved for archive metadata");
  }
  if (value.payload.size() > limits.maximum_payload_bytes) {
    return fail<std::optional<AdapterRecord>>(
        ErrorCode::resource_exhausted,
        "adapter record exceeds the configured payload limit");
  }
  return std::move(*record);
}

Result<std::vector<ProcessorOutput>> invoke_processor(
    StreamProcessor& processor, std::span<const ExtractedRecord> inputs,
    ProcessorRunLimits limits) {
  if (inputs.empty() || inputs.size() > maximum_provenance_inputs) {
    return fail<std::vector<ProcessorOutput>>(
        ErrorCode::invalid_argument,
        "processor input count exceeds the provenance contract");
  }
  if (limits.maximum_outputs == 0 || limits.maximum_output_bytes == 0) {
    return fail<std::vector<ProcessorOutput>>(
        ErrorCode::invalid_argument,
        "processor output limits must be non-zero");
  }
  for (const auto& input : inputs) {
    if (input.payload.size() != input.record.payload_size) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::invalid_argument,
          "processor input payload does not match its exact record size");
    }
  }

  auto outputs = processor.process(inputs);
  if (!outputs) return outputs.error();
  if (outputs->size() > limits.maximum_outputs) {
    return fail<std::vector<ProcessorOutput>>(
        ErrorCode::resource_exhausted,
        "processor output count exceeds the configured limit");
  }

  std::uint64_t total_bytes = 0;
  for (const auto& output : *outputs) {
    const auto output_bytes =
        static_cast<std::uint64_t>(output.payload.size());
    if (output_bytes > limits.maximum_output_bytes - total_bytes) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::resource_exhausted,
          "processor output bytes exceed the configured limit");
    }
    total_bytes += output_bytes;
    if (output.end_ns < output.start_ns) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::invalid_argument,
          "processor output end time precedes start time");
    }
    if (output.truth != TruthClass::state_exact &&
        output.truth != TruthClass::derived) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::invalid_argument,
          "processor output truth class must be S1 or D");
    }
    if (prohibited_artifact_type(output.type)) {
      return fail<std::vector<ProcessorOutput>>(
          ErrorCode::invalid_argument,
          "processor output type is reserved for archive metadata");
    }
    const StreamProvenance provisional{
        .subject_truth = output.truth,
        .subject = {},
        .inputs = {ProvenanceRecordLink{}},
        .process = output.process,
    };
    auto encoded = detail::encode_stream_provenance(provisional);
    if (!encoded) return encoded.error();
  }
  return std::move(*outputs);
}

Result<ExportResult> invoke_exporter(
    StreamExporter& exporter, std::span<const ExtractedRecord> inputs,
    ExporterRunLimits limits) {
  if (limits.maximum_inputs == 0 || limits.maximum_input_bytes == 0 ||
      limits.maximum_output_bytes == 0 ||
      limits.maximum_payload_type_bytes == 0) {
    return fail<ExportResult>(
        ErrorCode::invalid_argument,
        "exporter resource limits must be non-zero");
  }
  if (inputs.empty()) {
    return fail<ExportResult>(ErrorCode::invalid_argument,
                              "exporter requires at least one exact input");
  }
  if (inputs.size() > limits.maximum_inputs) {
    return fail<ExportResult>(
        ErrorCode::resource_exhausted,
        "exporter input count exceeds the configured limit");
  }

  std::uint64_t total_input_bytes = 0;
  for (const auto& input : inputs) {
    if (input.payload.size() != input.record.payload_size) {
      return fail<ExportResult>(
          ErrorCode::invalid_argument,
          "exporter input payload does not match its exact record size");
    }
    const auto input_bytes = static_cast<std::uint64_t>(input.payload.size());
    if (input_bytes > limits.maximum_input_bytes - total_input_bytes) {
      return fail<ExportResult>(
          ErrorCode::resource_exhausted,
          "exporter input bytes exceed the configured limit");
    }
    total_input_bytes += input_bytes;
  }

  auto output = exporter.export_records(inputs);
  if (!output) return output.error();
  if (output->payload_type.empty()) {
    return fail<ExportResult>(ErrorCode::invalid_argument,
                              "exporter payload type must be non-empty");
  }
  if (output->payload_type.size() > limits.maximum_payload_type_bytes) {
    return fail<ExportResult>(
        ErrorCode::resource_exhausted,
        "exporter payload type exceeds the configured limit");
  }
  if (output->payload.size() > limits.maximum_output_bytes) {
    return fail<ExportResult>(
        ErrorCode::resource_exhausted,
        "exporter output bytes exceed the configured limit");
  }

  std::vector<ProvenanceRecordLink> supporting_records;
  supporting_records.reserve(inputs.size());
  for (const auto& input : inputs) {
    supporting_records.push_back(ProvenanceRecordLink{
        .stream = input.record.stream,
        .type = input.record.type_code(),
        .sequence = input.record.sequence,
        .hash = input.record.hash,
    });
  }

  return ExportResult{
      .payload_type = std::move(output->payload_type),
      .payload = std::move(output->payload),
      .supporting_records = std::move(supporting_records),
  };
}

}  // namespace codec

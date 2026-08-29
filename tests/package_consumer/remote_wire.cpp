#include <codec/distributed_wire.hpp>
#include <codec/integrity.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

int main() {
  codec::ExtractedRecord input;
  input.record.type = codec::RecordType::source_bytes;
  input.record.sequence = 7;
  input.record.stream = codec::derive_stream_id("package/f7-wire");
  input.record.start_ns = 70;
  input.record.end_ns = 80;
  input.record.file_offset = 7007;
  input.payload = {std::byte{0x41}, std::byte{0x42}};
  input.record.payload_size = 2;
  input.record.hash = codec::sha256(input.payload);

  codec::DistributedRemoteExecutionRequest request{
      .worker_name = "package-remote",
      .processor_name = "package-processor",
      .inputs = {input},
  };
  auto request_bytes = codec::encode_distributed_remote_request(request);
  if (!request_bytes) return 1;
  auto decoded_request =
      codec::decode_distributed_remote_request(*request_bytes);
  if (!decoded_request || decoded_request->worker_name != request.worker_name ||
      decoded_request->processor_name != request.processor_name ||
      decoded_request->inputs.size() != 1 ||
      decoded_request->inputs.front().record.type_code() !=
          input.record.type_code() ||
      decoded_request->inputs.front().record.hash != input.record.hash ||
      decoded_request->inputs.front().payload != input.payload) {
    return 2;
  }

  codec::ProvenanceProcess process{
      .operation = "package-f7-wire",
      .implementation_id = "package-consumer",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 1,
      .details_type = {},
      .details = {},
  };
  codec::DistributedRemoteExecutionResponse response{
      .worker_name = request.worker_name,
      .processor_name = request.processor_name,
      .outputs = {codec::ProcessorOutput{
          .stream = input.record.stream,
          .type = 0x7d40,
          .start_ns = 70,
          .end_ns = 80,
          .truth = codec::TruthClass::derived,
          .payload = {std::byte{0x55}},
          .process = process,
      }},
  };
  auto response_bytes = codec::encode_distributed_remote_reply(
      codec::DistributedRemoteExecutionReply{response});
  if (!response_bytes) return 3;
  auto decoded_response =
      codec::decode_distributed_remote_reply(*response_bytes);
  if (!decoded_response ||
      !std::holds_alternative<codec::DistributedRemoteExecutionResponse>(
          *decoded_response)) {
    return 4;
  }
  const auto& response_value =
      std::get<codec::DistributedRemoteExecutionResponse>(*decoded_response);
  if (response_value.worker_name != response.worker_name ||
      response_value.processor_name != response.processor_name ||
      response_value.outputs.size() != 1 ||
      response_value.outputs.front().truth != codec::TruthClass::derived ||
      response_value.outputs.front().payload != response.outputs.front().payload ||
      response_value.outputs.front().process.operation != process.operation) {
    return 5;
  }

  codec::DistributedRemoteExecutionReply error_reply{
      codec::Error{codec::ErrorCode::network, "package network", true}};
  auto error_bytes = codec::encode_distributed_remote_reply(error_reply);
  if (!error_bytes) return 6;
  auto decoded_error = codec::decode_distributed_remote_reply(*error_bytes);
  if (!decoded_error || !std::holds_alternative<codec::Error>(*decoded_error)) {
    return 7;
  }
  const auto& error = std::get<codec::Error>(*decoded_error);
  if (error.code != codec::ErrorCode::network ||
      error.message != "package network" || !error.retryable) {
    return 8;
  }
  return 0;
}

#pragma once

#include <codec/distributed.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace codec {

inline constexpr std::uint16_t distributed_remote_wire_version = 1;

struct DistributedRemoteWireLimits {
  std::uint64_t maximum_envelope_bytes{128ULL * 1024ULL * 1024ULL};
  std::size_t maximum_input_records{1024};
  std::uint64_t maximum_input_bytes{64ULL * 1024ULL * 1024ULL};
  std::size_t maximum_outputs{1024};
  std::uint64_t maximum_output_bytes{64ULL * 1024ULL * 1024ULL};
  std::size_t maximum_label_bytes{256};
  std::size_t maximum_process_text_bytes{4096};
  std::uint64_t maximum_process_details_bytes{1024ULL * 1024ULL};
  std::size_t maximum_error_message_bytes{4096};
};

struct DistributedRemoteExecutionRequest {
  std::string worker_name;
  std::string processor_name;
  std::vector<ExtractedRecord> inputs;
};

using DistributedRemoteExecutionReply =
    std::variant<DistributedRemoteExecutionResponse, Error>;

Result<std::vector<std::byte>> encode_distributed_remote_request(
    const DistributedRemoteExecutionRequest& request,
    DistributedRemoteWireLimits limits = {});
Result<DistributedRemoteExecutionRequest> decode_distributed_remote_request(
    std::span<const std::byte> encoded,
    DistributedRemoteWireLimits limits = {});
Result<std::vector<std::byte>> encode_distributed_remote_reply(
    const DistributedRemoteExecutionReply& reply,
    DistributedRemoteWireLimits limits = {});
Result<DistributedRemoteExecutionReply> decode_distributed_remote_reply(
    std::span<const std::byte> encoded,
    DistributedRemoteWireLimits limits = {});

}  // namespace codec

#include "test.hpp"

#include <codec/distributed_wire.hpp>
#include <codec/integrity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

std::vector<std::byte> bytes(std::string_view value) {
  const auto chars = std::span{value.data(), value.size()};
  const auto raw = std::as_bytes(chars);
  return {raw.begin(), raw.end()};
}

codec::ExtractedRecord make_input(codec::RecordTypeCode type,
                                  std::string_view stream_name,
                                  std::uint64_t sequence,
                                  std::string_view payload) {
  codec::ExtractedRecord out;
  out.record.type = static_cast<codec::RecordType>(type);
  out.record.sequence = sequence;
  out.record.stream = codec::derive_stream_id(stream_name);
  out.record.start_ns = static_cast<std::int64_t>(sequence * 10);
  out.record.end_ns = out.record.start_ns + 10;
  out.record.file_offset = 9000 + sequence;
  out.payload = bytes(payload);
  out.record.payload_size = static_cast<std::uint64_t>(out.payload.size());
  out.record.hash = codec::sha256(out.payload);
  return out;
}

codec::ProvenanceProcess make_process() {
  return codec::ProvenanceProcess{
      .operation = "f7-test",
      .implementation_id = "codec-test",
      .implementation_version = "1",
      .implementation_hash = codec::sha256(bytes("impl")),
      .configuration_hash = codec::sha256(bytes("config")),
      .created_utc_ns = 123456,
      .details_type = "application/f7-test",
      .details = bytes("details"),
  };
}

codec::ProcessorOutput make_output(codec::TruthClass truth,
                                   std::string_view stream_name,
                                   codec::RecordTypeCode type,
                                   std::string_view payload) {
  return codec::ProcessorOutput{
      .stream = codec::derive_stream_id(stream_name),
      .type = type,
      .start_ns = 10,
      .end_ns = 20,
      .truth = truth,
      .payload = bytes(payload),
      .process = make_process(),
  };
}

codec::DistributedRemoteExecutionRequest valid_request() {
  return codec::DistributedRemoteExecutionRequest{
      .worker_name = "remote-a",
      .processor_name = "processor-a",
      .inputs = {
          make_input(codec::record_type_code(codec::RecordType::source_bytes),
                     "f7/request", 1, "alpha"),
          make_input(0x7d55, "f7/request", 2, "beta"),
      },
  };
}

codec::DistributedRemoteExecutionResponse valid_response() {
  return codec::DistributedRemoteExecutionResponse{
      .worker_name = "remote-a",
      .processor_name = "processor-a",
      .outputs = {
          make_output(codec::TruthClass::state_exact, "f7/reply", 0x7d01,
                      "state"),
          make_output(codec::TruthClass::derived, "f7/reply", 0x7d02,
                      "derived"),
      },
  };
}

bool same_record(const codec::ExtractedRecord& lhs,
                 const codec::ExtractedRecord& rhs) {
  return lhs.record.type_code() == rhs.record.type_code() &&
         lhs.record.sequence == rhs.record.sequence &&
         lhs.record.stream == rhs.record.stream &&
         lhs.record.start_ns == rhs.record.start_ns &&
         lhs.record.end_ns == rhs.record.end_ns &&
         lhs.record.payload_size == rhs.record.payload_size &&
         lhs.record.file_offset == rhs.record.file_offset &&
         lhs.record.hash == rhs.record.hash && lhs.payload == rhs.payload;
}

bool same_process(const codec::ProvenanceProcess& lhs,
                  const codec::ProvenanceProcess& rhs) {
  return lhs.operation == rhs.operation &&
         lhs.implementation_id == rhs.implementation_id &&
         lhs.implementation_version == rhs.implementation_version &&
         lhs.implementation_hash == rhs.implementation_hash &&
         lhs.configuration_hash == rhs.configuration_hash &&
         lhs.created_utc_ns == rhs.created_utc_ns &&
         lhs.details_type == rhs.details_type && lhs.details == rhs.details;
}

bool same_output(const codec::ProcessorOutput& lhs,
                 const codec::ProcessorOutput& rhs) {
  return lhs.stream == rhs.stream && lhs.type == rhs.type &&
         lhs.start_ns == rhs.start_ns && lhs.end_ns == rhs.end_ns &&
         lhs.truth == rhs.truth && lhs.payload == rhs.payload &&
         same_process(lhs.process, rhs.process);
}

std::uint64_t read_u64_le(std::span<const std::byte> data,
                          std::size_t offset) {
  std::uint64_t value{};
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<std::uint8_t>(data[offset + index]))
             << (index * 8U);
  }
  return value;
}

void write_u64_le(std::vector<std::byte>& data, std::size_t offset,
                  std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    data[offset + index] = static_cast<std::byte>(value & 0xffU);
    value >>= 8U;
  }
}

void expect_error_code(const codec::Result<codec::DistributedRemoteExecutionRequest>& result,
                       codec::ErrorCode code) {
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, code);
}

void expect_error_code(const codec::Result<codec::DistributedRemoteExecutionReply>& result,
                       codec::ErrorCode code) {
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, code);
}

class EncodedReplyTransport final : public codec::DistributedWorkerTransport {
 public:
  explicit EncodedReplyTransport(std::vector<std::byte> reply)
      : reply_(std::move(reply)) {}

  std::string name() const override { return "f7-wire-transport"; }

  codec::Result<codec::DistributedRemoteExecutionResponse> dispatch(
      std::string_view,
      std::string_view,
      std::span<const codec::ExtractedRecord>) override {
    ++calls;
    auto decoded = codec::decode_distributed_remote_reply(reply_);
    if (!decoded) return decoded.error();
    if (std::holds_alternative<codec::Error>(*decoded)) {
      return std::get<codec::Error>(std::move(*decoded));
    }
    return std::get<codec::DistributedRemoteExecutionResponse>(
        std::move(*decoded));
  }

  std::size_t calls{};

 private:
  std::vector<std::byte> reply_;
};

}  // namespace

TEST(distributed_wire_request_round_trip_is_deterministic) {
  const auto request = valid_request();
  auto first = codec::encode_distributed_remote_request(request);
  auto second = codec::encode_distributed_remote_request(request);
  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
  EXPECT_EQ(*first, *second);
  EXPECT_TRUE(first->size() > std::size_t{56});
  EXPECT_EQ(std::to_integer<char>((*first)[0]), 'D');
  EXPECT_EQ(std::to_integer<char>((*first)[1]), 'R');
  EXPECT_EQ(std::to_integer<char>((*first)[2]), 'Q');
  EXPECT_EQ(std::to_integer<char>((*first)[3]), '1');

  auto decoded = codec::decode_distributed_remote_request(*first);
  EXPECT_TRUE(decoded);
  EXPECT_EQ(decoded->worker_name, request.worker_name);
  EXPECT_EQ(decoded->processor_name, request.processor_name);
  EXPECT_EQ(decoded->inputs.size(), request.inputs.size());
  EXPECT_TRUE(same_record(decoded->inputs[0], request.inputs[0]));
  EXPECT_TRUE(same_record(decoded->inputs[1], request.inputs[1]));
  EXPECT_EQ(decoded->inputs[1].record.type_code(), codec::RecordTypeCode{0x7d55});

  auto canonical = codec::encode_distributed_remote_request(*decoded);
  EXPECT_TRUE(canonical);
  EXPECT_EQ(*canonical, *first);
}

TEST(distributed_wire_request_encoder_enforces_structure_and_limits) {
  auto request = valid_request();

  {
    auto value = request;
    value.worker_name.clear();
    auto result = codec::encode_distributed_remote_request(value);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  }
  {
    auto value = request;
    value.processor_name.clear();
    auto result = codec::encode_distributed_remote_request(value);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  }
  {
    auto value = request;
    value.inputs.clear();
    auto result = codec::encode_distributed_remote_request(value);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  }
  {
    auto value = request;
    ++value.inputs.front().record.payload_size;
    auto result = codec::encode_distributed_remote_request(value);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  }
  {
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_label_bytes = 4;
    auto result = codec::encode_distributed_remote_request(request, limits);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
  }
  {
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_input_records = 1;
    auto result = codec::encode_distributed_remote_request(request, limits);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
  }
  {
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_input_bytes = 1;
    auto result = codec::encode_distributed_remote_request(request, limits);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
  }
  {
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_envelope_bytes = 56;
    auto result = codec::encode_distributed_remote_request(request, limits);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
  }
  {
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_envelope_bytes = 0;
    auto result = codec::encode_distributed_remote_request(request, limits);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  }
}

TEST(distributed_wire_request_decoder_rejects_noncanonical_or_corrupt_bytes) {
  auto encoded = codec::encode_distributed_remote_request(valid_request());
  EXPECT_TRUE(encoded);

  {
    auto value = *encoded;
    value[0] = std::byte{'X'};
    expect_error_code(codec::decode_distributed_remote_request(value),
                      codec::ErrorCode::protocol);
  }
  {
    auto value = *encoded;
    value[4] = std::byte{2};
    expect_error_code(codec::decode_distributed_remote_request(value),
                      codec::ErrorCode::protocol);
  }
  {
    auto value = *encoded;
    value[6] = std::byte{1};
    expect_error_code(codec::decode_distributed_remote_request(value),
                      codec::ErrorCode::protocol);
  }
  {
    auto value = *encoded;
    value.back() ^= std::byte{1};
    expect_error_code(codec::decode_distributed_remote_request(value),
                      codec::ErrorCode::protocol);
  }
  {
    auto value = *encoded;
    value.pop_back();
    expect_error_code(codec::decode_distributed_remote_request(value),
                      codec::ErrorCode::protocol);
  }
  {
    auto value = *encoded;
    value.push_back(std::byte{0});
    expect_error_code(codec::decode_distributed_remote_request(value),
                      codec::ErrorCode::protocol);
  }
  {
    auto value = *encoded;
    write_u64_le(value, 16, read_u64_le(value, 16) + 1);
    expect_error_code(codec::decode_distributed_remote_request(value),
                      codec::ErrorCode::protocol);
  }
  {
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_envelope_bytes = static_cast<std::uint64_t>(encoded->size() - 1);
    expect_error_code(codec::decode_distributed_remote_request(*encoded, limits),
                      codec::ErrorCode::resource_exhausted);
  }
  {
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_input_records = 1;
    expect_error_code(codec::decode_distributed_remote_request(*encoded, limits),
                      codec::ErrorCode::resource_exhausted);
  }
  {
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_input_bytes = 1;
    expect_error_code(codec::decode_distributed_remote_request(*encoded, limits),
                      codec::ErrorCode::resource_exhausted);
  }
}

TEST(distributed_wire_success_reply_round_trip_is_deterministic) {
  codec::DistributedRemoteExecutionReply reply{valid_response()};
  auto first = codec::encode_distributed_remote_reply(reply);
  auto second = codec::encode_distributed_remote_reply(reply);
  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
  EXPECT_EQ(*first, *second);
  EXPECT_EQ(std::to_integer<char>((*first)[0]), 'D');
  EXPECT_EQ(std::to_integer<char>((*first)[1]), 'R');
  EXPECT_EQ(std::to_integer<char>((*first)[2]), 'S');
  EXPECT_EQ(std::to_integer<char>((*first)[3]), '1');

  auto decoded = codec::decode_distributed_remote_reply(*first);
  EXPECT_TRUE(decoded);
  EXPECT_TRUE(std::holds_alternative<codec::DistributedRemoteExecutionResponse>(
      *decoded));
  const auto& response =
      std::get<codec::DistributedRemoteExecutionResponse>(*decoded);
  const auto expected = valid_response();
  EXPECT_EQ(response.worker_name, expected.worker_name);
  EXPECT_EQ(response.processor_name, expected.processor_name);
  EXPECT_EQ(response.outputs.size(), expected.outputs.size());
  EXPECT_TRUE(same_output(response.outputs[0], expected.outputs[0]));
  EXPECT_TRUE(same_output(response.outputs[1], expected.outputs[1]));

  auto canonical = codec::encode_distributed_remote_reply(*decoded);
  EXPECT_TRUE(canonical);
  EXPECT_EQ(*canonical, *first);
}

TEST(distributed_wire_structurally_round_trips_source_exact_output) {
  auto response = valid_response();
  response.outputs = {
      make_output(codec::TruthClass::source_exact, "f7/structural", 0x7d20,
                  "s0"),
  };
  codec::DistributedRemoteExecutionReply reply{response};
  auto encoded = codec::encode_distributed_remote_reply(reply);
  EXPECT_TRUE(encoded);
  auto decoded = codec::decode_distributed_remote_reply(*encoded);
  EXPECT_TRUE(decoded);
  const auto& round_trip =
      std::get<codec::DistributedRemoteExecutionResponse>(*decoded);
  EXPECT_EQ(round_trip.outputs.size(), std::size_t{1});
  EXPECT_EQ(round_trip.outputs.front().truth, codec::TruthClass::source_exact);
}

TEST(distributed_wire_error_reply_round_trips_all_current_error_codes) {
  const std::array codes{
      codec::ErrorCode::invalid_argument,
      codec::ErrorCode::unauthorized_source,
      codec::ErrorCode::network,
      codec::ErrorCode::protocol,
      codec::ErrorCode::decode,
      codec::ErrorCode::archive_io,
      codec::ErrorCode::archive_corrupt,
      codec::ErrorCode::model_incompatible,
      codec::ErrorCode::inference,
      codec::ErrorCode::watermark_model_missing,
      codec::ErrorCode::watermark_code_ambiguous,
      codec::ErrorCode::watermark_signature_invalid,
      codec::ErrorCode::watermark_replay_suspected,
      codec::ErrorCode::watermark_path_unqualified,
      codec::ErrorCode::identity_not_enrolled,
      codec::ErrorCode::identity_uncalibrated,
      codec::ErrorCode::cancelled,
      codec::ErrorCode::resource_exhausted,
      codec::ErrorCode::internal,
  };

  for (const auto code : codes) {
    std::string message{"bad\0wire", 8};
    codec::DistributedRemoteExecutionReply reply{
        codec::Error{code, message, true}};
    auto encoded = codec::encode_distributed_remote_reply(reply);
    EXPECT_TRUE(encoded);
    auto decoded = codec::decode_distributed_remote_reply(*encoded);
    EXPECT_TRUE(decoded);
    EXPECT_TRUE(std::holds_alternative<codec::Error>(*decoded));
    const auto& error = std::get<codec::Error>(*decoded);
    EXPECT_EQ(error.code, code);
    EXPECT_EQ(error.message, message);
    EXPECT_TRUE(error.retryable);
    auto canonical = codec::encode_distributed_remote_reply(*decoded);
    EXPECT_TRUE(canonical);
    EXPECT_EQ(*canonical, *encoded);
  }
}

TEST(distributed_wire_reply_encoder_enforces_structure_and_limits) {
  {
    codec::DistributedRemoteExecutionReply reply{
        codec::Error{codec::ErrorCode::ok, "not an error", false}};
    auto result = codec::encode_distributed_remote_reply(reply);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  }
  {
    auto response = valid_response();
    response.worker_name.clear();
    auto result = codec::encode_distributed_remote_reply(
        codec::DistributedRemoteExecutionReply{response});
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  }
  {
    auto response = valid_response();
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_outputs = 1;
    auto result = codec::encode_distributed_remote_reply(
        codec::DistributedRemoteExecutionReply{response}, limits);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
  }
  {
    auto response = valid_response();
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_output_bytes = 1;
    auto result = codec::encode_distributed_remote_reply(
        codec::DistributedRemoteExecutionReply{response}, limits);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
  }
  {
    auto response = valid_response();
    response.outputs.front().truth = static_cast<codec::TruthClass>(0xff);
    auto result = codec::encode_distributed_remote_reply(
        codec::DistributedRemoteExecutionReply{response});
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  }
  {
    auto response = valid_response();
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_process_text_bytes = 1;
    auto result = codec::encode_distributed_remote_reply(
        codec::DistributedRemoteExecutionReply{response}, limits);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
  }
  {
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_error_message_bytes = 2;
    codec::DistributedRemoteExecutionReply reply{
        codec::Error{codec::ErrorCode::network, "long", true}};
    auto result = codec::encode_distributed_remote_reply(reply, limits);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
  }
}

TEST(distributed_wire_reply_decoder_rejects_noncanonical_or_corrupt_bytes) {
  codec::DistributedRemoteExecutionReply reply{valid_response()};
  auto encoded = codec::encode_distributed_remote_reply(reply);
  EXPECT_TRUE(encoded);

  {
    auto value = *encoded;
    value[0] = std::byte{'X'};
    expect_error_code(codec::decode_distributed_remote_reply(value),
                      codec::ErrorCode::protocol);
  }
  {
    auto value = *encoded;
    value[4] = std::byte{2};
    expect_error_code(codec::decode_distributed_remote_reply(value),
                      codec::ErrorCode::protocol);
  }
  {
    auto value = *encoded;
    value[6] = std::byte{1};
    expect_error_code(codec::decode_distributed_remote_reply(value),
                      codec::ErrorCode::protocol);
  }
  {
    auto value = *encoded;
    value.back() ^= std::byte{1};
    expect_error_code(codec::decode_distributed_remote_reply(value),
                      codec::ErrorCode::protocol);
  }
  {
    auto value = *encoded;
    value.pop_back();
    expect_error_code(codec::decode_distributed_remote_reply(value),
                      codec::ErrorCode::protocol);
  }
  {
    auto value = *encoded;
    value.push_back(std::byte{0});
    expect_error_code(codec::decode_distributed_remote_reply(value),
                      codec::ErrorCode::protocol);
  }
}

TEST(distributed_wire_error_decoder_rejects_unknown_code_and_retryable_value) {
  codec::DistributedRemoteExecutionReply reply{
      codec::Error{codec::ErrorCode::network, "network", true}};
  auto encoded = codec::encode_distributed_remote_reply(reply);
  EXPECT_TRUE(encoded);

  // Header is 56 bytes. Error body is: outcome/reserved 8, code u16,
  // retryable u8, reserved u8, message length u32, then message.
  {
    auto value = *encoded;
    value[64] = std::byte{0xff};
    value[65] = std::byte{0x7f};
    // This mutation also invalidates the digest; protocol rejection is still
    // the required outcome for an untrusted noncanonical envelope.
    expect_error_code(codec::decode_distributed_remote_reply(value),
                      codec::ErrorCode::protocol);
  }
  {
    auto value = *encoded;
    value[66] = std::byte{2};
    expect_error_code(codec::decode_distributed_remote_reply(value),
                      codec::ErrorCode::protocol);
  }
}

TEST(distributed_wire_does_not_weaken_f2_processor_semantics) {
  const std::vector<codec::ExtractedRecord> inputs{
      make_input(codec::record_type_code(codec::RecordType::source_bytes),
                 "f7/f2", 1, "input"),
  };
  auto partitions = codec::partition_exact_records(inputs);
  EXPECT_TRUE(partitions);
  EXPECT_EQ(partitions->size(), std::size_t{1});

  codec::DistributedRemoteExecutionResponse response{
      .worker_name = "remote-a",
      .processor_name = "processor-a",
      .outputs = {
          make_output(codec::TruthClass::source_exact, "f7/f2", 0x7d30,
                      "invalid-s0-output"),
      },
  };
  auto encoded = codec::encode_distributed_remote_reply(
      codec::DistributedRemoteExecutionReply{response});
  EXPECT_TRUE(encoded);

  EncodedReplyTransport transport{std::move(*encoded)};
  codec::RemoteDistributedWorker worker{
      transport, "remote-a", "processor-a"};
  auto result = codec::execute_partition(
      worker, partitions->front(), inputs);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  EXPECT_EQ(transport.calls, std::size_t{1});
}

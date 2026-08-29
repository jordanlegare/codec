#include "test.hpp"

#include <codec/distributed_wire.hpp>
#include <codec/integrity.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> strict_bytes(const char* value) {
  const std::string text{value};
  const auto raw = std::as_bytes(std::span{text.data(), text.size()});
  return {raw.begin(), raw.end()};
}

codec::ExtractedRecord strict_input() {
  codec::ExtractedRecord input;
  input.record.type = codec::RecordType::source_bytes;
  input.record.sequence = 1;
  input.record.stream = codec::derive_stream_id("f7/strict");
  input.record.start_ns = 10;
  input.record.end_ns = 20;
  input.record.file_offset = 77;
  input.payload = strict_bytes("x");
  input.record.payload_size = 1;
  input.record.hash = codec::sha256(input.payload);
  return input;
}

codec::ProcessorOutput strict_output() {
  return codec::ProcessorOutput{
      .stream = codec::derive_stream_id("f7/strict"),
      .type = 0x7d50,
      .start_ns = 10,
      .end_ns = 20,
      .truth = codec::TruthClass::derived,
      .payload = strict_bytes("out"),
      .process = codec::ProvenanceProcess{
          .operation = "strict-op",
          .implementation_id = "strict-impl",
          .implementation_version = "1",
          .implementation_hash = std::nullopt,
          .configuration_hash = std::nullopt,
          .created_utc_ns = 1,
          .details_type = "strict/details",
          .details = strict_bytes("details"),
      },
  };
}

void refresh_digest(std::vector<std::byte>& encoded) {
  std::vector<std::byte> hash_input;
  hash_input.reserve(24 + encoded.size() - 56);
  hash_input.insert(hash_input.end(), encoded.begin(), encoded.begin() + 24);
  hash_input.insert(hash_input.end(), encoded.begin() + 56, encoded.end());
  const auto digest = codec::sha256(hash_input);
  for (std::size_t index = 0; index < digest.size(); ++index) {
    encoded[24 + index] = static_cast<std::byte>(digest[index]);
  }
}

void expect_request_protocol(const std::vector<std::byte>& encoded) {
  auto result = codec::decode_distributed_remote_request(encoded);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::protocol);
}

void expect_reply_protocol(const std::vector<std::byte>& encoded) {
  auto result = codec::decode_distributed_remote_reply(encoded);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::protocol);
}

}  // namespace

TEST(distributed_wire_request_rejects_reserved_record_field_with_valid_digest) {
  codec::DistributedRemoteExecutionRequest request{
      .worker_name = "w", .processor_name = "p", .inputs = {strict_input()}};
  auto encoded = codec::encode_distributed_remote_request(request);
  EXPECT_TRUE(encoded);

  // Header 56 + worker(5) + processor(5) + count(4) = first record at 70.
  // Its u16 reserved field follows the u16 record type.
  (*encoded)[72] = std::byte{1};
  refresh_digest(*encoded);
  expect_request_protocol(*encoded);
}

TEST(distributed_wire_reply_rejects_deep_noncanonical_fields_with_valid_digest) {
  codec::DistributedRemoteExecutionResponse response{
      .worker_name = "w", .processor_name = "p", .outputs = {strict_output()}};
  auto base = codec::encode_distributed_remote_reply(
      codec::DistributedRemoteExecutionReply{response});
  EXPECT_TRUE(base);

  // Success output starts after header(56), outcome/reserved(8), worker(5),
  // processor(5), and count(4): offset 78. Within the fixed output prefix,
  // truth is +18, reserved is +19, process flags are +44.
  {
    auto encoded = *base;
    encoded[96] = std::byte{0xff};
    refresh_digest(encoded);
    expect_reply_protocol(encoded);
  }
  {
    auto encoded = *base;
    encoded[97] = std::byte{1};
    refresh_digest(encoded);
    expect_reply_protocol(encoded);
  }
  {
    auto encoded = *base;
    encoded[122] = std::byte{0x80};
    refresh_digest(encoded);
    expect_reply_protocol(encoded);
  }
  {
    auto encoded = *base;
    encoded[57] = std::byte{1};
    refresh_digest(encoded);
    expect_reply_protocol(encoded);
  }
  {
    auto encoded = *base;
    encoded[56] = std::byte{2};
    refresh_digest(encoded);
    expect_reply_protocol(encoded);
  }
}

TEST(distributed_wire_error_rejects_unknown_code_and_flags_with_valid_digest) {
  codec::DistributedRemoteExecutionReply reply{
      codec::Error{codec::ErrorCode::network, "network", true}};
  auto base = codec::encode_distributed_remote_reply(reply);
  EXPECT_TRUE(base);

  // Error fields start at body offset 8: wire code at 64, retryable at 66,
  // reserved at 67.
  {
    auto encoded = *base;
    encoded[64] = std::byte{0xff};
    encoded[65] = std::byte{0x7f};
    refresh_digest(encoded);
    expect_reply_protocol(encoded);
  }
  {
    auto encoded = *base;
    encoded[66] = std::byte{2};
    refresh_digest(encoded);
    expect_reply_protocol(encoded);
  }
  {
    auto encoded = *base;
    encoded[67] = std::byte{1};
    refresh_digest(encoded);
    expect_reply_protocol(encoded);
  }
}

TEST(distributed_wire_decode_limits_apply_after_valid_envelope_verification) {
  codec::DistributedRemoteExecutionRequest request{
      .worker_name = "worker", .processor_name = "processor",
      .inputs = {strict_input()}};
  auto request_bytes = codec::encode_distributed_remote_request(request);
  EXPECT_TRUE(request_bytes);
  {
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_label_bytes = 2;
    auto decoded = codec::decode_distributed_remote_request(*request_bytes, limits);
    EXPECT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code, codec::ErrorCode::resource_exhausted);
  }

  codec::DistributedRemoteExecutionResponse response{
      .worker_name = "worker", .processor_name = "processor",
      .outputs = {strict_output(), strict_output()}};
  auto reply_bytes = codec::encode_distributed_remote_reply(
      codec::DistributedRemoteExecutionReply{response});
  EXPECT_TRUE(reply_bytes);
  {
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_outputs = 1;
    auto decoded = codec::decode_distributed_remote_reply(*reply_bytes, limits);
    EXPECT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code, codec::ErrorCode::resource_exhausted);
  }
  {
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_process_text_bytes = 2;
    auto decoded = codec::decode_distributed_remote_reply(*reply_bytes, limits);
    EXPECT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code, codec::ErrorCode::resource_exhausted);
  }
  {
    codec::DistributedRemoteWireLimits limits;
    limits.maximum_process_details_bytes = 2;
    auto decoded = codec::decode_distributed_remote_reply(*reply_bytes, limits);
    EXPECT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code, codec::ErrorCode::resource_exhausted);
  }

  auto error_bytes = codec::encode_distributed_remote_reply(
      codec::DistributedRemoteExecutionReply{
          codec::Error{codec::ErrorCode::network, "network", false}});
  EXPECT_TRUE(error_bytes);
  codec::DistributedRemoteWireLimits limits;
  limits.maximum_error_message_bytes = 2;
  auto decoded_error = codec::decode_distributed_remote_reply(*error_bytes, limits);
  EXPECT_FALSE(decoded_error);
  EXPECT_EQ(decoded_error.error().code, codec::ErrorCode::resource_exhausted);
}

#include "test.hpp"

#include <codec/distributed_wire.hpp>
#include <codec/integrity.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

TEST(distributed_wire_request_contract_exists) {
  codec::ExtractedRecord input;
  input.record.type = codec::RecordType::source_bytes;
  input.record.sequence = 1;
  input.record.stream = codec::derive_stream_id("f7/red");
  input.record.start_ns = 10;
  input.record.end_ns = 20;
  input.record.file_offset = 9001;
  input.payload = {std::byte{0x41}};
  input.record.payload_size = 1;
  input.record.hash = codec::sha256(input.payload);

  codec::DistributedRemoteExecutionRequest request{
      .worker_name = "remote-a",
      .processor_name = "processor-a",
      .inputs = {input},
  };
  auto encoded = codec::encode_distributed_remote_request(request);
  EXPECT_TRUE(encoded);
}

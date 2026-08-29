#include "test.hpp"

#include <codec/distributed_wire.hpp>

#include <string>

TEST(distributed_wire_request_contract_exists) {
  codec::DistributedRemoteExecutionRequest request{
      .worker_name = "remote-a",
      .processor_name = "processor-a",
      .inputs = {},
  };
  auto encoded = codec::encode_distributed_remote_request(request);
  EXPECT_FALSE(encoded);
}

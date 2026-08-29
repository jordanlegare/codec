#include "test.hpp"

#include <codec/distributed.hpp>
#include <codec/integrity.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::byte> bytes(std::string_view value) {
  const auto chars = std::span{value.data(), value.size()};
  const auto raw = std::as_bytes(chars);
  return {raw.begin(), raw.end()};
}

codec::ExtractedRecord exact_record(std::string_view stream_name,
                                    std::uint64_t sequence,
                                    std::string_view payload) {
  codec::ExtractedRecord out;
  out.record.type = codec::RecordType::source_bytes;
  out.record.sequence = sequence;
  out.record.stream = codec::derive_stream_id(stream_name);
  out.record.start_ns = static_cast<std::int64_t>(sequence * 10);
  out.record.end_ns = out.record.start_ns + 10;
  out.payload = bytes(payload);
  out.record.payload_size = static_cast<std::uint64_t>(out.payload.size());
  out.record.hash = codec::sha256(out.payload);
  return out;
}

codec::ProvenanceProcess process_identity() {
  return codec::ProvenanceProcess{
      .operation = "f6-test",
      .implementation_id = "codec-test",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 1,
      .details_type = {},
      .details = {},
  };
}

codec::ProcessorOutput valid_output(codec::StreamId stream) {
  return codec::ProcessorOutput{
      .stream = stream,
      .type = 0x7d01,
      .start_ns = 10,
      .end_ns = 20,
      .truth = codec::TruthClass::derived,
      .payload = bytes("remote"),
      .process = process_identity(),
  };
}

class RecordingTransport final : public codec::DistributedWorkerTransport {
 public:
  std::string name() const override { return "recording-transport"; }

  codec::Result<codec::DistributedRemoteExecutionResponse> dispatch(
      std::string_view worker_name,
      std::string_view processor_name,
      std::span<const codec::ExtractedRecord> inputs) override {
    ++calls;
    return codec::DistributedRemoteExecutionResponse{
        .worker_name = std::string{worker_name},
        .processor_name = std::string{processor_name},
        .outputs = {valid_output(inputs.front().record.stream)},
    };
  }

  std::size_t calls{};
};

}  // namespace

TEST(distributed_remote_worker_executes_verified_partition_once) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f6/valid", 1, "alpha")};
  auto partitions = codec::partition_exact_records(inputs);
  EXPECT_TRUE(partitions);
  EXPECT_EQ(partitions->size(), std::size_t{1});

  RecordingTransport transport;
  codec::RemoteDistributedWorker worker{
      transport, "remote-a", "processor-a"};

  auto result = codec::execute_partition(worker, partitions->front(), inputs);
  EXPECT_TRUE(result);
  EXPECT_EQ(transport.calls, std::size_t{1});
  EXPECT_EQ(result->worker_name, std::string{"remote-a"});
  EXPECT_EQ(result->processor_name, std::string{"processor-a"});
}

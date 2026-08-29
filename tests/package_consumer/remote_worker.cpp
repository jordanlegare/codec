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

class PackageWorkerTransport final : public codec::DistributedWorkerTransport {
 public:
  std::string name() const override { return "package-worker-transport"; }

  codec::Result<codec::DistributedRemoteExecutionResponse> dispatch(
      std::string_view worker_name,
      std::string_view processor_name,
      std::span<const codec::ExtractedRecord> inputs) override {
    ++calls;
    codec::ProvenanceProcess process{
        .operation = "package-remote-distributed",
        .implementation_id = "package-consumer",
        .implementation_version = "1",
        .implementation_hash = std::nullopt,
        .configuration_hash = std::nullopt,
        .created_utc_ns = 1,
        .details_type = {},
        .details = {},
    };
    return codec::DistributedRemoteExecutionResponse{
        .worker_name = std::string{worker_name},
        .processor_name = std::string{processor_name},
        .outputs = {codec::ProcessorOutput{
            .stream = inputs.front().record.stream,
            .type = 0x7a20,
            .start_ns = 0,
            .end_ns = 1,
            .truth = codec::TruthClass::derived,
            .payload = {std::byte{0x52}},
            .process = std::move(process),
        }},
    };
  }

  std::size_t calls{};
};

}  // namespace

int main() {
  codec::ExtractedRecord input;
  input.record.type = codec::RecordType::source_bytes;
  input.record.stream = codec::derive_stream_id("package-consumer/remote");
  input.record.sequence = 1;
  input.record.start_ns = 0;
  input.record.end_ns = 1;
  input.payload = {std::byte{0x41}, std::byte{0x42}};
  input.record.payload_size = static_cast<std::uint64_t>(input.payload.size());
  input.record.hash = codec::sha256(input.payload);

  const std::vector<codec::ExtractedRecord> inputs{input};
  auto partitions = codec::partition_exact_records(inputs);
  if (!partitions || partitions->size() != 1) return 1;

  PackageWorkerTransport transport;
  codec::RemoteDistributedWorker worker{
      transport, "package-remote-worker", "package-remote-processor"};
  auto execution = codec::execute_partition(
      worker, partitions->front(), inputs);
  if (!execution || transport.calls != 1 ||
      execution->worker_name != "package-remote-worker" ||
      execution->processor_name != "package-remote-processor" ||
      execution->outputs.size() != 1 ||
      execution->outputs.front().truth != codec::TruthClass::derived ||
      execution->outputs.front().payload !=
          std::vector<std::byte>{std::byte{0x52}}) {
    return 1;
  }
  return 0;
}

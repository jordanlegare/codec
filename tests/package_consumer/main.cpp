#include <codec/archive.hpp>
#include <codec/archive_follow.hpp>
#include <codec/distributed.hpp>
#include <codec/integrity.hpp>
#include <codec/processing.hpp>
#include <codec/profiles/audio.hpp>
#include <codec/recovery.hpp>
#include <codec/streaming_repair.hpp>
#include <codec/transport.hpp>
#include <codec/xor_recovery.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

class PackageObjectStore final : public codec::ObjectStoreBackend {
 public:
  std::string name() const override { return "package-object-store"; }

  codec::Result<std::vector<std::byte>> read_range(
      const codec::ObjectStoreObjectRef& object,
      std::uint64_t offset,
      std::uint64_t length) override {
    const auto size = static_cast<std::uint64_t>(bytes_.size());
    if (object.store != "package" || object.key != "records" ||
        object.version != "v1" || offset > size || length > size - offset) {
      return codec::fail<std::vector<std::byte>>(
          codec::ErrorCode::network, "package object range unavailable");
    }
    const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = begin + static_cast<std::ptrdiff_t>(length);
    return std::vector<std::byte>{begin, end};
  }

  std::vector<std::byte> bytes_;
};

class PackageDistributedProcessor final : public codec::StreamProcessor {
 public:
  std::string name() const override { return "package-distributed"; }

  codec::Result<std::vector<codec::ProcessorOutput>> process(
      std::span<const codec::ExtractedRecord> inputs) override {
    codec::ProvenanceProcess process{
        .operation = "package-distributed",
        .implementation_id = "package-consumer",
        .implementation_version = "1",
        .implementation_hash = std::nullopt,
        .configuration_hash = std::nullopt,
        .created_utc_ns = 1,
        .details_type = {},
        .details = {},
    };
    return std::vector<codec::ProcessorOutput>{codec::ProcessorOutput{
        .stream = inputs.front().record.stream,
        .type = 0x7a10,
        .start_ns = 0,
        .end_ns = 1,
        .truth = codec::TruthClass::derived,
        .payload = {std::byte{0x42}},
        .process = std::move(process),
    }};
  }
};

}  // namespace

int main() {
  const codec::profiles::audio::Pcm16FlacExportLimits limits{};
  const codec::profiles::audio::OfflinePcm16SeparationRequest offline{};
  const codec::profiles::audio::OfflinePcm16SeparationLimits offline_limits{};
  const codec::profiles::audio::OnnxCpuSeparationOptions runtime_options{};
  const auto runtime_compiled =
      codec::profiles::audio::onnx_cpu_separation_runtime_compiled();
  (void)runtime_compiled;
  const codec::profiles::audio::SeparationModelBundle model_bundle{
      .manifest = {
          .model_id = "consumer.separator",
          .model_version = "1",
          .license_id = "Apache-2.0",
          .quality_domain = "test",
          .input_sample_rate = 48000,
          .input_channels = 2,
          .window_samples = 8,
          .hop_samples = 4,
          .lookahead_samples = 0,
          .maximum_sources = 2,
          .causal = true,
          .input_tensor_name = "mixture",
          .output_tensor_name = "sources",
      },
      .onnx_model = {std::byte{0x08}},
  };
  const auto encoded_model_bundle =
      codec::profiles::audio::encode_separation_model_bundle(model_bundle);
  codec::Pcm16State state{
      .sample_rate = 48000,
      .channels = 2,
      .samples = {0, 0},
  };

  codec::MultiplexFrame multiplex;
  multiplex.stream = codec::derive_stream_id("package-consumer/mux");
  multiplex.sequence = 4;
  multiplex.epoch = {.connection = 2, .format = 1};
  multiplex.clock.source_timebase_numerator = 1;
  multiplex.clock.source_timebase_denominator = 1;
  multiplex.payload = {std::byte{0x42}};
  auto encoded_multiplex = codec::encode_multiplex_frame(multiplex);
  if (!encoded_multiplex) return 1;
  codec::MultiplexDecoder decoder;
  auto decoded_multiplex = decoder.push(*encoded_multiplex);
  if (!decoded_multiplex || decoded_multiplex->size() != 1 ||
      decoded_multiplex->front().stream != multiplex.stream ||
      !decoder.finish()) {
    return 1;
  }

  codec::SequenceLossObserver loss_observer;
  auto sequence_observation = loss_observer.observe(decoded_multiplex->front());
  if (!sequence_observation ||
      sequence_observation->kind != codec::SequenceObservationKind::baseline) {
    return 1;
  }

  codec::RecoveryGroupTracker recovery_groups;
  codec::RecoveryGroupDescriptor recovery_descriptor;
  recovery_descriptor.key.stream = multiplex.stream;
  recovery_descriptor.key.epoch = multiplex.epoch;
  recovery_descriptor.key.group_sequence = 1;
  recovery_descriptor.first_sequence = multiplex.sequence;
  recovery_descriptor.source_count = 1;
  if (!recovery_groups.begin(recovery_descriptor)) return 1;
  auto recovery_observation =
      recovery_groups.observe(decoded_multiplex->front());
  if (!recovery_observation ||
      recovery_observation->kind !=
          codec::RecoveryGroupFrameKind::first_observation) {
    return 1;
  }
  auto recovery_report = recovery_groups.seal(recovery_descriptor.key);
  if (!recovery_report ||
      recovery_report->state != codec::RecoveryGroupState::sealed_complete ||
      !recovery_report->missing_ranges.empty()) {
    return 1;
  }

  auto xor_second = multiplex;
  xor_second.sequence = multiplex.sequence + 1;
  xor_second.payload = {std::byte{0x99}, std::byte{0x01}};
  codec::RecoveryGroupDescriptor xor_descriptor;
  xor_descriptor.key.stream = multiplex.stream;
  xor_descriptor.key.epoch = multiplex.epoch;
  xor_descriptor.key.group_sequence = 2;
  xor_descriptor.first_sequence = multiplex.sequence;
  xor_descriptor.source_count = 2;
  const std::vector<codec::MultiplexFrame> xor_frames{multiplex, xor_second};
  auto xor_symbol =
      codec::create_xor_repair_symbol(xor_descriptor, xor_frames);
  if (!xor_symbol) return 1;
  auto xor_wire = codec::encode_xor_repair_symbol(*xor_symbol);
  if (!xor_wire) return 1;
  auto decoded_xor_symbol = codec::decode_xor_repair_symbol(*xor_wire);
  if (!decoded_xor_symbol) return 1;
  const std::vector<codec::MultiplexFrame> xor_observed{multiplex};
  auto xor_recovered = codec::recover_xor_single_erasure(
      *decoded_xor_symbol, xor_observed);
  if (!xor_recovered ||
      xor_recovered->frame.sequence != xor_second.sequence ||
      xor_recovered->frame.payload != xor_second.payload) {
    return 1;
  }

  codec::StreamingRepairSession repair_session;
  if (!repair_session.begin_group(xor_descriptor)) return 1;
  auto repair_symbol_batch = repair_session.push_repair_symbol(*xor_wire);
  if (!repair_symbol_batch || !repair_symbol_batch->frames.empty()) return 1;
  auto repair_observed_batch = repair_session.push_frame(*encoded_multiplex);
  if (!repair_observed_batch || repair_observed_batch->frames.size() != 1 ||
      repair_observed_batch->frames.front().kind !=
          codec::StreamingRepairFrameKind::observed ||
      repair_observed_batch->frames.front().encoded_frame != *encoded_multiplex) {
    return 1;
  }
  auto repair_sealed_batch = repair_session.seal_group(xor_descriptor.key);
  if (!repair_sealed_batch || !repair_sealed_batch->group_report.has_value() ||
      repair_sealed_batch->group_report->state !=
          codec::RecoveryGroupState::sealed_incomplete ||
      repair_sealed_batch->frames.size() != 1 ||
      repair_sealed_batch->frames.front().kind !=
          codec::StreamingRepairFrameKind::recovered ||
      repair_sealed_batch->frames.front().frame.sequence != xor_second.sequence ||
      repair_sealed_batch->frames.front().frame.payload != xor_second.payload) {
    return 1;
  }

  codec::ExtractedRecord partition_input_a;
  partition_input_a.record.type = codec::RecordType::source_bytes;
  partition_input_a.record.stream =
      codec::derive_stream_id("package-consumer/partition");
  partition_input_a.record.sequence = 1;
  partition_input_a.payload = {std::byte{0x10}};
  partition_input_a.record.payload_size = 1;
  partition_input_a.record.hash = codec::sha256(partition_input_a.payload);

  auto partition_input_b = partition_input_a;
  partition_input_b.record.sequence = 2;
  partition_input_b.payload = {std::byte{0x20}, std::byte{0x21}};
  partition_input_b.record.payload_size = 2;
  partition_input_b.record.hash = codec::sha256(partition_input_b.payload);

  const std::vector<codec::ExtractedRecord> partition_inputs{
      partition_input_a, partition_input_b};
  auto partitions = codec::partition_exact_records(partition_inputs);
  if (!partitions || partitions->size() != 1 ||
      partitions->front().records.size() != 2 ||
      partitions->front().payload_bytes != 3 ||
      partitions->front().stream != partition_input_a.record.stream) {
    return 1;
  }

  PackageObjectStore object_store;
  object_store.bytes_ = {std::byte{0x10}, std::byte{0x20}, std::byte{0x21}};
  const codec::ObjectStoreObjectRef object_ref{
      .store = "package", .key = "records", .version = "v1"};
  const std::vector<codec::DistributedRecordLocation> locations{
      codec::DistributedRecordLocation{
          .record = partition_input_a.record,
          .object = object_ref,
          .offset = 0,
          .length = partition_input_a.record.payload_size,
      },
      codec::DistributedRecordLocation{
          .record = partition_input_b.record,
          .object = object_ref,
          .offset = 1,
          .length = partition_input_b.record.payload_size,
      },
  };

  auto location_index = codec::build_distributed_location_index(locations);
  if (!location_index || location_index->record_count() != 2 ||
      location_index->location_count() != 2) {
    return 1;
  }
  auto location_candidates = codec::resolve_partition_location_candidates(
      *location_index, partitions->front());
  if (!location_candidates || !location_candidates->complete ||
      location_candidates->partition_identity != partitions->front().identity ||
      location_candidates->stream != partitions->front().stream ||
      location_candidates->records.size() != 2 ||
      location_candidates->records[0].candidates.size() != 1 ||
      location_candidates->records[1].candidates.size() != 1) {
    return 1;
  }
  std::vector<codec::DistributedRecordLocation> selected_locations;
  selected_locations.reserve(location_candidates->records.size());
  for (const auto& candidate_set : location_candidates->records) {
    if (candidate_set.candidates.empty()) return 1;
    selected_locations.push_back(candidate_set.candidates.front());
  }

  auto retrieved = codec::retrieve_partition_records(
      object_store, partitions->front(), selected_locations);
  if (!retrieved ||
      retrieved->partition_identity != partitions->front().identity ||
      retrieved->stream != partitions->front().stream ||
      retrieved->backend_name != "package-object-store" ||
      retrieved->records.size() != 2 ||
      retrieved->records[0].record.sequence != partition_input_a.record.sequence ||
      retrieved->records[0].record.hash != partition_input_a.record.hash ||
      retrieved->records[0].payload != partition_input_a.payload ||
      retrieved->records[1].record.sequence != partition_input_b.record.sequence ||
      retrieved->records[1].record.hash != partition_input_b.record.hash ||
      retrieved->records[1].payload != partition_input_b.payload) {
    return 1;
  }

  PackageDistributedProcessor distributed_processor;
  codec::LocalProcessorWorker distributed_worker{
      distributed_processor, "package-local-worker"};
  auto execution = codec::execute_partition(
      distributed_worker, partitions->front(), retrieved->records);
  if (!execution || execution->partition_identity != partitions->front().identity ||
      execution->worker_name != "package-local-worker" ||
      execution->processor_name != "package-distributed" ||
      execution->outputs.size() != 1 ||
      execution->outputs.front().truth != codec::TruthClass::derived) {
    return 1;
  }

  codec::DistributedPartitionLimits schedule_partition_limits;
  schedule_partition_limits.maximum_records_per_partition = 1;
  auto scheduled_partitions =
      codec::partition_exact_records(partition_inputs, schedule_partition_limits);
  if (!scheduled_partitions || scheduled_partitions->size() != 2) return 1;

  PackageDistributedProcessor schedule_processor_a;
  PackageDistributedProcessor schedule_processor_b;
  codec::LocalProcessorWorker schedule_worker_a{
      schedule_processor_a, "package-schedule-a"};
  codec::LocalProcessorWorker schedule_worker_b{
      schedule_processor_b, "package-schedule-b"};
  std::array<codec::DistributedWorker*, 2> schedule_workers{
      &schedule_worker_a, &schedule_worker_b};
  auto scheduled = codec::schedule_partitions(
      schedule_workers, object_store, *location_index, *scheduled_partitions);
  if (!scheduled || scheduled->succeeded != 2 ||
      scheduled->partitions.size() != 2 ||
      scheduled->partitions[0].worker_index != 0 ||
      scheduled->partitions[1].worker_index != 1 ||
      scheduled->partitions[0].status !=
          codec::DistributedPartitionOutcomeStatus::succeeded ||
      scheduled->partitions[1].status !=
          codec::DistributedPartitionOutcomeStatus::succeeded ||
      !scheduled->partitions[0].execution.has_value() ||
      !scheduled->partitions[1].execution.has_value() ||
      scheduled->partitions[0].execution->worker_name != "package-schedule-a" ||
      scheduled->partitions[1].execution->worker_name != "package-schedule-b" ||
      scheduled->partitions[0].selected_locations.size() != 1 ||
      scheduled->partitions[1].selected_locations.size() != 1 ||
      scheduled->partitions[0].selected_locations.front().offset != 0 ||
      scheduled->partitions[1].selected_locations.front().offset != 1) {
    return 1;
  }

  const auto follow_path =
      std::filesystem::temp_directory_path() / "codec-package-consumer-follow.coda";
  std::filesystem::remove(follow_path);
  auto writer_result = codec::CodaWriter::create(follow_path);
  if (!writer_result) return 1;
  auto writer = std::move(*writer_result);
  const auto follow_stream = codec::derive_stream_id("package-consumer/follow");
  const std::vector<std::byte> follow_payload{std::byte{0x11}, std::byte{0x22}};
  if (!writer.append(codec::RecordType::source_bytes, follow_stream, 1, 1,
                     follow_payload)) {
    return 1;
  }
  auto follow_archive = codec::CodaArchive::open(follow_path);
  if (!follow_archive) return 1;
  const codec::SourceExactCursor initial_cursor{};
  auto follow_batch = codec::extract_stream_source_exact_prefix(
      *follow_archive, follow_stream, initial_cursor, 1, 1024);
  if (!follow_batch || follow_batch->records.size() != 1 ||
      follow_batch->records.front().payload != follow_payload ||
      follow_batch->cursor.next_archive_sequence != 1 || follow_batch->finalized) {
    return 1;
  }
  std::filesystem::remove(follow_path);

  return limits.maximum_output_bytes == 0 ||
                 offline.maximum_sources == 0 ||
                 offline_limits.maximum_output_bytes == 0 ||
                 runtime_options.intra_op_threads == 0 ||
                 runtime_options.limits.maximum_input_frames == 0 ||
                 !encoded_model_bundle ||
                 state.frames() != 1
             ? 1
             : 0;
}

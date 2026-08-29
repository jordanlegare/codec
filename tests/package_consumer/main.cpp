#include <codec/archive.hpp>
#include <codec/archive_follow.hpp>
#include <codec/profiles/audio.hpp>
#include <codec/recovery.hpp>
#include <codec/streaming_repair.hpp>
#include <codec/transport.hpp>
#include <codec/xor_recovery.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

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
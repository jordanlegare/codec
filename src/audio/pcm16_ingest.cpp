#include <codec/profiles/audio.hpp>

#include "../capture/capture.hpp"
#include "../core/internal.hpp"
#include "wav_codec.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace codec::profiles::audio {
namespace {

constexpr std::size_t minimum_capture_chunk_bytes = 4096;
constexpr std::size_t maximum_capture_chunk_bytes = 16U * 1024U * 1024U;
constexpr std::uint32_t maximum_capture_redirects = 20;

Result<void> validate_request(const Pcm16WavIngestRequest& request) {
  if (request.source_uri.empty()) {
    return fail(ErrorCode::invalid_argument,
                "audio ingest requires a source URI");
  }
  if (request.archive_path.empty() || request.archive_path.filename().empty()) {
    return fail(ErrorCode::invalid_argument,
                "audio ingest requires an archive file path");
  }
  if (request.end_ns < request.start_ns) {
    return fail(ErrorCode::invalid_argument,
                "audio ingest interval is inverted");
  }
  if (request.capture_chunk_bytes < minimum_capture_chunk_bytes ||
      request.capture_chunk_bytes > maximum_capture_chunk_bytes) {
    return fail(ErrorCode::invalid_argument,
                "audio ingest capture chunk is outside the supported bounds");
  }
  if (request.maximum_source_bytes == 0 ||
      request.maximum_source_bytes >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return fail(ErrorCode::invalid_argument,
                "audio ingest source limit is outside the process bounds");
  }
  if (request.maximum_redirects > maximum_capture_redirects) {
    return fail(ErrorCode::invalid_argument,
                "audio ingest redirect limit is outside the supported bounds");
  }
  if (request.descriptor.type != StreamType::audio ||
      request.descriptor.payload_type != "audio/wav") {
    return fail(ErrorCode::invalid_argument,
                "audio ingest requires an audio/wav stream descriptor");
  }
  auto encoded_descriptor =
      detail::encode_stream_descriptor(request.descriptor);
  if (!encoded_descriptor) return encoded_descriptor.error();

  std::error_code inspection_error;
  const auto status =
      std::filesystem::symlink_status(request.archive_path, inspection_error);
  if (inspection_error == std::errc::no_such_file_or_directory) {
    inspection_error.clear();
  } else if (inspection_error) {
    return fail(ErrorCode::archive_io,
                "cannot inspect audio ingest archive output: " +
                    inspection_error.message());
  }
  if (!inspection_error &&
      status.type() != std::filesystem::file_type::not_found &&
      status.type() != std::filesystem::file_type::none) {
    return fail(ErrorCode::archive_io,
                "audio ingest refuses to replace an existing archive path");
  }
  return {};
}

}  // namespace

Result<Pcm16WavIngestReport> ingest_pcm16_wav(
    const Pcm16WavIngestRequest& request) {
  auto valid = validate_request(request);
  if (!valid) return valid.error();

  auto prepared = detail::PreparedCapture::prepare(
      request.source_uri,
      detail::CaptureOptions{
          .chunk_bytes = request.capture_chunk_bytes,
          .maximum_bytes = request.maximum_source_bytes,
          .maximum_redirects = request.maximum_redirects,
          .deny_private_network = request.deny_private_network,
      });
  if (!prepared) return prepared.error();

  std::vector<std::byte> source_bytes;
  auto captured = prepared->run(
      [&source_bytes, maximum_source_bytes = request.maximum_source_bytes](
          std::span<const std::byte> bytes) -> Result<void> {
        const auto current_size =
            static_cast<std::uint64_t>(source_bytes.size());
        if (current_size > maximum_source_bytes ||
            bytes.size() > maximum_source_bytes - current_size ||
            bytes.size() > source_bytes.max_size() - source_bytes.size()) {
          return fail(ErrorCode::resource_exhausted,
                      "audio ingest source exceeds the configured limit");
        }
        source_bytes.insert(source_bytes.end(), bytes.begin(), bytes.end());
        return {};
      });
  if (!captured) return captured.error();

  auto created = CodaWriter::create(request.archive_path);
  if (!created) return created.error();
  auto writer = std::move(*created);
  auto descriptor = writer.append_stream_descriptor(request.descriptor,
                                                     request.start_ns);
  if (!descriptor) return descriptor.error();
  auto source = writer.append(RecordType::source_bytes,
                              request.descriptor.id, request.start_ns,
                              request.end_ns, source_bytes);
  if (!source) return source.error();

  Pcm16WavIngestReport report{
      .archive_path = request.archive_path,
      .descriptor = *descriptor,
      .source = *source,
      .state = std::nullopt,
      .provenance = std::nullopt,
      .profile_error = std::nullopt,
  };
  const auto finish_source_only =
      [&writer, &report](Error error) -> Result<Pcm16WavIngestReport> {
    auto finalized = writer.finalize();
    if (!finalized) return finalized.error();
    report.profile_error = std::move(error);
    return report;
  };

  auto wav = detail::decode_wav_pcm16(source_bytes);
  if (!wav) return finish_source_only(wav.error());
  auto state = canonicalize_pcm16(*wav);
  if (!state) return finish_source_only(state.error());
  auto encoded = encode_pcm16_state(*state);
  if (!encoded) return finish_source_only(encoded.error());
  auto state_record = writer.append(RecordType::pcm16,
                                    request.descriptor.id, request.start_ns,
                                    request.end_ns, *encoded);
  if (!state_record) return state_record.error();

  const ProvenanceProcess process{
      .operation = "audio.pcm16.canonicalize",
      .implementation_id = "codec-audio-profile",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = request.end_ns,
      .details_type = {},
      .details = {},
  };
  const std::array inputs{*source};
  auto provenance = writer.append_stream_provenance(
      *state_record, TruthClass::state_exact, inputs, process);
  if (!provenance) return provenance.error();
  auto finalized = writer.finalize();
  if (!finalized) return finalized.error();

  report.state = *state_record;
  report.provenance = *provenance;
  return report;
}

}  // namespace codec::profiles::audio

#include "flac_encoder.hpp"

#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace codec::profiles::audio::detail {
namespace {

struct EncoderOutput {
  std::vector<std::byte> bytes;
  std::size_t cursor{};
  std::uint64_t limit{};
  bool exhausted{false};
};

FLAC__StreamEncoderWriteStatus write_callback(
    const FLAC__StreamEncoder*, const FLAC__byte buffer[], std::size_t bytes,
    unsigned, unsigned, void* client_data) {
  auto& output = *static_cast<EncoderOutput*>(client_data);
  if (buffer == nullptr && bytes != 0) {
    return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
  }

  const auto cursor = static_cast<std::uint64_t>(output.cursor);
  const auto count = static_cast<std::uint64_t>(bytes);
  if (cursor > output.limit || count > output.limit - cursor ||
      bytes > std::numeric_limits<std::size_t>::max() - output.cursor) {
    output.exhausted = true;
    return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
  }

  const auto end = output.cursor + bytes;
  if (end > output.bytes.size()) {
    try {
      output.bytes.resize(end);
    } catch (...) {
      return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }
  }
  if (bytes != 0) {
    std::memcpy(output.bytes.data() + output.cursor, buffer, bytes);
  }
  output.cursor = end;
  return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

FLAC__StreamEncoderSeekStatus seek_callback(const FLAC__StreamEncoder*,
                                            FLAC__uint64 offset,
                                            void* client_data) {
  auto& output = *static_cast<EncoderOutput*>(client_data);
  if (offset > output.bytes.size() ||
      offset > std::numeric_limits<std::size_t>::max()) {
    return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
  }
  output.cursor = static_cast<std::size_t>(offset);
  return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}

FLAC__StreamEncoderTellStatus tell_callback(const FLAC__StreamEncoder*,
                                            FLAC__uint64* offset,
                                            void* client_data) {
  if (offset == nullptr) return FLAC__STREAM_ENCODER_TELL_STATUS_ERROR;
  const auto& output = *static_cast<EncoderOutput*>(client_data);
  *offset = static_cast<FLAC__uint64>(output.cursor);
  return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

Result<std::vector<std::byte>> encoder_failure(const EncoderOutput& output,
                                               const char* message) {
  if (output.exhausted) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "verified PCM16 FLAC export exceeds the configured output limit");
  }
  return fail<std::vector<std::byte>>(ErrorCode::internal, message);
}

}  // namespace

Result<std::vector<std::byte>> encode_flac_pcm16(
    const Pcm16State& state, std::uint64_t maximum_output_bytes) {
  if (maximum_output_bytes == 0) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "PCM16 FLAC export output limit must be non-zero");
  }
  if (state.sample_rate == 0 || state.channels == 0 ||
      state.samples.size() % state.channels != 0) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "PCM16 FLAC export requires valid complete PCM16 frames");
  }

  using EncoderPtr =
      std::unique_ptr<FLAC__StreamEncoder, decltype(&FLAC__stream_encoder_delete)>;
  EncoderPtr encoder{FLAC__stream_encoder_new(), &FLAC__stream_encoder_delete};
  if (!encoder) {
    return fail<std::vector<std::byte>>(ErrorCode::internal,
                                       "libFLAC encoder allocation failed");
  }

  const auto frames = static_cast<FLAC__uint64>(state.frames());
  if (!FLAC__stream_encoder_set_verify(encoder.get(), true) ||
      !FLAC__stream_encoder_set_streamable_subset(encoder.get(), true) ||
      !FLAC__stream_encoder_set_channels(encoder.get(), state.channels) ||
      !FLAC__stream_encoder_set_bits_per_sample(encoder.get(), 16) ||
      !FLAC__stream_encoder_set_sample_rate(encoder.get(), state.sample_rate) ||
      !FLAC__stream_encoder_set_compression_level(encoder.get(), 5) ||
      !FLAC__stream_encoder_set_total_samples_estimate(encoder.get(), frames)) {
    return fail<std::vector<std::byte>>(
        ErrorCode::internal,
        "libFLAC rejected the PCM16 encoder configuration");
  }

  EncoderOutput output{
      .bytes = {},
      .cursor = 0,
      .limit = maximum_output_bytes,
      .exhausted = false,
  };
  const auto init = FLAC__stream_encoder_init_stream(
      encoder.get(), write_callback, seek_callback, tell_callback, nullptr,
      &output);
  if (init != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
    return encoder_failure(output, "libFLAC stream encoder initialization failed");
  }

  std::vector<FLAC__int32> pcm;
  try {
    pcm.reserve(state.samples.size());
    for (const auto sample : state.samples) {
      pcm.push_back(static_cast<FLAC__int32>(sample));
    }
  } catch (...) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                       "PCM16 FLAC input staging allocation failed");
  }

  std::size_t frame_offset = 0;
  const auto channels = static_cast<std::size_t>(state.channels);
  while (frame_offset < state.frames()) {
    const auto remaining = state.frames() - frame_offset;
    const auto frame_count = static_cast<unsigned>(std::min<std::size_t>(
        remaining, std::numeric_limits<unsigned>::max()));
    const auto sample_offset = frame_offset * channels;
    if (!FLAC__stream_encoder_process_interleaved(
            encoder.get(), pcm.data() + sample_offset, frame_count)) {
      return encoder_failure(output, "libFLAC failed while encoding PCM16 samples");
    }
    frame_offset += frame_count;
  }

  if (!FLAC__stream_encoder_finish(encoder.get())) {
    return encoder_failure(output,
                           "libFLAC failed to finish or verify the FLAC stream");
  }
  if (output.bytes.size() > maximum_output_bytes) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "verified PCM16 FLAC export exceeds the configured output limit");
  }
  return std::move(output.bytes);
}

}  // namespace codec::profiles::audio::detail

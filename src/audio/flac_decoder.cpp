#include "flac_decoder.hpp"

#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace codec::profiles::audio::detail {
namespace {

struct DecoderState {
  std::span<const std::byte> source;
  std::size_t offset{};
  std::uint64_t maximum_decoded_pcm_bytes{};
  Pcm16State pcm;
  bool saw_streaminfo{false};
  bool decode_failed{false};
  bool exhausted{false};
};

FLAC__StreamDecoderReadStatus read_callback(
    const FLAC__StreamDecoder*, FLAC__byte buffer[], std::size_t* bytes,
    void* client_data) {
  auto& state = *static_cast<DecoderState*>(client_data);
  if (bytes == nullptr || buffer == nullptr || *bytes == 0) {
    state.decode_failed = true;
    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  }
  if (state.offset >= state.source.size()) {
    *bytes = 0;
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }
  const auto count = std::min(*bytes, state.source.size() - state.offset);
  std::memcpy(buffer, state.source.data() + state.offset, count);
  state.offset += count;
  *bytes = count;
  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderSeekStatus seek_callback(const FLAC__StreamDecoder*,
                                            FLAC__uint64 offset,
                                            void* client_data) {
  auto& state = *static_cast<DecoderState*>(client_data);
  if (offset > state.source.size() ||
      offset > std::numeric_limits<std::size_t>::max()) {
    return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
  }
  state.offset = static_cast<std::size_t>(offset);
  return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

FLAC__StreamDecoderTellStatus tell_callback(const FLAC__StreamDecoder*,
                                            FLAC__uint64* offset,
                                            void* client_data) {
  if (offset == nullptr) return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
  const auto& state = *static_cast<DecoderState*>(client_data);
  *offset = static_cast<FLAC__uint64>(state.offset);
  return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus length_callback(const FLAC__StreamDecoder*,
                                                FLAC__uint64* length,
                                                void* client_data) {
  if (length == nullptr) return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
  const auto& state = *static_cast<DecoderState*>(client_data);
  *length = static_cast<FLAC__uint64>(state.source.size());
  return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

FLAC__bool eof_callback(const FLAC__StreamDecoder*, void* client_data) {
  const auto& state = *static_cast<DecoderState*>(client_data);
  return state.offset >= state.source.size();
}

void metadata_callback(const FLAC__StreamDecoder*,
                       const FLAC__StreamMetadata* metadata,
                       void* client_data) {
  auto& state = *static_cast<DecoderState*>(client_data);
  if (metadata == nullptr || metadata->type != FLAC__METADATA_TYPE_STREAMINFO) {
    return;
  }
  const auto& info = metadata->data.stream_info;
  state.saw_streaminfo = true;
  if (info.sample_rate == 0 || info.channels == 0 ||
      info.bits_per_sample != 16) {
    state.decode_failed = true;
    return;
  }
  state.pcm.sample_rate = info.sample_rate;
  state.pcm.channels = static_cast<std::uint16_t>(info.channels);

  if (info.total_samples == 0) return;
  if (info.total_samples > std::numeric_limits<std::size_t>::max()) {
    state.exhausted = true;
    return;
  }
  const auto frames = static_cast<std::size_t>(info.total_samples);
  if (frames > std::numeric_limits<std::size_t>::max() / info.channels) {
    state.exhausted = true;
    return;
  }
  const auto samples = frames * static_cast<std::size_t>(info.channels);
  if (samples > std::numeric_limits<std::uint64_t>::max() / 2 ||
      static_cast<std::uint64_t>(samples) * 2 >
          state.maximum_decoded_pcm_bytes ||
      samples > state.pcm.samples.max_size()) {
    state.exhausted = true;
    return;
  }
  try {
    state.pcm.samples.reserve(samples);
  } catch (...) {
    state.exhausted = true;
  }
}

FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder*, const FLAC__Frame* frame,
    const FLAC__int32* const buffer[], void* client_data) {
  auto& state = *static_cast<DecoderState*>(client_data);
  if (frame == nullptr || buffer == nullptr || !state.saw_streaminfo ||
      state.decode_failed || state.exhausted) {
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (frame->header.sample_rate != state.pcm.sample_rate ||
      frame->header.channels != state.pcm.channels ||
      frame->header.bits_per_sample != 16) {
    state.decode_failed = true;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  const auto channels = static_cast<std::size_t>(frame->header.channels);
  const auto blocksize = static_cast<std::size_t>(frame->header.blocksize);
  if (channels == 0 || blocksize >
                           std::numeric_limits<std::size_t>::max() / channels) {
    state.decode_failed = true;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  const auto new_samples = blocksize * channels;
  if (new_samples > state.pcm.samples.max_size() - state.pcm.samples.size()) {
    state.exhausted = true;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  const auto total_samples = state.pcm.samples.size() + new_samples;
  if (total_samples > std::numeric_limits<std::uint64_t>::max() / 2 ||
      static_cast<std::uint64_t>(total_samples) * 2 >
          state.maximum_decoded_pcm_bytes) {
    state.exhausted = true;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  try {
    state.pcm.samples.reserve(total_samples);
    for (std::size_t sample = 0; sample < blocksize; ++sample) {
      for (std::size_t channel = 0; channel < channels; ++channel) {
        if (buffer[channel] == nullptr) {
          state.decode_failed = true;
          return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        const auto value = buffer[channel][sample];
        if (value < std::numeric_limits<std::int16_t>::min() ||
            value > std::numeric_limits<std::int16_t>::max()) {
          state.decode_failed = true;
          return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        state.pcm.samples.push_back(static_cast<std::int16_t>(value));
      }
    }
  } catch (...) {
    state.exhausted = true;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void error_callback(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus,
                    void* client_data) {
  static_cast<DecoderState*>(client_data)->decode_failed = true;
}

Result<Pcm16State> decoder_failure(const DecoderState& state,
                                   const char* message) {
  if (state.exhausted) {
    return fail<Pcm16State>(
        ErrorCode::resource_exhausted,
        "native FLAC decoded PCM exceeds the configured output limit");
  }
  return fail<Pcm16State>(ErrorCode::decode, message);
}

}  // namespace

Result<Pcm16State> decode_flac_pcm16(
    std::span<const std::byte> source,
    std::uint64_t maximum_decoded_pcm_bytes) {
  if (maximum_decoded_pcm_bytes == 0) {
    return fail<Pcm16State>(ErrorCode::invalid_argument,
                            "native FLAC decoded PCM limit must be non-zero");
  }
  constexpr std::byte magic[] = {
      std::byte{'f'}, std::byte{'L'}, std::byte{'a'}, std::byte{'C'}};
  if (source.size() < 4 || !std::equal(std::begin(magic), std::end(magic),
                                       source.begin())) {
    return fail<Pcm16State>(ErrorCode::decode,
                            "audio source is not native FLAC");
  }

  using DecoderPtr =
      std::unique_ptr<FLAC__StreamDecoder, decltype(&FLAC__stream_decoder_delete)>;
  DecoderPtr decoder{FLAC__stream_decoder_new(), &FLAC__stream_decoder_delete};
  if (!decoder) {
    return fail<Pcm16State>(ErrorCode::resource_exhausted,
                            "libFLAC decoder allocation failed");
  }
  if (!FLAC__stream_decoder_set_md5_checking(decoder.get(), true)) {
    return fail<Pcm16State>(ErrorCode::decode,
                            "libFLAC rejected MD5 checking configuration");
  }

  DecoderState state{
      .source = source,
      .offset = 0,
      .maximum_decoded_pcm_bytes = maximum_decoded_pcm_bytes,
      .pcm = {},
      .saw_streaminfo = false,
      .decode_failed = false,
      .exhausted = false,
  };
  const auto init = FLAC__stream_decoder_init_stream(
      decoder.get(), read_callback, seek_callback, tell_callback,
      length_callback, eof_callback, write_callback, metadata_callback,
      error_callback, &state);
  if (init != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
    return fail<Pcm16State>(ErrorCode::decode,
                            "libFLAC stream decoder initialization failed");
  }

  if (!FLAC__stream_decoder_process_until_end_of_metadata(decoder.get()) ||
      !state.saw_streaminfo || state.decode_failed || state.exhausted) {
    FLAC__stream_decoder_finish(decoder.get());
    return decoder_failure(state,
                           "native FLAC metadata is malformed or unsupported");
  }
  if (!FLAC__stream_decoder_process_until_end_of_stream(decoder.get())) {
    FLAC__stream_decoder_finish(decoder.get());
    return decoder_failure(state, "native FLAC sample decoding failed");
  }
  if (!FLAC__stream_decoder_finish(decoder.get())) {
    return decoder_failure(state,
                           "native FLAC integrity verification failed");
  }
  if (state.decode_failed || state.exhausted || !state.saw_streaminfo ||
      state.pcm.channels == 0 ||
      state.pcm.samples.size() % state.pcm.channels != 0) {
    return decoder_failure(state,
                           "native FLAC decoded PCM geometry is invalid");
  }
  return std::move(state.pcm);
}

}  // namespace codec::profiles::audio::detail

#pragma once

#include <codec/audio.hpp>
#include <codec/inference.hpp>
#include <codec/profiles/audio_export.hpp>
#include <codec/profiles/audio_flac_export.hpp>
#include <codec/profiles/audio_ingest.hpp>
#include <codec/profiles/audio_model_bundle.hpp>
#include <codec/profiles/audio_onnx_cpu_runtime.hpp>
#include <codec/profiles/audio_offline_separation.hpp>
#include <codec/profiles/audio_state_reader.hpp>

namespace codec::profiles::audio {

using ::codec::Pcm16State;
using ::codec::SeparationBackend;
using ::codec::SeparationRequest;
using ::codec::SeparationResult;
using ::codec::WavPcm16;

using ::codec::canonicalize_pcm16;
using ::codec::decode_pcm16_state;
using ::codec::default_separation_backend;
using ::codec::encode_pcm16_state;

}  // namespace codec::profiles::audio

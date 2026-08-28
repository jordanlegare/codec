#pragma once

#include <codec/audio.hpp>
#include <codec/inference.hpp>
#include <codec/profiles/audio_ingest.hpp>
#include <codec/statement.hpp>
#include <codec/watermark.hpp>

namespace codec::profiles::audio {

using ::codec::CarrierBand;
using ::codec::FeedStatement;
using ::codec::Pcm16State;
using ::codec::SeparationBackend;
using ::codec::SeparationRequest;
using ::codec::SeparationResult;
using ::codec::StatementState;
using ::codec::StatementVerification;
using ::codec::WatermarkEmbedReport;
using ::codec::WatermarkObservation;
using ::codec::WatermarkPolicy;
using ::codec::WavPcm16;

using ::codec::carrier_band_name;
using ::codec::canonicalize_pcm16;
using ::codec::decode_pcm16_state;
using ::codec::default_separation_backend;
using ::codec::detect_watermarks;
using ::codec::embed_watermark;
using ::codec::encode_pcm16_state;
using ::codec::generate_ed25519_keypair;
using ::codec::issue_statement;
using ::codec::statement_state_name;
using ::codec::verify_statement;

}  // namespace codec::profiles::audio

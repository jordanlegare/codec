#include <codec/codec_c.h>

#include <codec/archive.hpp>
#include <codec/engine.hpp>

#include "../core/internal.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <utility>

struct codec_engine {
  codec::Engine value;
};

struct codec_archive {
  codec::CodaArchive value;
};

namespace {

codec_status_t status_for(codec::ErrorCode code) {
  switch (code) {
    case codec::ErrorCode::ok: return CODEC_STATUS_OK;
    case codec::ErrorCode::invalid_argument:
      return CODEC_STATUS_INVALID_ARGUMENT;
    case codec::ErrorCode::archive_io: return CODEC_STATUS_IO;
    case codec::ErrorCode::archive_corrupt: return CODEC_STATUS_CORRUPT;
    case codec::ErrorCode::network:
    case codec::ErrorCode::protocol: return CODEC_STATUS_NETWORK;
    case codec::ErrorCode::unauthorized_source:
      return CODEC_STATUS_UNAUTHORIZED;
    case codec::ErrorCode::resource_exhausted:
      return CODEC_STATUS_RESOURCE_EXHAUSTED;
    case codec::ErrorCode::decode: return CODEC_STATUS_DECODE;
    case codec::ErrorCode::model_incompatible:
      return CODEC_STATUS_UNAVAILABLE;
    default: return CODEC_STATUS_INTERNAL;
  }
}

void set_error(codec_error_t* output, const codec::Error& error) {
  if (output == nullptr || output->size < sizeof(codec_error_t) ||
      output->abi_version != CODEC_ABI_VERSION) {
    return;
  }
  std::free(output->message);
  output->message = static_cast<char*>(std::malloc(error.message.size() + 1));
  if (output->message != nullptr) {
    std::memcpy(output->message, error.message.c_str(), error.message.size() + 1);
  }
  output->code = static_cast<int32_t>(error.code);
  output->retryable = error.retryable ? 1 : 0;
}

codec_status_t invalid(codec_error_t* error, std::string message) {
  const codec::Error detail{codec::ErrorCode::invalid_argument,
                            std::move(message), false};
  set_error(error, detail);
  return CODEC_STATUS_INVALID_ARGUMENT;
}

codec_status_t unexpected(codec_error_t* error) {
  const codec::Error detail{codec::ErrorCode::internal,
                            "unexpected exception in CODEC C ABI", false};
  set_error(error, detail);
  return CODEC_STATUS_INTERNAL;
}

}  // namespace

extern "C" codec_status_t codec_engine_create(
    const codec_engine_config_t* config, codec_engine_t** output,
    codec_error_t* error) {
  try {
    if (config == nullptr || output == nullptr ||
        config->size < sizeof(codec_engine_config_t) ||
        config->abi_version != CODEC_ABI_VERSION) {
      return invalid(error, "invalid engine configuration or output handle");
    }
    *output = nullptr;
    codec::EngineConfig native;
    native.capture_chunk_bytes = config->capture_chunk_bytes;
    native.maximum_feed_bytes = config->maximum_feed_bytes;
    native.maximum_redirects = config->maximum_redirects;
    native.deny_private_network = config->deny_private_network != 0;
    auto engine = codec::Engine::create(native);
    if (!engine) {
      set_error(error, engine.error());
      return status_for(engine.error().code);
    }
    *output = new codec_engine_t{std::move(*engine)};
    codec_error_clear(error);
    return CODEC_STATUS_OK;
  } catch (...) {
    return unexpected(error);
  }
}

extern "C" codec_status_t codec_engine_record_file(
    codec_engine_t* engine, const char* input_path, const char* feed_label,
    const char* archive_path, codec_error_t* error) {
  try {
    if (engine == nullptr || input_path == nullptr || feed_label == nullptr ||
        archive_path == nullptr) {
      return invalid(error, "record_file requires non-null arguments");
    }
    auto report = engine->value.record(
        {codec::FeedSpec{.uri = input_path, .label = feed_label}}, archive_path);
    if (!report) {
      set_error(error, report.error());
      return status_for(report.error().code);
    }
    codec_error_clear(error);
    return CODEC_STATUS_OK;
  } catch (...) {
    return unexpected(error);
  }
}

extern "C" void codec_engine_destroy(codec_engine_t* engine) { delete engine; }

extern "C" codec_status_t codec_archive_open(const char* archive_path,
                                              codec_archive_t** output,
                                              codec_error_t* error) {
  try {
    if (archive_path == nullptr || output == nullptr) {
      return invalid(error, "archive_open requires a path and output handle");
    }
    *output = nullptr;
    auto archive = codec::CodaArchive::open(archive_path);
    if (!archive) {
      set_error(error, archive.error());
      return status_for(archive.error().code);
    }
    *output = new codec_archive_t{std::move(*archive)};
    codec_error_clear(error);
    return CODEC_STATUS_OK;
  } catch (...) {
    return unexpected(error);
  }
}

extern "C" codec_status_t codec_archive_verify(
    codec_archive_t* archive, codec_verification_report_t* output,
    codec_error_t* error) {
  try {
    if (archive == nullptr || output == nullptr ||
        output->size < sizeof(codec_verification_report_t) ||
        output->abi_version != CODEC_ABI_VERSION) {
      return invalid(error, "invalid archive or verification report");
    }
    const auto report = archive->value.verify();
    output->ok = report.ok ? 1 : 0;
    output->finalized = report.finalized ? 1 : 0;
    output->committed_records = report.committed_records;
    output->verified_payload_bytes = report.verified_payload_bytes;
    output->valid_prefix_bytes = report.valid_prefix_bytes;
    output->file_bytes = report.file_bytes;
    if (!report.ok) {
      const codec::Error detail{report.error_code, report.message, false};
      set_error(error, detail);
      return status_for(report.error_code);
    }
    codec_error_clear(error);
    return CODEC_STATUS_OK;
  } catch (...) {
    return unexpected(error);
  }
}

extern "C" codec_status_t codec_archive_extract_feed(
    codec_archive_t* archive, const char* feed_label, const char* output_path,
    codec_error_t* error) {
  try {
    if (archive == nullptr || feed_label == nullptr || output_path == nullptr) {
      return invalid(error, "extract_feed requires non-null arguments");
    }
    auto bytes = archive->value.extract_feed(feed_label);
    if (!bytes) {
      set_error(error, bytes.error());
      return status_for(bytes.error().code);
    }
    auto written = codec::detail::write_file(output_path, *bytes);
    if (!written) {
      set_error(error, written.error());
      return status_for(written.error().code);
    }
    codec_error_clear(error);
    return CODEC_STATUS_OK;
  } catch (...) {
    return unexpected(error);
  }
}

extern "C" void codec_archive_destroy(codec_archive_t* archive) {
  delete archive;
}

extern "C" void codec_error_clear(codec_error_t* error) {
  if (error == nullptr || error->size < sizeof(codec_error_t) ||
      error->abi_version != CODEC_ABI_VERSION) {
    return;
  }
  std::free(error->message);
  error->message = nullptr;
  error->code = 0;
  error->retryable = 0;
}

extern "C" const char* codec_version_string(void) { return "0.1.0"; }

#pragma once

#include <codec/archive.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace codec::profiles::audio {

struct Pcm16WavIngestRequest {
  std::string source_uri;
  std::filesystem::path archive_path;
  StreamDescriptor descriptor;
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::size_t capture_chunk_bytes{256U * 1024U};
  std::uint64_t maximum_source_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint32_t maximum_redirects{5};
  bool deny_private_network{true};
};

struct Pcm16WavIngestReport {
  std::filesystem::path archive_path;
  RecordInfo descriptor;
  RecordInfo source;
  std::optional<RecordInfo> state;
  std::optional<RecordInfo> provenance;
  std::optional<Error> profile_error;

  bool state_exact() const noexcept {
    return state.has_value() && provenance.has_value() &&
           !profile_error.has_value();
  }
};

Result<Pcm16WavIngestReport> ingest_pcm16_wav(
    const Pcm16WavIngestRequest& request);

}  // namespace codec::profiles::audio

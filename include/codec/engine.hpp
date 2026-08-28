#pragma once

#include <codec/archive.hpp>
#include <codec/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace codec {

struct StreamSpec {
  std::string uri;
  StreamDescriptor descriptor;
  bool preserve_source{true};
  std::uint64_t maximum_bytes{16ULL * 1024ULL * 1024ULL * 1024ULL};
};

struct FeedSpec {
  std::string uri;
  std::string label;
  bool preserve_source{true};
  std::uint64_t maximum_bytes{16ULL * 1024ULL * 1024ULL * 1024ULL};
};

struct EngineConfig {
  std::size_t capture_chunk_bytes{256U * 1024U};
  std::uint64_t maximum_feed_bytes{16ULL * 1024ULL * 1024ULL * 1024ULL};
  std::uint32_t maximum_redirects{5};
  bool deny_private_network{true};
};

struct StreamRecordingReport {
  std::filesystem::path archive;
  std::size_t streams_recorded{};
  std::uint64_t source_bytes{};
  std::uint64_t source_records{};
};

struct RecordingReport {
  std::filesystem::path archive;
  std::size_t feeds_recorded{};
  std::uint64_t source_bytes{};
  std::uint64_t source_records{};
};

struct Capabilities {
  bool coda_archive{true};
  bool file_capture{true};
  bool http_capture{true};
  bool pcm16_wav{true};
  bool w0_ed25519{true};
  bool w1_reference{true};
  bool w2_reference{true};
  bool neural_separation{false};
  bool gpu_inference{false};
};

class Engine {
 public:
  static Result<Engine> create(EngineConfig config);
  static Capabilities capabilities() noexcept;

  Result<StreamRecordingReport> record_streams(
      const std::vector<StreamSpec>& streams,
      const std::filesystem::path& archive_path) const;
  Result<RecordingReport> record(const std::vector<FeedSpec>& feeds,
                                 const std::filesystem::path& archive_path) const;

 private:
  explicit Engine(EngineConfig config) : config_(config) {}
  EngineConfig config_;
};

}  // namespace codec

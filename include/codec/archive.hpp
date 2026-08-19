#pragma once

#include <codec/integrity.hpp>
#include <codec/result.hpp>
#include <codec/stream.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace codec {

// The executable MVP uses a provisional profile with distinct magic and
// version 0. It is intentionally not the frozen normative CODA v1 schema.
inline constexpr std::uint32_t coda_format_version = 0;
inline constexpr std::uint32_t coda_development_profile_version = 1;
inline constexpr std::uint64_t coda_header_size = 64;
inline constexpr std::uint64_t coda_record_envelope_size = 96;
inline constexpr std::uint64_t coda_commit_trailer_size = 40;

enum class RecordType : std::uint16_t {
  feed_descriptor = 1,
  source_bytes = 2,
  pcm16 = 3,
  gap = 4,
  watermark_statement = 20,
  watermark_observation = 21,
  feed_identity_event = 22,
  audit_event = 30,
  final_index = 0xfffe,
};

const char* record_type_name(RecordType type) noexcept;

struct RecordInfo {
  RecordType type{RecordType::source_bytes};
  std::uint64_t sequence{};
  StreamId stream{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::uint64_t payload_size{};
  std::uint64_t file_offset{};
  Sha256 hash{};
};

struct VerificationReport {
  bool ok{false};
  bool finalized{false};
  std::uint64_t committed_records{};
  std::uint64_t verified_payload_bytes{};
  std::uint64_t valid_prefix_bytes{};
  std::uint64_t file_bytes{};
  ErrorCode error_code{ErrorCode::ok};
  std::string message;
};

struct RepairReport {
  std::uint64_t recovered_records{};
  std::uint64_t recovered_payload_bytes{};
  std::uint64_t discarded_tail_bytes{};
};

struct FeedInfo {
  StreamId stream{};
  std::string label;
  std::string uri;
  bool preserve_source{true};
};

enum class ArchiveReadPolicy {
  complete_archive,
  verified_prefix,
};

class CodaWriter {
 public:
  CodaWriter(CodaWriter&&) noexcept;
  CodaWriter& operator=(CodaWriter&&) noexcept;
  ~CodaWriter();

  CodaWriter(const CodaWriter&) = delete;
  CodaWriter& operator=(const CodaWriter&) = delete;

  static Result<CodaWriter> create(const std::filesystem::path& path);
  Result<RecordInfo> append(RecordType type, const StreamId& stream,
                            std::int64_t start_ns, std::int64_t end_ns,
                            std::span<const std::byte> payload,
                            std::uint16_t flags = 0);
  Result<void> finalize();
  bool finalized() const noexcept;

 private:
  struct Impl;
  explicit CodaWriter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

class CodaArchive {
 public:
  static Result<CodaArchive> open(const std::filesystem::path& path);
  VerificationReport verify() const;
  Result<std::vector<RecordInfo>> records(
      ArchiveReadPolicy policy = ArchiveReadPolicy::complete_archive) const;
  Result<std::vector<FeedInfo>> feeds(
      ArchiveReadPolicy policy = ArchiveReadPolicy::complete_archive) const;
  Result<std::vector<std::byte>> read_payload(const RecordInfo& record) const;
  Result<std::vector<std::byte>> extract_stream(const StreamId& stream,
      RecordType type = RecordType::source_bytes,
      ArchiveReadPolicy policy = ArchiveReadPolicy::complete_archive) const;
  Result<std::vector<std::byte>> extract_feed(
      std::string_view label,
      ArchiveReadPolicy policy = ArchiveReadPolicy::complete_archive) const;
  static Result<RepairReport> repair(const std::filesystem::path& source,
                                     const std::filesystem::path& destination);
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  explicit CodaArchive(std::filesystem::path path) : path_(std::move(path)) {}
  std::filesystem::path path_;
};

}  // namespace codec

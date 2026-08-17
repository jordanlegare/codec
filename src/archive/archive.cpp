#include <codec/archive.hpp>

#include "../core/internal.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace codec {
namespace {

constexpr std::array<std::byte, 8> header_magic{
    std::byte{'C'}, std::byte{'O'}, std::byte{'D'}, std::byte{'A'},
    std::byte{'D'}, std::byte{'E'}, std::byte{'V'}, std::byte{'\n'}};
constexpr std::array<std::byte, 4> record_magic{
    std::byte{'C'}, std::byte{'D'}, std::byte{'R'}, std::byte{'1'}};
constexpr std::array<std::byte, 4> commit_magic{
    std::byte{'C'}, std::byte{'M'}, std::byte{'T'}, std::byte{'1'}};

template <typename Integer>
void put_le(std::span<std::byte> output, std::size_t offset, Integer value) {
  using Unsigned = std::make_unsigned_t<Integer>;
  auto bits = static_cast<Unsigned>(value);
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    output[offset + index] = static_cast<std::byte>(bits & 0xffU);
    bits >>= 8U;
  }
}

template <typename Integer>
Integer get_le(std::span<const std::byte> input, std::size_t offset) {
  using Unsigned = std::make_unsigned_t<Integer>;
  Unsigned bits = 0;
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    bits |= static_cast<Unsigned>(
                static_cast<std::uint8_t>(input[offset + index]))
            << (index * 8U);
  }
  return static_cast<Integer>(bits);
}

bool bytes_equal(std::span<const std::byte> left,
                 std::span<const std::byte> right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

Result<std::array<std::byte, coda_header_size>> make_header() {
  std::array<std::byte, coda_header_size> header{};
  std::copy(header_magic.begin(), header_magic.end(), header.begin());
  put_le<std::uint32_t>(header, 8, coda_format_version);
  put_le<std::uint32_t>(header, 12,
                        static_cast<std::uint32_t>(coda_header_size));
  put_le<std::uint32_t>(header, 16, 0U);
  std::array<unsigned char, 16> uuid{};
  if (RAND_bytes(uuid.data(), static_cast<int>(uuid.size())) != 1) {
    return fail<std::array<std::byte, coda_header_size>>(
        ErrorCode::internal, "cryptographic archive UUID generation failed");
  }
  for (std::size_t index = 0; index < uuid.size(); ++index) {
    header[24 + index] = static_cast<std::byte>(uuid[index]);
  }
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  put_le<std::int64_t>(header, 40, nanoseconds);
  put_le<std::uint32_t>(header, 20, coda_development_profile_version);
  put_le<std::uint32_t>(header, 60,
                        detail::crc32c(std::span{header}.first(60)));
  return header;
}

Result<void> validate_header(std::span<const std::byte> header) {
  if (header.size() != coda_header_size ||
      !bytes_equal(header.first(header_magic.size()), header_magic)) {
    return fail(ErrorCode::archive_corrupt, "invalid CODA header magic");
  }
  if (get_le<std::uint32_t>(header, 8) != coda_format_version ||
      get_le<std::uint32_t>(header, 12) != coda_header_size) {
    return fail(ErrorCode::archive_corrupt,
                "unsupported CODA format version or header size");
  }
  if (get_le<std::uint32_t>(header, 20) !=
          coda_development_profile_version ||
      detail::crc32c(header.first(60)) !=
      get_le<std::uint32_t>(header, 60)) {
    return fail(ErrorCode::archive_corrupt,
                "CODA development-profile header mismatch");
  }
  return {};
}

std::vector<std::byte> join_for_hash(std::span<const std::byte> envelope,
                                    std::span<const std::byte> payload) {
  std::vector<std::byte> joined;
  joined.reserve(envelope.size() + payload.size());
  joined.insert(joined.end(), envelope.begin(), envelope.end());
  joined.insert(joined.end(), payload.begin(), payload.end());
  return joined;
}

bool write_all_fd(int descriptor, std::span<const std::byte> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count =
        ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

struct ScanResult {
  VerificationReport report;
  std::vector<RecordInfo> records;
};

Result<ScanResult> scan_archive(const std::filesystem::path& path) {
  std::error_code size_error;
  const auto file_size = std::filesystem::file_size(path, size_error);
  if (size_error) {
    return fail<ScanResult>(ErrorCode::archive_io,
                            "cannot determine archive size: " +
                                size_error.message());
  }
  ScanResult result;
  result.report.file_bytes = file_size;
  result.report.valid_prefix_bytes = 0;
  result.report.error_code = ErrorCode::ok;

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return fail<ScanResult>(ErrorCode::archive_io,
                            "cannot open archive: " + path.string());
  }
  std::array<std::byte, coda_header_size> header{};
  input.read(reinterpret_cast<char*>(header.data()),
             static_cast<std::streamsize>(header.size()));
  if (input.gcount() != static_cast<std::streamsize>(header.size())) {
    result.report.error_code = ErrorCode::archive_corrupt;
    result.report.message = "truncated CODA header";
    return result;
  }
  auto header_status = validate_header(header);
  if (!header_status) {
    result.report.error_code = header_status.error().code;
    result.report.message = header_status.error().message;
    return result;
  }
  std::uint64_t offset = coda_header_size;
  result.report.valid_prefix_bytes = offset;
  Sha256 previous_hash{};
  std::uint64_t expected_sequence = 0;

  while (offset < file_size) {
    if (file_size - offset < coda_record_envelope_size) {
      result.report.error_code = ErrorCode::archive_corrupt;
      result.report.message = "incomplete record envelope at archive tail";
      break;
    }
    std::array<std::byte, coda_record_envelope_size> envelope{};
    input.seekg(static_cast<std::streamoff>(offset));
    input.read(reinterpret_cast<char*>(envelope.data()),
               static_cast<std::streamsize>(envelope.size()));
    if (!input ||
        !bytes_equal(std::span{envelope}.first(record_magic.size()),
                     record_magic)) {
      result.report.error_code = ErrorCode::archive_corrupt;
      result.report.message = "invalid record envelope magic";
      break;
    }
    if (get_le<std::uint32_t>(envelope, 8) != coda_record_envelope_size ||
        detail::crc32c(std::span{envelope}.first(92)) !=
            get_le<std::uint32_t>(envelope, 92)) {
      result.report.error_code = ErrorCode::archive_corrupt;
      result.report.message = "invalid record envelope size or CRC";
      break;
    }
    const auto payload_size = get_le<std::uint64_t>(envelope, 12);
    if (payload_size > std::numeric_limits<std::size_t>::max() ||
        payload_size > file_size - offset - coda_record_envelope_size ||
        file_size - offset - coda_record_envelope_size - payload_size <
            coda_commit_trailer_size) {
      result.report.error_code = ErrorCode::archive_corrupt;
      result.report.message = "incomplete record payload or commit trailer";
      break;
    }
    std::vector<std::byte> payload(static_cast<std::size_t>(payload_size));
    if (!payload.empty()) {
      input.read(reinterpret_cast<char*>(payload.data()),
                 static_cast<std::streamsize>(payload.size()));
    }
    std::array<std::byte, coda_commit_trailer_size> trailer{};
    input.read(reinterpret_cast<char*>(trailer.data()),
               static_cast<std::streamsize>(trailer.size()));
    if (!input ||
        !bytes_equal(std::span{trailer}.first(commit_magic.size()),
                     commit_magic) ||
        detail::crc32c(std::span{trailer}.first(36)) !=
            get_le<std::uint32_t>(trailer, 36)) {
      result.report.error_code = ErrorCode::archive_corrupt;
      result.report.message = "invalid or incomplete commit trailer";
      break;
    }
    const auto record_hash = sha256(join_for_hash(envelope, payload));
    if (!std::equal(record_hash.begin(), record_hash.end(),
                    trailer.begin() + 4,
                    [](std::uint8_t left, std::byte right) {
                      return left == static_cast<std::uint8_t>(right);
                    })) {
      result.report.error_code = ErrorCode::archive_corrupt;
      result.report.message = "record SHA-256 mismatch";
      break;
    }
    if (!std::equal(previous_hash.begin(), previous_hash.end(),
                    envelope.begin() + 60,
                    [](std::uint8_t left, std::byte right) {
                      return left == static_cast<std::uint8_t>(right);
                    })) {
      result.report.error_code = ErrorCode::archive_corrupt;
      result.report.message = "record hash-chain mismatch";
      break;
    }
    const auto sequence = get_le<std::uint64_t>(envelope, 20);
    if (sequence != expected_sequence) {
      result.report.error_code = ErrorCode::archive_corrupt;
      result.report.message = "non-contiguous record sequence";
      break;
    }
    RecordInfo info;
    info.type = static_cast<RecordType>(get_le<std::uint16_t>(envelope, 4));
    info.sequence = sequence;
    for (std::size_t index = 0; index < info.stream.bytes.size(); ++index) {
      info.stream.bytes[index] =
          static_cast<std::uint8_t>(envelope[28 + index]);
    }
    info.start_ns = get_le<std::int64_t>(envelope, 44);
    info.end_ns = get_le<std::int64_t>(envelope, 52);
    info.payload_size = payload_size;
    info.file_offset = offset;
    info.hash = record_hash;
    result.records.push_back(info);
    result.report.committed_records += 1;
    result.report.verified_payload_bytes += payload_size;
    result.report.finalized = info.type == RecordType::final_index;
    previous_hash = record_hash;
    expected_sequence += 1;
    offset += coda_record_envelope_size + payload_size +
              coda_commit_trailer_size;
    result.report.valid_prefix_bytes = offset;
  }

  if (offset == file_size) {
    result.report.ok = true;
    result.report.error_code = ErrorCode::ok;
    result.report.message = result.report.finalized
                                ? "archive verified and finalized"
                                : "complete streaming archive verified";
  } else {
    result.report.ok = false;
  }
  return result;
}

std::vector<std::byte> make_index_payload(
    const std::vector<RecordInfo>& records) {
  std::ostringstream json;
  json << "{\"format\":\"CODA-index-v1\",\"records\":[";
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto& record = records[index];
    if (index != 0) json << ',';
    json << "{\"sequence\":" << record.sequence << ",\"offset\":"
         << record.file_offset << ",\"type\":"
         << static_cast<std::uint16_t>(record.type) << ",\"stream\":\""
         << to_string(record.stream) << "\",\"bytes\":"
         << record.payload_size << '}';
  }
  json << "]}";
  const auto text = json.str();
  std::vector<std::byte> output(text.size());
  std::memcpy(output.data(), text.data(), text.size());
  return output;
}

}  // namespace

struct CodaWriter::Impl {
  std::filesystem::path path;
  int output{-1};
  std::uint64_t next_sequence{};
  Sha256 previous_hash{};
  std::vector<RecordInfo> records;
  bool is_finalized{false};

  ~Impl() {
    if (output >= 0) ::close(output);
  }
};

CodaWriter::CodaWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CodaWriter::CodaWriter(CodaWriter&&) noexcept = default;
CodaWriter& CodaWriter::operator=(CodaWriter&&) noexcept = default;
CodaWriter::~CodaWriter() = default;

Result<CodaWriter> CodaWriter::create(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail<CodaWriter>(ErrorCode::invalid_argument,
                            "archive path must not be empty");
  }
  auto impl = std::make_unique<Impl>();
  impl->path = path;
  impl->output = ::open(path.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        0644);
  if (impl->output < 0) {
    return fail<CodaWriter>(ErrorCode::archive_io,
                            errno == EEXIST
                                ? "archive output already exists; refusing to replace it"
                                : "cannot create archive: " +
                                      std::string{std::strerror(errno)});
  }
  auto header = make_header();
  if (!header) {
    ::close(impl->output);
    impl->output = -1;
    ::unlink(path.c_str());
    return header.error();
  }
  if (!write_all_fd(impl->output, *header) || ::fsync(impl->output) != 0) {
    ::close(impl->output);
    impl->output = -1;
    ::unlink(path.c_str());
    return fail<CodaWriter>(ErrorCode::archive_io,
                            "cannot write CODA header");
  }
  return CodaWriter{std::move(impl)};
}

Result<RecordInfo> CodaWriter::append(RecordType type, const StreamId& stream,
                                      std::int64_t start_ns,
                                      std::int64_t end_ns,
                                      std::span<const std::byte> payload,
                                      std::uint16_t flags) {
  if (!impl_ || impl_->is_finalized) {
    return fail<RecordInfo>(ErrorCode::invalid_argument,
                            "cannot append to a finalized archive");
  }
  if (end_ns < start_ns) {
    return fail<RecordInfo>(ErrorCode::invalid_argument,
                            "record end time precedes start time");
  }
  const auto position = ::lseek(impl_->output, 0, SEEK_CUR);
  if (position < 0) {
    return fail<RecordInfo>(ErrorCode::archive_io,
                            "cannot determine archive write offset");
  }
  std::array<std::byte, coda_record_envelope_size> envelope{};
  std::copy(record_magic.begin(), record_magic.end(), envelope.begin());
  put_le<std::uint16_t>(envelope, 4, static_cast<std::uint16_t>(type));
  put_le<std::uint16_t>(envelope, 6, flags);
  put_le<std::uint32_t>(envelope, 8,
                        static_cast<std::uint32_t>(envelope.size()));
  put_le<std::uint64_t>(envelope, 12,
                        static_cast<std::uint64_t>(payload.size()));
  put_le<std::uint64_t>(envelope, 20, impl_->next_sequence);
  for (std::size_t index = 0; index < stream.bytes.size(); ++index) {
    envelope[28 + index] = static_cast<std::byte>(stream.bytes[index]);
  }
  put_le<std::int64_t>(envelope, 44, start_ns);
  put_le<std::int64_t>(envelope, 52, end_ns);
  for (std::size_t index = 0; index < impl_->previous_hash.size(); ++index) {
    envelope[60 + index] = static_cast<std::byte>(impl_->previous_hash[index]);
  }
  put_le<std::uint32_t>(envelope, 92,
                        detail::crc32c(std::span{envelope}.first(92)));
  const auto record_hash = sha256(join_for_hash(envelope, payload));
  std::array<std::byte, coda_commit_trailer_size> trailer{};
  std::copy(commit_magic.begin(), commit_magic.end(), trailer.begin());
  for (std::size_t index = 0; index < record_hash.size(); ++index) {
    trailer[4 + index] = static_cast<std::byte>(record_hash[index]);
  }
  put_le<std::uint32_t>(trailer, 36,
                        detail::crc32c(std::span{trailer}.first(36)));

  if (!write_all_fd(impl_->output, envelope) ||
      !write_all_fd(impl_->output, payload) ||
      !write_all_fd(impl_->output, trailer) ||
      ::fsync(impl_->output) != 0) {
    return fail<RecordInfo>(ErrorCode::archive_io,
                            "failed committing archive record");
  }
  RecordInfo info;
  info.type = type;
  info.sequence = impl_->next_sequence;
  info.stream = stream;
  info.start_ns = start_ns;
  info.end_ns = end_ns;
  info.payload_size = payload.size();
  info.file_offset = static_cast<std::uint64_t>(position);
  info.hash = record_hash;
  impl_->records.push_back(info);
  impl_->previous_hash = record_hash;
  impl_->next_sequence += 1;
  return info;
}

Result<void> CodaWriter::finalize() {
  if (!impl_) {
    return fail(ErrorCode::invalid_argument, "writer is not initialized");
  }
  if (impl_->is_finalized) return {};
  const auto index = make_index_payload(impl_->records);
  StreamId archive_stream{};
  auto appended = append(RecordType::final_index, archive_stream, 0, 0, index);
  if (!appended) return appended.error();
  impl_->is_finalized = true;
  const auto sync_status = ::fsync(impl_->output);
  const auto close_status = ::close(impl_->output);
  impl_->output = -1;
  if (sync_status != 0 || close_status != 0) {
    return fail(ErrorCode::archive_io, "failed closing finalized archive");
  }
  return {};
}

bool CodaWriter::finalized() const noexcept {
  return impl_ && impl_->is_finalized;
}

Result<CodaArchive> CodaArchive::open(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return fail<CodaArchive>(ErrorCode::archive_io,
                             "cannot open archive: " + path.string());
  }
  std::array<std::byte, coda_header_size> header{};
  input.read(reinterpret_cast<char*>(header.data()),
             static_cast<std::streamsize>(header.size()));
  if (input.gcount() != static_cast<std::streamsize>(header.size())) {
    return fail<CodaArchive>(ErrorCode::archive_corrupt,
                             "truncated CODA header");
  }
  auto status = validate_header(header);
  if (!status) return status.error();
  return CodaArchive{path};
}

VerificationReport CodaArchive::verify() const {
  auto scanned = scan_archive(path_);
  if (!scanned) {
    VerificationReport report;
    report.error_code = scanned.error().code;
    report.message = scanned.error().message;
    return report;
  }
  return scanned->report;
}

Result<std::vector<RecordInfo>> CodaArchive::records(
    ArchiveReadPolicy policy) const {
  auto scanned = scan_archive(path_);
  if (!scanned) return scanned.error();
  if (!scanned->report.ok &&
      scanned->report.valid_prefix_bytes < coda_header_size) {
    return fail<std::vector<RecordInfo>>(scanned->report.error_code,
                                        scanned->report.message);
  }
  if (!scanned->report.ok && policy == ArchiveReadPolicy::complete_archive) {
    return fail<std::vector<RecordInfo>>(scanned->report.error_code,
                                        scanned->report.message);
  }
  return std::move(scanned->records);
}

Result<std::vector<FeedInfo>> CodaArchive::feeds(
    ArchiveReadPolicy policy) const {
  auto record_list = records(policy);
  if (!record_list) return record_list.error();
  std::vector<FeedInfo> output;
  for (const auto& record : *record_list) {
    if (record.type != RecordType::feed_descriptor) continue;
    auto payload = read_payload(record);
    if (!payload) return payload.error();
    auto feed = detail::decode_feed_descriptor(*payload, record.stream);
    if (!feed) return feed.error();
    output.push_back(std::move(*feed));
  }
  return output;
}

Result<std::vector<std::byte>> CodaArchive::read_payload(
    const RecordInfo& record) const {
  std::ifstream input(path_, std::ios::binary);
  if (!input) {
    return fail<std::vector<std::byte>>(ErrorCode::archive_io,
                                        "cannot open archive payload");
  }
  input.seekg(static_cast<std::streamoff>(record.file_offset));
  std::array<std::byte, coda_record_envelope_size> envelope{};
  input.read(reinterpret_cast<char*>(envelope.data()),
             static_cast<std::streamsize>(envelope.size()));
  if (!input ||
      !bytes_equal(std::span{envelope}.first(record_magic.size()),
                   record_magic) ||
      get_le<std::uint32_t>(envelope, 8) != coda_record_envelope_size ||
      detail::crc32c(std::span{envelope}.first(92)) !=
          get_le<std::uint32_t>(envelope, 92) ||
      get_le<std::uint64_t>(envelope, 12) != record.payload_size ||
      get_le<std::uint64_t>(envelope, 20) != record.sequence ||
      get_le<std::uint16_t>(envelope, 4) !=
          static_cast<std::uint16_t>(record.type)) {
    return fail<std::vector<std::byte>>(ErrorCode::archive_corrupt,
                                        "record changed after verification");
  }
  for (std::size_t index = 0; index < record.stream.bytes.size(); ++index) {
    if (static_cast<std::uint8_t>(envelope[28 + index]) !=
        record.stream.bytes[index]) {
      return fail<std::vector<std::byte>>(ErrorCode::archive_corrupt,
                                          "record stream changed after verification");
    }
  }
  if (record.payload_size > std::numeric_limits<std::size_t>::max()) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                        "record payload exceeds address space");
  }
  std::vector<std::byte> payload(static_cast<std::size_t>(record.payload_size));
  if (!payload.empty()) {
    input.read(reinterpret_cast<char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
  }
  std::array<std::byte, coda_commit_trailer_size> trailer{};
  input.read(reinterpret_cast<char*>(trailer.data()),
             static_cast<std::streamsize>(trailer.size()));
  if (!input ||
      !bytes_equal(std::span{trailer}.first(commit_magic.size()),
                   commit_magic) ||
      detail::crc32c(std::span{trailer}.first(36)) !=
          get_le<std::uint32_t>(trailer, 36)) {
    return fail<std::vector<std::byte>>(ErrorCode::archive_corrupt,
                                        "record trailer changed after verification");
  }
  const auto actual_hash = sha256(join_for_hash(envelope, payload));
  if (actual_hash != record.hash ||
      !std::equal(actual_hash.begin(), actual_hash.end(), trailer.begin() + 4,
                  [](std::uint8_t left, std::byte right) {
                    return left == static_cast<std::uint8_t>(right);
                  })) {
    return fail<std::vector<std::byte>>(ErrorCode::archive_corrupt,
                                        "record hash changed after verification");
  }
  return payload;
}

Result<std::vector<std::byte>> CodaArchive::extract_stream(
    const StreamId& stream, RecordType type, ArchiveReadPolicy policy) const {
  auto record_list = records(policy);
  if (!record_list) return record_list.error();
  std::vector<std::byte> output;
  for (const auto& record : *record_list) {
    if (record.stream != stream || record.type != type) continue;
    auto payload = read_payload(record);
    if (!payload) return payload.error();
    if (output.size() > std::numeric_limits<std::size_t>::max() -
                            payload->size()) {
      return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                          "extracted stream is too large");
    }
    output.insert(output.end(), payload->begin(), payload->end());
  }
  return output;
}

Result<std::vector<std::byte>> CodaArchive::extract_feed(
    std::string_view label, ArchiveReadPolicy policy) const {
  auto record_list = records(policy);
  if (!record_list) return record_list.error();
  std::optional<StreamId> selected_stream;
  for (const auto& record : *record_list) {
    if (record.type != RecordType::feed_descriptor) continue;
    auto payload = read_payload(record);
    if (!payload) return payload.error();
    auto feed = detail::decode_feed_descriptor(*payload, record.stream);
    if (!feed) return feed.error();
    if (feed->label != label) continue;
    if (selected_stream && *selected_stream != record.stream) {
      return fail<std::vector<std::byte>>(ErrorCode::archive_corrupt,
                                          "duplicate feed label maps to multiple streams");
    }
    selected_stream = record.stream;
  }
  if (!selected_stream) {
    return fail<std::vector<std::byte>>(ErrorCode::invalid_argument,
                                        "feed label not found: " +
                                            std::string{label});
  }
  std::vector<std::byte> output;
  for (const auto& record : *record_list) {
    if (record.stream != *selected_stream ||
        record.type != RecordType::source_bytes) {
      continue;
    }
    auto payload = read_payload(record);
    if (!payload) return payload.error();
    if (output.size() > std::numeric_limits<std::size_t>::max() -
                            payload->size()) {
      return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                          "extracted feed is too large");
    }
    output.insert(output.end(), payload->begin(), payload->end());
  }
  return output;
}

Result<RepairReport> CodaArchive::repair(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
  if (source == destination) {
    return fail<RepairReport>(ErrorCode::invalid_argument,
                              "repair destination must differ from source");
  }
  auto scanned = scan_archive(source);
  if (!scanned) return scanned.error();
  if (scanned->report.valid_prefix_bytes < coda_header_size) {
    return fail<RepairReport>(ErrorCode::archive_corrupt,
                              "source does not contain a valid CODA header");
  }
  auto source_archive = CodaArchive::open(source);
  if (!source_archive) return source_archive.error();
  auto writer_result = CodaWriter::create(destination);
  if (!writer_result) return writer_result.error();
  auto writer = std::move(*writer_result);
  RepairReport report;
  for (const auto& record : scanned->records) {
    if (record.type == RecordType::final_index) continue;
    auto payload = source_archive->read_payload(record);
    if (!payload) return payload.error();
    auto appended = writer.append(record.type, record.stream, record.start_ns,
                                  record.end_ns, *payload);
    if (!appended) return appended.error();
    report.recovered_records += 1;
    report.recovered_payload_bytes += payload->size();
  }
  auto finalized = writer.finalize();
  if (!finalized) return finalized.error();
  report.discarded_tail_bytes =
      scanned->report.file_bytes - scanned->report.valid_prefix_bytes;
  return report;
}

StreamId derive_stream_id(std::string_view value) {
  std::vector<std::byte> input(value.size());
  if (!value.empty()) {
    std::memcpy(input.data(), value.data(), value.size());
  }
  const auto digest = sha256(input);
  StreamId id;
  std::copy_n(digest.begin(), id.bytes.size(), id.bytes.begin());
  id.bytes[6] = static_cast<std::uint8_t>((id.bytes[6] & 0x0fU) | 0x50U);
  id.bytes[8] = static_cast<std::uint8_t>((id.bytes[8] & 0x3fU) | 0x80U);
  return id;
}

std::string to_string(const StreamId& value) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string output;
  output.reserve(36);
  for (std::size_t index = 0; index < value.bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      output.push_back('-');
    }
    output.push_back(hex[value.bytes[index] >> 4U]);
    output.push_back(hex[value.bytes[index] & 0x0fU]);
  }
  return output;
}

const char* record_type_name(RecordType type) noexcept {
  switch (type) {
    case RecordType::feed_descriptor: return "feed_descriptor";
    case RecordType::source_bytes: return "source_bytes";
    case RecordType::pcm16: return "pcm16";
    case RecordType::gap: return "gap";
    case RecordType::watermark_statement: return "watermark_statement";
    case RecordType::watermark_observation: return "watermark_observation";
    case RecordType::feed_identity_event: return "feed_identity_event";
    case RecordType::audit_event: return "audit_event";
    case RecordType::final_index: return "final_index";
  }
  return "unknown";
}

}  // namespace codec

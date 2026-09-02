#include <codec/archive.hpp>

#include "../core/internal.hpp"
#include "verified_snapshot_scope.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
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

#ifdef CODEC_TESTING
std::atomic<std::uint64_t> archive_scan_count{0U};
#endif

thread_local const CodaArchive* scoped_snapshot_archive = nullptr;
thread_local const VerifiedArchiveSnapshot* scoped_snapshot = nullptr;

const VerifiedArchiveSnapshot* scoped_snapshot_for(
    const CodaArchive& archive) noexcept {
  return scoped_snapshot_archive == &archive ? scoped_snapshot : nullptr;
}

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

Result<void> validate_record_query(const RecordQuery& query) {
  if (query.sequence && query.sequence->begin >= query.sequence->end) {
    return fail(ErrorCode::invalid_argument,
                "record query sequence range must be non-empty");
  }
  if (query.time && query.time->begin_ns >= query.time->end_ns) {
    return fail(ErrorCode::invalid_argument,
                "record query time range must be non-empty");
  }
  return {};
}

bool matches_record_query(const RecordInfo& record, const RecordQuery& query) {
  if (query.stream && record.stream != *query.stream) return false;
  if (query.type && record.type_code() != *query.type) return false;
  if (query.sequence &&
      (record.sequence < query.sequence->begin ||
       record.sequence >= query.sequence->end)) {
    return false;
  }
  if (query.time) {
    const auto point = record.start_ns == record.end_ns;
    const auto matches =
        point ? query.time->begin_ns <= record.start_ns &&
                    record.start_ns < query.time->end_ns
              : record.start_ns < query.time->end_ns &&
                    query.time->begin_ns < record.end_ns;
    if (!matches) return false;
  }
  return true;
}

Result<std::vector<StreamProvenance>> decode_archive_provenance(
    const CodaArchive& archive, const std::vector<RecordInfo>& record_list) {
  std::vector<StreamProvenance> output;
  std::set<std::uint64_t> provenance_subjects;
  const auto prohibited = [](RecordTypeCode type) {
    return type == record_type_code(RecordType::stream_provenance) ||
           type == record_type_code(RecordType::final_index);
  };
  const auto exact_record = [&record_list](const ProvenanceRecordLink& link) {
    return std::find_if(
        record_list.begin(), record_list.end(),
        [&link](const RecordInfo& candidate) {
          return candidate.sequence == link.sequence &&
                 candidate.stream == link.stream &&
                 candidate.type_code() == link.type &&
                 candidate.hash == link.hash;
        });
  };
  for (const auto& record : record_list) {
    if (record.type != RecordType::stream_provenance) continue;
    auto payload = archive.read_payload(record);
    if (!payload) return payload.error();
    auto decoded = detail::decode_stream_provenance(*payload);
    if (!decoded) return decoded.error();
    const auto subject = exact_record(decoded->subject);
    if (subject == record_list.end() || subject->sequence >= record.sequence ||
        subject->stream != record.stream || prohibited(subject->type_code())) {
      return fail<std::vector<StreamProvenance>>(
          ErrorCode::archive_corrupt,
          "provenance subject link is invalid");
    }
    if (!provenance_subjects.insert(subject->sequence).second) {
      return fail<std::vector<StreamProvenance>>(
          ErrorCode::archive_corrupt,
          "provenance subject has duplicate sidecars");
    }
    std::set<std::uint64_t> input_sequences;
    for (const auto& input_link : decoded->inputs) {
      const auto input = exact_record(input_link);
      if (input == record_list.end() ||
          input->sequence >= subject->sequence ||
          prohibited(input->type_code())) {
        return fail<std::vector<StreamProvenance>>(
            ErrorCode::archive_corrupt,
            "provenance input link is invalid");
      }
      if (!input_sequences.insert(input->sequence).second) {
        return fail<std::vector<StreamProvenance>>(
            ErrorCode::archive_corrupt,
            "provenance input link is duplicated");
      }
    }
    output.push_back(std::move(*decoded));
  }
  return output;
}

Result<std::vector<StreamDescriptor>> decode_archive_streams(
    const CodaArchive& archive, const std::vector<RecordInfo>& record_list) {
  std::vector<StreamDescriptor> output;
  for (const auto& record : record_list) {
    if (record.type == RecordType::stream_descriptor) {
      auto payload = archive.read_payload(record);
      if (!payload) return payload.error();
      auto descriptor =
          detail::decode_stream_descriptor(*payload, record.stream);
      if (!descriptor) return descriptor.error();
      output.push_back(std::move(*descriptor));
      continue;
    }
    if (record.type == RecordType::feed_descriptor) {
      auto payload = archive.read_payload(record);
      if (!payload) return payload.error();
      auto feed = detail::decode_feed_descriptor(*payload, record.stream);
      if (!feed) return feed.error();
      output.push_back(StreamDescriptor{
          .id = feed->stream,
          .type = StreamType::opaque,
          .label = std::move(feed->label),
          .source_id = std::move(feed->uri),
          .payload_type = {},
      });
    }
  }
  return output;
}

Result<std::vector<StreamProvenance>> filter_archive_provenance(
    const std::vector<RecordInfo>& record_list,
    const std::vector<StreamProvenance>& semantic_records,
    const ProvenanceQuery& query) {
  if (query.subject_truth) {
    switch (*query.subject_truth) {
      case TruthClass::source_exact:
      case TruthClass::state_exact:
      case TruthClass::derived:
        break;
      default:
        return fail<std::vector<StreamProvenance>>(
            ErrorCode::invalid_argument,
            "provenance query truth class is invalid");
    }
  }
  if (query.subject) {
    auto valid = validate_record_query(*query.subject);
    if (!valid) return valid.error();
  }
  if (query.direct_input) {
    auto valid = validate_record_query(*query.direct_input);
    if (!valid) return valid.error();
  }
  const auto exact_record = [&record_list](const ProvenanceRecordLink& link) {
    return std::find_if(
        record_list.begin(), record_list.end(),
        [&link](const RecordInfo& candidate) {
          return candidate.sequence == link.sequence &&
                 candidate.stream == link.stream &&
                 candidate.type_code() == link.type &&
                 candidate.hash == link.hash;
        });
  };
  std::vector<StreamProvenance> output;
  for (const auto& provenance_record : semantic_records) {
    if (query.subject_truth &&
        provenance_record.subject_truth != *query.subject_truth) {
      continue;
    }
    const auto subject = exact_record(provenance_record.subject);
    if (subject == record_list.end()) {
      return fail<std::vector<StreamProvenance>>(
          ErrorCode::archive_corrupt,
          "provenance subject changed during query");
    }
    if (query.subject && !matches_record_query(*subject, *query.subject)) {
      continue;
    }
    if (query.direct_input) {
      const auto matches_input = std::any_of(
          provenance_record.inputs.begin(), provenance_record.inputs.end(),
          [&exact_record, &query, &record_list](
              const ProvenanceRecordLink& link) {
            const auto input = exact_record(link);
            return input != record_list.end() &&
                   matches_record_query(*input, *query.direct_input);
          });
      if (!matches_input) continue;
    }
    output.push_back(provenance_record);
  }
  return output;
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
#ifdef CODEC_TESTING
  archive_scan_count.fetch_add(1U, std::memory_order_relaxed);
#endif
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

#ifdef CODEC_TESTING
namespace detail {

void reset_archive_scan_count_for_tests() noexcept {
  archive_scan_count.store(0U, std::memory_order_relaxed);
}

std::uint64_t archive_scan_count_for_tests() noexcept {
  return archive_scan_count.load(std::memory_order_relaxed);
}

}  // namespace detail
#endif

struct VerifiedArchiveSnapshot::Impl {
  std::filesystem::path path;
  VerificationReport verification;
  std::vector<RecordInfo> records;
  std::vector<StreamDescriptor> streams;
  std::vector<StreamProvenance> provenance;
};

const VerificationReport& VerifiedArchiveSnapshot::verification() const noexcept {
  return impl_->verification;
}

const std::vector<RecordInfo>& VerifiedArchiveSnapshot::records() const noexcept {
  return impl_->records;
}

const std::vector<StreamDescriptor>& VerifiedArchiveSnapshot::streams() const noexcept {
  return impl_->streams;
}

const std::vector<StreamProvenance>& VerifiedArchiveSnapshot::provenance() const noexcept {
  return impl_->provenance;
}

Result<std::vector<RecordInfo>> VerifiedArchiveSnapshot::query_records(
    const RecordQuery& query) const {
  auto valid = validate_record_query(query);
  if (!valid) return valid.error();
  std::vector<RecordInfo> output;
  output.reserve(impl_->records.size());
  for (const auto& record : impl_->records) {
    if (matches_record_query(record, query)) output.push_back(record);
  }
  return output;
}

Result<std::vector<StreamProvenance>>
VerifiedArchiveSnapshot::query_provenance(const ProvenanceQuery& query) const {
  return filter_archive_provenance(impl_->records, impl_->provenance, query);
}

const std::filesystem::path& VerifiedArchiveSnapshot::path() const noexcept {
  return impl_->path;
}

namespace detail {

VerifiedArchiveSnapshotScope::VerifiedArchiveSnapshotScope(
    VerifiedArchiveSnapshotScope&& other) noexcept
    : previous_archive_(other.previous_archive_),
      previous_snapshot_(other.previous_snapshot_),
      active_(other.active_) {
  other.active_ = false;
}

VerifiedArchiveSnapshotScope::~VerifiedArchiveSnapshotScope() {
  if (!active_) return;
  scoped_snapshot_archive = previous_archive_;
  scoped_snapshot = previous_snapshot_;
}

Result<VerifiedArchiveSnapshotScope> activate_verified_archive_snapshot(
    const CodaArchive& archive, const VerifiedArchiveSnapshot& snapshot) {
  if (snapshot.path() != archive.path()) {
    return fail<VerifiedArchiveSnapshotScope>(
        ErrorCode::invalid_argument,
        "verified archive snapshot does not belong to this archive");
  }
  VerifiedArchiveSnapshotScope scope{scoped_snapshot_archive, scoped_snapshot};
  scoped_snapshot_archive = &archive;
  scoped_snapshot = &snapshot;
  return scope;
}

}  // namespace detail

struct CodaWriter::Impl {
  std::filesystem::path path;
  int output{-1};
  std::uint64_t next_sequence{};
  Sha256 previous_hash{};
  std::vector<RecordInfo> records;
  std::map<StreamId, detail::StreamContinuityState> continuity_states;
  std::set<std::uint64_t> provenance_subjects;
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
  return append_raw(record_type_code(type), stream, start_ns, end_ns, payload,
                    flags);
}

Result<RecordInfo> CodaWriter::append_raw(
    RecordTypeCode type, const StreamId& stream, std::int64_t start_ns,
    std::int64_t end_ns, std::span<const std::byte> payload,
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
  put_le<RecordTypeCode>(envelope, 4, type);
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
  info.type = static_cast<RecordType>(type);
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

Result<RecordInfo> CodaWriter::append_stream_descriptor(
    const StreamDescriptor& descriptor, std::int64_t timestamp_ns) {
  auto payload = detail::encode_stream_descriptor(descriptor);
  if (!payload) return payload.error();
  return append(RecordType::stream_descriptor, descriptor.id, timestamp_ns,
                timestamp_ns, *payload);
}

Result<RecordInfo> CodaWriter::append_stream_timing(
    const RecordInfo& source, std::uint64_t stream_sequence,
    const StreamClock& clock, const StreamEpoch& epoch) {
  if (!impl_ || impl_->is_finalized) {
    return fail<RecordInfo>(ErrorCode::invalid_argument,
                            "cannot append to a finalized archive");
  }
  const auto committed = std::find_if(
      impl_->records.begin(), impl_->records.end(),
      [&source](const RecordInfo& candidate) {
        return candidate.sequence == source.sequence;
      });
  if (committed == impl_->records.end() ||
      committed->type != RecordType::source_bytes ||
      source.type != RecordType::source_bytes ||
      committed->stream != source.stream || committed->hash != source.hash) {
    return fail<RecordInfo>(
        ErrorCode::invalid_argument,
        "stream timing source must match an exact committed source record");
  }
  const auto committed_stream = committed->stream;
  const StreamTiming timing{
      .stream = committed_stream,
      .sequence = stream_sequence,
      .clock = clock,
      .epoch = epoch,
      .source_record_sequence = committed->sequence,
      .source_record_hash = committed->hash,
  };
  detail::StreamContinuityState next_state;
  const auto state = impl_->continuity_states.find(committed_stream);
  if (state != impl_->continuity_states.end()) next_state = state->second;
  auto valid = detail::validate_and_advance(next_state, timing);
  if (!valid) return valid.error();
  const auto payload = detail::encode_stream_timing(timing);
  auto appended = append(RecordType::stream_timing, committed_stream,
                         clock.monotonic_ns, clock.monotonic_ns, payload);
  if (!appended) return appended.error();
  impl_->continuity_states[committed_stream] = next_state;
  return appended;
}

Result<RecordInfo> CodaWriter::append_stream_gap(
    const StreamId& stream, std::uint64_t first_sequence,
    std::uint64_t missing_count, const StreamEpoch& epoch) {
  const StreamGap gap{
      .stream = stream,
      .first_sequence = first_sequence,
      .missing_count = missing_count,
      .epoch = epoch,
  };
  if (!impl_ || impl_->is_finalized) {
    return fail<RecordInfo>(ErrorCode::invalid_argument,
                            "cannot append to a finalized archive");
  }
  detail::StreamContinuityState next_state;
  const auto state = impl_->continuity_states.find(stream);
  if (state != impl_->continuity_states.end()) next_state = state->second;
  auto valid = detail::validate_and_advance(next_state, gap);
  if (!valid) return valid.error();
  const auto payload = detail::encode_stream_gap(gap);
  auto appended = append(RecordType::gap, stream, 0, 0, payload);
  if (!appended) return appended.error();
  impl_->continuity_states[stream] = next_state;
  return appended;
}

Result<RecordInfo> CodaWriter::append_stream_provenance(
    const RecordInfo& subject, TruthClass subject_truth,
    std::span<const RecordInfo> inputs, const ProvenanceProcess& process) {
  if (!impl_ || impl_->is_finalized) {
    return fail<RecordInfo>(ErrorCode::invalid_argument,
                            "cannot append to a finalized archive");
  }
  const auto exact_record = [this](const RecordInfo& requested) {
    return std::find_if(
        impl_->records.begin(), impl_->records.end(),
        [&requested](const RecordInfo& candidate) {
          return candidate.sequence == requested.sequence &&
                 candidate.stream == requested.stream &&
                 candidate.type_code() == requested.type_code() &&
                 candidate.hash == requested.hash;
        });
  };
  const auto committed_subject = exact_record(subject);
  if (committed_subject == impl_->records.end()) {
    return fail<RecordInfo>(
        ErrorCode::invalid_argument,
        "provenance subject must match an exact committed record");
  }
  const auto committed_subject_record = *committed_subject;
  const auto prohibited = [](RecordTypeCode type) {
    return type == record_type_code(RecordType::stream_provenance) ||
           type == record_type_code(RecordType::final_index);
  };
  if (prohibited(committed_subject_record.type_code())) {
    return fail<RecordInfo>(ErrorCode::invalid_argument,
                            "provenance subject cannot be metadata");
  }
  if (impl_->provenance_subjects.contains(
          committed_subject_record.sequence)) {
    return fail<RecordInfo>(ErrorCode::invalid_argument,
                            "provenance subject already has a sidecar");
  }
  std::set<std::uint64_t> input_sequences;
  StreamProvenance provenance{
      .subject_truth = subject_truth,
      .subject = ProvenanceRecordLink{
          .stream = committed_subject_record.stream,
          .type = committed_subject_record.type_code(),
          .sequence = committed_subject_record.sequence,
          .hash = committed_subject_record.hash,
      },
      .inputs = {},
      .process = process,
  };
  provenance.inputs.reserve(inputs.size());
  for (const auto& input : inputs) {
    const auto committed_input = exact_record(input);
    if (committed_input == impl_->records.end()) {
      return fail<RecordInfo>(
          ErrorCode::invalid_argument,
          "provenance input must match an exact committed record");
    }
    if (committed_input->sequence >= committed_subject_record.sequence) {
      return fail<RecordInfo>(ErrorCode::invalid_argument,
                              "provenance input must precede its subject");
    }
    if (prohibited(committed_input->type_code())) {
      return fail<RecordInfo>(ErrorCode::invalid_argument,
                              "provenance input cannot be metadata");
    }
    if (!input_sequences.insert(committed_input->sequence).second) {
      return fail<RecordInfo>(ErrorCode::invalid_argument,
                              "provenance input is duplicated");
    }
    provenance.inputs.push_back(ProvenanceRecordLink{
        .stream = committed_input->stream,
        .type = committed_input->type_code(),
        .sequence = committed_input->sequence,
        .hash = committed_input->hash,
    });
  }
  auto payload = detail::encode_stream_provenance(provenance);
  if (!payload) return payload.error();
  const auto subject_sequence = committed_subject_record.sequence;
  auto appended = append(RecordType::stream_provenance,
                         committed_subject_record.stream,
                         committed_subject_record.start_ns,
                         committed_subject_record.end_ns, *payload);
  if (!appended) return appended.error();
  impl_->provenance_subjects.insert(subject_sequence);
  return appended;
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
  if (const auto* snapshot = scoped_snapshot_for(*this)) {
    return snapshot->verification();
  }
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
  if (const auto* snapshot = scoped_snapshot_for(*this)) {
    return snapshot->records();
  }
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

Result<std::vector<RecordInfo>> CodaArchive::query_records(
    const RecordQuery& query, ArchiveReadPolicy policy) const {
  auto valid = validate_record_query(query);
  if (!valid) return valid.error();
  auto record_list = records(policy);
  if (!record_list) return record_list.error();
  std::vector<RecordInfo> output;
  output.reserve(record_list->size());
  for (const auto& record : *record_list) {
    if (matches_record_query(record, query)) output.push_back(record);
  }
  return output;
}

Result<std::vector<ExtractedRecord>> CodaArchive::extract_records(
    const RecordQuery& query, ArchiveReadPolicy policy) const {
  auto selected = query_records(query, policy);
  if (!selected) return selected.error();
  std::vector<ExtractedRecord> output;
  output.reserve(selected->size());
  for (const auto& record : *selected) {
    auto payload = read_payload(record);
    if (!payload) return payload.error();
    output.push_back(ExtractedRecord{record, std::move(*payload)});
  }
  return output;
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

Result<std::vector<StreamDescriptor>> CodaArchive::streams(
    ArchiveReadPolicy policy) const {
  if (const auto* snapshot = scoped_snapshot_for(*this)) {
    return snapshot->streams();
  }
  auto record_list = records(policy);
  if (!record_list) return record_list.error();
  return decode_archive_streams(*this, *record_list);
}

Result<std::vector<StreamContinuityEvent>> CodaArchive::continuity(
    ArchiveReadPolicy policy) const {
  auto record_list = records(policy);
  if (!record_list) return record_list.error();
  std::vector<StreamContinuityEvent> output;
  std::map<StreamId, detail::StreamContinuityState> states;
  for (const auto& record : *record_list) {
    if (record.type == RecordType::stream_timing) {
      auto payload = read_payload(record);
      if (!payload) return payload.error();
      auto timing = detail::decode_stream_timing(*payload, record.stream);
      if (!timing) return timing.error();
      const auto source = std::find_if(
          record_list->begin(), record_list->end(),
          [&timing](const RecordInfo& candidate) {
            return candidate.sequence == timing->source_record_sequence;
          });
      if (source == record_list->end() || source->sequence >= record.sequence ||
          source->type != RecordType::source_bytes ||
          source->stream != timing->stream ||
          source->hash != timing->source_record_hash) {
        return fail<std::vector<StreamContinuityEvent>>(
            ErrorCode::archive_corrupt,
            "stream timing source link is invalid");
      }
      auto valid = detail::validate_and_advance(states[timing->stream],
                                                *timing);
      if (!valid) {
        return fail<std::vector<StreamContinuityEvent>>(
            ErrorCode::archive_corrupt,
            "invalid stream timing continuity: " + valid.error().message);
      }
      output.push_back(StreamContinuityEvent{
          .kind = StreamContinuityKind::timing,
          .timing = std::move(*timing),
      });
      continue;
    }
    if (record.type == RecordType::gap) {
      auto payload = read_payload(record);
      if (!payload) return payload.error();
      auto gap = detail::decode_stream_gap(*payload, record.stream);
      if (!gap) return gap.error();
      auto valid = detail::validate_and_advance(states[gap->stream], *gap);
      if (!valid) {
        return fail<std::vector<StreamContinuityEvent>>(
            ErrorCode::archive_corrupt,
            "invalid stream gap continuity: " + valid.error().message);
      }
      output.push_back(StreamContinuityEvent{
          .kind = StreamContinuityKind::gap,
          .gap = std::move(*gap),
      });
    }
  }
  return output;
}

Result<std::vector<StreamProvenance>> CodaArchive::provenance(
    ArchiveReadPolicy policy) const {
  if (const auto* snapshot = scoped_snapshot_for(*this)) {
    return snapshot->provenance();
  }
  auto record_list = records(policy);
  if (!record_list) return record_list.error();
  return decode_archive_provenance(*this, *record_list);
}

Result<std::vector<StreamProvenance>> CodaArchive::query_provenance(
    const ProvenanceQuery& query, ArchiveReadPolicy policy) const {
  const std::vector<RecordInfo> no_records;
  const std::vector<StreamProvenance> no_provenance;
  auto valid = filter_archive_provenance(no_records, no_provenance, query);
  if (!valid) return valid.error();
  if (const auto* snapshot = scoped_snapshot_for(*this)) {
    return snapshot->query_provenance(query);
  }
  auto record_list = records(policy);
  if (!record_list) return record_list.error();
  auto semantic_records = decode_archive_provenance(*this, *record_list);
  if (!semantic_records) return semantic_records.error();
  return filter_archive_provenance(*record_list, *semantic_records, query);
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
  return extract_stream_raw(stream, record_type_code(type), policy);
}

Result<std::vector<std::byte>> CodaArchive::extract_stream_raw(
    const StreamId& stream, RecordTypeCode type, ArchiveReadPolicy policy) const {
  const RecordQuery query{
      .stream = stream,
      .type = type,
      .sequence = std::nullopt,
      .time = std::nullopt,
  };
  auto selected = query_records(query, policy);
  if (!selected) return selected.error();
  std::vector<std::byte> output;
  for (const auto& record : *selected) {
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

Result<VerifiedArchiveSnapshot> CodaArchive::verified_snapshot() const {
  auto scanned = scan_archive(path_);
  if (!scanned) return scanned.error();
  if (!scanned->report.ok) {
    return fail<VerifiedArchiveSnapshot>(scanned->report.error_code,
                                         scanned->report.message);
  }
  if (!scanned->report.finalized) {
    return fail<VerifiedArchiveSnapshot>(
        ErrorCode::archive_corrupt,
        "verified archive snapshot requires a finalized archive");
  }

  auto stream_list = decode_archive_streams(*this, scanned->records);
  if (!stream_list) return stream_list.error();
  auto provenance_list = decode_archive_provenance(*this, scanned->records);
  if (!provenance_list) return provenance_list.error();

  auto impl = std::make_shared<VerifiedArchiveSnapshot::Impl>();
  impl->path = path_;
  impl->verification = scanned->report;
  impl->records = std::move(scanned->records);
  impl->streams = std::move(*stream_list);
  impl->provenance = std::move(*provenance_list);
  return VerifiedArchiveSnapshot{std::move(impl)};
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
    auto appended = writer.append_raw(record.type_code(), record.stream,
                                      record.start_ns, record.end_ns, *payload);
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
    case RecordType::stream_descriptor: return "stream_descriptor";
    case RecordType::stream_timing: return "stream_timing";
    case RecordType::stream_provenance: return "stream_provenance";
    case RecordType::feed_identity_event: return "feed_identity_event";
    case RecordType::audit_event: return "audit_event";
    case RecordType::final_index: return "final_index";
  }
  return "unknown";
}

}  // namespace codec

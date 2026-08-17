#include "test.hpp"

#include <codec/archive.hpp>
#include <codec/integrity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

std::filesystem::path test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-test-" + std::string{name});
}

std::vector<std::byte> bytes(std::string_view value) {
  std::vector<std::byte> result;
  result.reserve(value.size());
  for (const unsigned char ch : value) {
    result.push_back(static_cast<std::byte>(ch));
  }
  return result;
}

codec::StreamId stream_id(std::uint8_t seed) {
  codec::StreamId value{};
  for (std::size_t index = 0; index < value.bytes.size(); ++index) {
    value.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

}  // namespace

TEST(sha256_matches_the_nist_abc_vector) {
  const auto input = bytes("abc");
  EXPECT_EQ(codec::sha256_hex(input),
            std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb"
                        "410ff61f20015ad"});
}

TEST(sha256_matches_the_nist_two_block_padding_vector) {
  const auto input = bytes(
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
  EXPECT_EQ(codec::sha256_hex(input),
            std::string{"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6e"
                        "cedd419db06c1"});
}

TEST(archive_extracts_committed_source_bytes_exactly) {
  const auto path = test_path("roundtrip.coda");
  std::filesystem::remove(path);
  auto writer_result = codec::CodaWriter::create(path);
  EXPECT_TRUE(writer_result);
  auto writer = std::move(*writer_result);
  const auto first = bytes("first accepted payload\n");
  const auto second = bytes("second accepted payload\0with binary");
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, stream_id(3), 10,
                            20, first));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, stream_id(3), 21,
                            30, second));
  EXPECT_TRUE(writer.finalize());

  auto archive_result = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive_result);
  auto report = archive_result->verify();
  EXPECT_TRUE(report.ok);
  EXPECT_EQ(report.committed_records, std::uint64_t{3});

  auto extracted = archive_result->extract_stream(stream_id(3));
  EXPECT_TRUE(extracted);
  auto expected = first;
  expected.insert(expected.end(), second.begin(), second.end());
  EXPECT_EQ(*extracted, expected);
  std::filesystem::remove(path);
}

TEST(archive_detects_payload_tampering) {
  const auto path = test_path("tampered.coda");
  std::filesystem::remove(path);
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, stream_id(8), 0,
                            1, bytes("unaltered")));
  EXPECT_TRUE(writer.finalize());

  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(static_cast<std::streamoff>(codec::coda_header_size +
                                         codec::coda_record_envelope_size + 2));
  const char changed = 'X';
  file.write(&changed, 1);
  file.close();

  auto archive = std::move(*codec::CodaArchive::open(path));
  const auto report = archive.verify();
  EXPECT_FALSE(report.ok);
  EXPECT_EQ(report.error_code, codec::ErrorCode::archive_corrupt);
  std::filesystem::remove(path);
}

TEST(repair_keeps_every_complete_record_and_discards_the_torn_tail) {
  const auto source = test_path("truncated.coda");
  const auto repaired = test_path("repaired.coda");
  std::filesystem::remove(source);
  std::filesystem::remove(repaired);
  auto writer = std::move(*codec::CodaWriter::create(source));
  const auto complete = bytes("complete");
  auto first_record = writer.append(codec::RecordType::source_bytes,
                                    stream_id(12), 0, 1, complete);
  EXPECT_TRUE(first_record);
  auto second_record = writer.append(codec::RecordType::source_bytes,
                                     stream_id(12), 2, 3,
                                     bytes("will be torn"));
  EXPECT_TRUE(second_record);
  EXPECT_TRUE(writer.finalize());

  std::filesystem::resize_file(
      source, second_record->file_offset + codec::coda_record_envelope_size + 4);
  auto repair = codec::CodaArchive::repair(source, repaired);
  EXPECT_TRUE(repair);
  EXPECT_EQ(repair->recovered_records, std::uint64_t{1});
  EXPECT_TRUE(repair->discarded_tail_bytes > 0);

  auto archive = std::move(*codec::CodaArchive::open(repaired));
  EXPECT_TRUE(archive.verify().ok);
  auto extracted = archive.extract_stream(stream_id(12));
  EXPECT_TRUE(extracted);
  EXPECT_EQ(*extracted, complete);
  std::filesystem::remove(source);
  std::filesystem::remove(repaired);
}

TEST(archive_creation_refuses_to_replace_an_existing_file) {
  const auto path = test_path("existing-output.coda");
  const auto sentinel = bytes("do not replace");
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(sentinel.data()),
                 static_cast<std::streamsize>(sentinel.size()));
  }
  auto writer = codec::CodaWriter::create(path);
  EXPECT_FALSE(writer);
  std::ifstream input(path, std::ios::binary);
  std::vector<std::byte> actual(sentinel.size());
  input.read(reinterpret_cast<char*>(actual.data()),
             static_cast<std::streamsize>(actual.size()));
  EXPECT_EQ(actual, sentinel);
  std::filesystem::remove(path);
}

TEST(repair_refuses_a_symlink_that_aliases_the_source) {
  const auto source = test_path("repair-alias-source.coda");
  const auto alias = test_path("repair-alias-output.coda");
  std::filesystem::remove(source);
  std::filesystem::remove(alias);
  auto writer = std::move(*codec::CodaWriter::create(source));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, stream_id(44), 0,
                            1, bytes("preserve me")));
  EXPECT_TRUE(writer.finalize());
  const auto size_before = std::filesystem::file_size(source);
  std::filesystem::create_symlink(source, alias);
  auto repaired = codec::CodaArchive::repair(source, alias);
  EXPECT_FALSE(repaired);
  EXPECT_EQ(std::filesystem::file_size(source), size_before);
  EXPECT_TRUE(codec::CodaArchive::open(source)->verify().ok);
  std::filesystem::remove(alias);
  std::filesystem::remove(source);
}

TEST(payload_read_rechecks_the_hash_after_the_archive_was_scanned) {
  const auto path = test_path("read-race.coda");
  std::filesystem::remove(path);
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, stream_id(55), 0,
                            1, bytes("trusted bytes")));
  EXPECT_TRUE(writer.finalize());
  auto archive = std::move(*codec::CodaArchive::open(path));
  auto records = archive.records();
  EXPECT_TRUE(records);
  const auto source_record = records->front();
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(static_cast<std::streamoff>(source_record.file_offset +
                                           codec::coda_record_envelope_size));
    file.write("X", 1);
  }
  auto payload = archive.read_payload(source_record);
  EXPECT_FALSE(payload);
  EXPECT_EQ(payload.error().code, codec::ErrorCode::archive_corrupt);
  std::filesystem::remove(path);
}

TEST(explicit_prefix_policy_reads_committed_records_before_a_torn_tail) {
  const auto path = test_path("prefix-read.coda");
  std::filesystem::remove(path);
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, stream_id(66), 0,
                            1, bytes("prefix")));
  auto second = writer.append(codec::RecordType::source_bytes, stream_id(66),
                              2, 3, bytes("torn"));
  EXPECT_TRUE(second);
  EXPECT_TRUE(writer.finalize());
  std::filesystem::resize_file(
      path, second->file_offset + codec::coda_record_envelope_size + 1);
  auto archive = std::move(*codec::CodaArchive::open(path));
  EXPECT_FALSE(archive.verify().ok);
  auto strict = archive.records();
  EXPECT_FALSE(strict);
  auto prefix = archive.records(codec::ArchiveReadPolicy::verified_prefix);
  EXPECT_TRUE(prefix);
  EXPECT_EQ(prefix->size(), std::size_t{1});
  auto extracted = archive.extract_stream(
      stream_id(66), codec::RecordType::source_bytes,
      codec::ArchiveReadPolicy::verified_prefix);
  EXPECT_TRUE(extracted);
  EXPECT_EQ(*extracted, bytes("prefix"));
  std::filesystem::remove(path);
}

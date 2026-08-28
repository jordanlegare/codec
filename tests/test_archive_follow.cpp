#include "test.hpp"

#include <codec/archive.hpp>
#include <codec/archive_follow.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path follow_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-follow-" + std::string{name});
}

std::vector<std::byte> follow_bytes(std::string_view value) {
  std::vector<std::byte> output;
  output.reserve(value.size());
  for (const unsigned char ch : value) {
    output.push_back(static_cast<std::byte>(ch));
  }
  return output;
}

codec::StreamId follow_stream(std::uint8_t seed) {
  codec::StreamId stream{};
  for (std::size_t index = 0; index < stream.bytes.size(); ++index) {
    stream.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return stream;
}

}  // namespace

TEST(archive_follow_reads_only_selected_verified_prefix_source_records) {
  const auto path = follow_path("selected-prefix.coda");
  std::filesystem::remove(path);
  const auto a = follow_stream(1);
  const auto b = follow_stream(40);
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, a, 1, 1,
                            follow_bytes("a0")));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, b, 2, 2,
                            follow_bytes("b0")));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, a, 3, 3,
                            follow_bytes("a1")));

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto batch = codec::extract_stream_source_exact_prefix(*archive, a);
  EXPECT_TRUE(batch);
  EXPECT_FALSE(batch->finalized);
  EXPECT_EQ(batch->records.size(), std::size_t{2});
  EXPECT_EQ(batch->records[0].record.sequence, std::uint64_t{0});
  EXPECT_EQ(batch->records[1].record.sequence, std::uint64_t{2});
  EXPECT_EQ(batch->cursor.next_archive_sequence, std::uint64_t{3});
  EXPECT_EQ(batch->records[0].payload, follow_bytes("a0"));
  EXPECT_EQ(batch->records[1].payload, follow_bytes("a1"));

  std::filesystem::remove(path);
}

TEST(archive_follow_cursor_skips_already_inspected_interleaved_records) {
  const auto path = follow_path("cursor.coda");
  std::filesystem::remove(path);
  const auto a = follow_stream(2);
  const auto b = follow_stream(60);
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, b, 1, 1,
                            follow_bytes("b0")));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, a, 2, 2,
                            follow_bytes("a0")));

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto first = codec::extract_stream_source_exact_prefix(*archive, a);
  EXPECT_TRUE(first);
  EXPECT_EQ(first->records.size(), std::size_t{1});
  EXPECT_EQ(first->cursor.next_archive_sequence, std::uint64_t{2});

  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, b, 3, 3,
                            follow_bytes("b1")));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, a, 4, 4,
                            follow_bytes("a1")));

  auto second = codec::extract_stream_source_exact_prefix(
      *archive, a, first->cursor);
  EXPECT_TRUE(second);
  EXPECT_EQ(second->records.size(), std::size_t{1});
  EXPECT_EQ(second->records[0].payload, follow_bytes("a1"));
  EXPECT_EQ(second->cursor.next_archive_sequence, std::uint64_t{4});

  std::filesystem::remove(path);
}

TEST(archive_follow_reports_finalization_after_last_selected_batch) {
  const auto path = follow_path("finalized.coda");
  std::filesystem::remove(path);
  const auto a = follow_stream(3);
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, a, 1, 1,
                            follow_bytes("last")));

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto before = codec::extract_stream_source_exact_prefix(*archive, a);
  EXPECT_TRUE(before);
  EXPECT_FALSE(before->finalized);
  EXPECT_EQ(before->records.size(), std::size_t{1});

  EXPECT_TRUE(writer.finalize());
  auto after = codec::extract_stream_source_exact_prefix(
      *archive, a, before->cursor);
  EXPECT_TRUE(after);
  EXPECT_TRUE(after->finalized);
  EXPECT_EQ(after->records.size(), std::size_t{0});
  EXPECT_TRUE(after->cursor.next_archive_sequence >
              before->cursor.next_archive_sequence);

  std::filesystem::remove(path);
}

TEST(archive_follow_rejects_zero_limits) {
  const auto path = follow_path("limits.coda");
  std::filesystem::remove(path);
  const auto a = follow_stream(4);
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, a, 1, 1,
                            follow_bytes("x")));
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);

  auto zero_records = codec::extract_stream_source_exact_prefix(
      *archive, a, {}, 0, 1024);
  EXPECT_FALSE(zero_records);
  EXPECT_EQ(zero_records.error().code, codec::ErrorCode::invalid_argument);

  auto zero_bytes = codec::extract_stream_source_exact_prefix(
      *archive, a, {}, 4, 0);
  EXPECT_FALSE(zero_bytes);
  EXPECT_EQ(zero_bytes.error().code, codec::ErrorCode::invalid_argument);

  std::filesystem::remove(path);
}

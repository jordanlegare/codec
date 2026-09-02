#include "test.hpp"

#include <codec/archive.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace codec::detail {
void reset_archive_scan_count_for_tests() noexcept;
std::uint64_t archive_scan_count_for_tests() noexcept;
}  // namespace codec::detail

namespace {

std::filesystem::path snapshot_test_path() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("codec-verified-snapshot-" + std::to_string(now) + ".coda");
}

}  // namespace

static_assert(sizeof(codec::CodaArchive) == sizeof(std::filesystem::path),
              "verified snapshots must not add cache state to CodaArchive");

TEST(archive_verified_snapshot_reuses_one_complete_scan) {
  const auto path = snapshot_test_path();
  std::error_code cleanup_error;
  std::filesystem::remove(path, cleanup_error);

  auto writer_result = codec::CodaWriter::create(path);
  EXPECT_TRUE(writer_result);
  auto writer = std::move(*writer_result);

  const auto stream = codec::derive_stream_id("snapshot-video");
  auto descriptor = writer.append_stream_descriptor(
      codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "snapshot-video",
          .source_id = "fixture",
          .payload_type = "video/test",
      },
      0);
  EXPECT_TRUE(descriptor);

  const std::vector<std::byte> source{
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
  auto source_record = writer.append(codec::RecordType::source_bytes, stream, 0,
                                     1'000'000'000, source);
  EXPECT_TRUE(source_record);
  auto finalized = writer.finalize();
  EXPECT_TRUE(finalized);

  auto archive_result = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive_result);
  auto archive = std::move(*archive_result);

  codec::detail::reset_archive_scan_count_for_tests();
  auto snapshot = archive.verified_snapshot();
  EXPECT_TRUE(snapshot);
  EXPECT_EQ(codec::detail::archive_scan_count_for_tests(), 1U);
  EXPECT_TRUE(snapshot->verification().ok);
  EXPECT_TRUE(snapshot->verification().finalized);
  EXPECT_EQ(snapshot->streams().size(), 1U);
  EXPECT_EQ(snapshot->streams().front().id, stream);

  auto selected = snapshot->query_records(codec::RecordQuery{
      .stream = stream,
      .type = codec::record_type_code(codec::RecordType::source_bytes),
      .sequence = std::nullopt,
      .time = std::nullopt,
  });
  EXPECT_TRUE(selected);
  EXPECT_EQ(selected->size(), 1U);

  auto selected_provenance =
      snapshot->query_provenance(codec::ProvenanceQuery{});
  EXPECT_TRUE(selected_provenance);
  EXPECT_TRUE(selected_provenance->empty());
  EXPECT_EQ(codec::detail::archive_scan_count_for_tests(), 1U);

  auto payload = archive.read_payload(selected->front());
  EXPECT_TRUE(payload);
  EXPECT_EQ(payload->size(), source.size());
  EXPECT_EQ(codec::detail::archive_scan_count_for_tests(), 1U);

  auto fresh_records = archive.records();
  EXPECT_TRUE(fresh_records);
  EXPECT_EQ(codec::detail::archive_scan_count_for_tests(), 2U);

  std::filesystem::remove(path, cleanup_error);
}

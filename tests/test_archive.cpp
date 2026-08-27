#include "test.hpp"

#include <codec/archive.hpp>
#include <codec/integrity.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
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

std::vector<std::byte> file_bytes(const std::filesystem::path& path) {
  const auto size = std::filesystem::file_size(path);
  std::vector<std::byte> output(static_cast<std::size_t>(size));
  std::ifstream input(path, std::ios::binary);
  input.read(reinterpret_cast<char*>(output.data()),
             static_cast<std::streamsize>(output.size()));
  return output;
}

codec::StreamId stream_id(std::uint8_t seed) {
  codec::StreamId value{};
  for (std::size_t index = 0; index < value.bytes.size(); ++index) {
    value.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
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

std::vector<std::byte> timing_payload(
    std::uint64_t sequence, const codec::StreamClock& clock,
    const codec::StreamEpoch& epoch, std::uint64_t source_record_sequence,
    const codec::Sha256& source_record_hash) {
  std::vector<std::byte> output(120);
  std::memcpy(output.data(), "STM1", 4);
  put_le<std::uint16_t>(output, 4, 1);
  put_le<std::uint16_t>(output, 6, 0);
  put_le<std::uint64_t>(output, 8, sequence);
  put_le<std::uint64_t>(output, 16, epoch.connection);
  put_le<std::uint64_t>(output, 24, epoch.format);
  put_le<std::int64_t>(output, 32, clock.monotonic_ns);
  put_le<std::int64_t>(output, 40, clock.observed_utc_ns);
  put_le<std::uint64_t>(output, 48, clock.observed_utc_uncertainty_ns);
  put_le<std::int64_t>(output, 56, clock.source_timestamp);
  put_le<std::int64_t>(output, 64, clock.source_timebase_numerator);
  put_le<std::int64_t>(output, 72, clock.source_timebase_denominator);
  put_le<std::uint64_t>(output, 80, source_record_sequence);
  for (std::size_t index = 0; index < source_record_hash.size(); ++index) {
    output[88 + index] = static_cast<std::byte>(source_record_hash[index]);
  }
  return output;
}

std::vector<std::byte> gap_payload(std::uint64_t first_sequence,
                                   std::uint64_t missing_count,
                                   const codec::StreamEpoch& epoch) {
  std::vector<std::byte> output(40);
  std::memcpy(output.data(), "SGP1", 4);
  put_le<std::uint16_t>(output, 4, 1);
  put_le<std::uint16_t>(output, 6, 0);
  put_le<std::uint64_t>(output, 8, first_sequence);
  put_le<std::uint64_t>(output, 16, missing_count);
  put_le<std::uint64_t>(output, 24, epoch.connection);
  put_le<std::uint64_t>(output, 32, epoch.format);
  return output;
}

template <typename AppendMetadata>
void expect_invalid_continuity_metadata(std::string_view name,
                                        const codec::StreamId& stream,
                                        AppendMetadata append_metadata) {
  const auto path = test_path(name);
  std::filesystem::remove(path);
  const auto source_payload = bytes("preserved despite invalid continuity");
  auto writer = std::move(*codec::CodaWriter::create(path));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                              source_payload);
  EXPECT_TRUE(source);
  append_metadata(writer, *source);
  EXPECT_TRUE(writer.finalize());

  auto archive = std::move(*codec::CodaArchive::open(path));
  EXPECT_TRUE(archive.verify().ok);
  auto continuity = archive.continuity();
  EXPECT_FALSE(continuity);
  EXPECT_EQ(continuity.error().code, codec::ErrorCode::archive_corrupt);
  auto extracted = archive.extract_stream(stream);
  EXPECT_TRUE(extracted);
  EXPECT_TRUE(extracted->size() >= source_payload.size());
  EXPECT_TRUE(std::equal(source_payload.begin(), source_payload.end(),
                         extracted->begin()));
  std::filesystem::remove(path);
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

TEST(raw_record_type_api_matches_typed_record_encoding) {
  const auto typed_path = test_path("typed-record.coda");
  const auto raw_path = test_path("raw-record.coda");
  std::filesystem::remove(typed_path);
  std::filesystem::remove(raw_path);
  const auto stream = stream_id(4);
  const auto payload = bytes("same S0 bytes");

  auto typed_writer = std::move(*codec::CodaWriter::create(typed_path));
  EXPECT_TRUE(typed_writer.append(codec::RecordType::source_bytes, stream, 10,
                                  20, payload));
  EXPECT_TRUE(typed_writer.finalize());

  auto raw_writer = std::move(*codec::CodaWriter::create(raw_path));
  EXPECT_TRUE(raw_writer.append_raw(
      codec::record_type_code(codec::RecordType::source_bytes), stream, 10,
      20, payload));
  EXPECT_TRUE(raw_writer.finalize());

  const auto typed = file_bytes(typed_path);
  const auto raw = file_bytes(raw_path);
  EXPECT_EQ(std::vector<std::byte>(typed.begin() + codec::coda_header_size,
                                   typed.end()),
            std::vector<std::byte>(raw.begin() + codec::coda_header_size,
                                   raw.end()));
  std::filesystem::remove(typed_path);
  std::filesystem::remove(raw_path);
}

TEST(unknown_record_type_code_round_trips_source_bytes_exactly) {
  const auto path = test_path("unknown-record.coda");
  std::filesystem::remove(path);
  constexpr codec::RecordTypeCode unknown_type = 0x7a51;
  const auto stream = stream_id(5);
  const auto payload =
      bytes(std::string_view{"opaque provider payload\0with binary", 35});

  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_raw(unknown_type, stream, 100, 200, payload));
  EXPECT_TRUE(writer.finalize());

  auto archive = std::move(*codec::CodaArchive::open(path));
  auto records = archive.records();
  EXPECT_TRUE(records);
  EXPECT_EQ(records->front().type_code(), unknown_type);
  auto extracted = archive.extract_stream_raw(stream, unknown_type);
  EXPECT_TRUE(extracted);
  EXPECT_EQ(*extracted, payload);
  std::filesystem::remove(path);
}

TEST(repair_preserves_unknown_record_type_code_and_payload) {
  const auto source = test_path("unknown-repair-source.coda");
  const auto repaired = test_path("unknown-repair-output.coda");
  std::filesystem::remove(source);
  std::filesystem::remove(repaired);
  constexpr codec::RecordTypeCode unknown_type = 0x7a52;
  const auto stream = stream_id(6);
  const auto payload = bytes("opaque record survives repair");

  auto writer = std::move(*codec::CodaWriter::create(source));
  EXPECT_TRUE(writer.append_raw(unknown_type, stream, 1, 2, payload));
  auto torn = writer.append(codec::RecordType::source_bytes, stream, 3, 4,
                            bytes("torn tail"));
  EXPECT_TRUE(torn);
  EXPECT_TRUE(writer.finalize());
  std::filesystem::resize_file(
      source, torn->file_offset + codec::coda_record_envelope_size + 1);

  auto repair = codec::CodaArchive::repair(source, repaired);
  EXPECT_TRUE(repair);
  auto archive = std::move(*codec::CodaArchive::open(repaired));
  auto records = archive.records();
  EXPECT_TRUE(records);
  EXPECT_EQ(records->front().type_code(), unknown_type);
  auto extracted = archive.extract_stream_raw(stream, unknown_type);
  EXPECT_TRUE(extracted);
  EXPECT_EQ(*extracted, payload);
  std::filesystem::remove(source);
  std::filesystem::remove(repaired);
}

TEST(generic_stream_descriptor_round_trips_without_profile_interpretation) {
  const auto path = test_path("stream-descriptor.coda");
  std::filesystem::remove(path);
  const codec::StreamDescriptor descriptor{
      .id = stream_id(7),
      .type = codec::StreamType::telemetry,
      .label = "reactor-temperature",
      .source_id = "plant-a/sensor-42",
      .payload_type = "application/vnd.example.telemetry+cbor",
  };
  const auto payload =
      bytes(std::string_view{"\x01\x00\xff\x7f", 4});

  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(descriptor, 100));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, descriptor.id,
                            101, 102, payload));
  EXPECT_TRUE(writer.finalize());

  auto archive = std::move(*codec::CodaArchive::open(path));
  auto streams = archive.streams();
  EXPECT_TRUE(streams);
  EXPECT_EQ(streams->size(), std::size_t{1});
  EXPECT_EQ(streams->front().id, descriptor.id);
  EXPECT_EQ(streams->front().type, descriptor.type);
  EXPECT_EQ(streams->front().label, descriptor.label);
  EXPECT_EQ(streams->front().source_id, descriptor.source_id);
  EXPECT_EQ(streams->front().payload_type, descriptor.payload_type);
  auto extracted = archive.extract_stream(descriptor.id);
  EXPECT_TRUE(extracted);
  EXPECT_EQ(*extracted, payload);
  std::filesystem::remove(path);
}

TEST(generic_stream_continuity_round_trips_exact_timing_and_gaps) {
  const auto path = test_path("stream-continuity.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id(8);

  auto writer = std::move(*codec::CodaWriter::create(path));
  auto first_source = writer.append(codec::RecordType::source_bytes, stream,
                                    100, 110, bytes("sample-0"));
  EXPECT_TRUE(first_source);
  const codec::StreamClock first_clock{
      .monotonic_ns = 1000,
      .observed_utc_ns = 2000,
      .observed_utc_uncertainty_ns = 25,
      .source_timestamp = 90,
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 1000,
  };
  const codec::StreamEpoch first_epoch{.connection = 0, .format = 0};
  EXPECT_TRUE(writer.append_stream_timing(*first_source, 0, first_clock,
                                          first_epoch));
  EXPECT_TRUE(writer.append_stream_gap(stream, 1, 2, first_epoch));

  auto second_source = writer.append(codec::RecordType::source_bytes, stream,
                                     120, 130, bytes("sample-3"));
  EXPECT_TRUE(second_source);
  const codec::StreamClock second_clock{
      .monotonic_ns = 50,
      .observed_utc_ns = 3000,
      .observed_utc_uncertainty_ns = 30,
      .source_timestamp = 15,
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 48000,
  };
  const codec::StreamEpoch second_epoch{.connection = 1, .format = 1};
  EXPECT_TRUE(writer.append_stream_timing(*second_source, 3, second_clock,
                                          second_epoch));
  EXPECT_TRUE(writer.finalize());

  auto archive = std::move(*codec::CodaArchive::open(path));
  auto continuity = archive.continuity();
  EXPECT_TRUE(continuity);
  EXPECT_EQ(continuity->size(), std::size_t{3});

  const auto& first = continuity->at(0);
  EXPECT_EQ(first.kind, codec::StreamContinuityKind::timing);
  EXPECT_EQ(first.timing.stream, stream);
  EXPECT_EQ(first.timing.sequence, std::uint64_t{0});
  EXPECT_EQ(first.timing.clock.monotonic_ns, std::int64_t{1000});
  EXPECT_EQ(first.timing.clock.observed_utc_ns, std::int64_t{2000});
  EXPECT_EQ(first.timing.clock.observed_utc_uncertainty_ns,
            std::uint64_t{25});
  EXPECT_EQ(first.timing.clock.source_timestamp, std::int64_t{90});
  EXPECT_EQ(first.timing.clock.source_timebase_numerator, std::int64_t{1});
  EXPECT_EQ(first.timing.clock.source_timebase_denominator,
            std::int64_t{1000});
  EXPECT_EQ(first.timing.epoch.connection, std::uint64_t{0});
  EXPECT_EQ(first.timing.epoch.format, std::uint64_t{0});
  EXPECT_EQ(first.timing.source_record_sequence, first_source->sequence);
  EXPECT_EQ(first.timing.source_record_hash, first_source->hash);

  const auto& gap = continuity->at(1);
  EXPECT_EQ(gap.kind, codec::StreamContinuityKind::gap);
  EXPECT_EQ(gap.gap.stream, stream);
  EXPECT_EQ(gap.gap.first_sequence, std::uint64_t{1});
  EXPECT_EQ(gap.gap.missing_count, std::uint64_t{2});
  EXPECT_EQ(gap.gap.epoch.connection, std::uint64_t{0});
  EXPECT_EQ(gap.gap.epoch.format, std::uint64_t{0});

  const auto& second = continuity->at(2);
  EXPECT_EQ(second.kind, codec::StreamContinuityKind::timing);
  EXPECT_EQ(second.timing.stream, stream);
  EXPECT_EQ(second.timing.sequence, std::uint64_t{3});
  EXPECT_EQ(second.timing.clock.monotonic_ns, std::int64_t{50});
  EXPECT_EQ(second.timing.clock.observed_utc_ns, std::int64_t{3000});
  EXPECT_EQ(second.timing.clock.observed_utc_uncertainty_ns,
            std::uint64_t{30});
  EXPECT_EQ(second.timing.clock.source_timestamp, std::int64_t{15});
  EXPECT_EQ(second.timing.clock.source_timebase_numerator, std::int64_t{1});
  EXPECT_EQ(second.timing.clock.source_timebase_denominator,
            std::int64_t{48000});
  EXPECT_EQ(second.timing.epoch.connection, std::uint64_t{1});
  EXPECT_EQ(second.timing.epoch.format, std::uint64_t{1});
  EXPECT_EQ(second.timing.source_record_sequence, second_source->sequence);
  EXPECT_EQ(second.timing.source_record_hash, second_source->hash);
  std::filesystem::remove(path);
}

TEST(writer_rejects_timing_source_not_committed_exactly_by_this_writer) {
  const auto path = test_path("continuity-source-link.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id(81);
  auto writer = std::move(*codec::CodaWriter::create(path));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                              bytes("linked source"));
  EXPECT_TRUE(source);
  const codec::StreamClock clock{
      .monotonic_ns = 10,
      .observed_utc_ns = 20,
      .observed_utc_uncertainty_ns = 1,
      .source_timestamp = 30,
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 1000,
  };
  const codec::StreamEpoch epoch{};

  auto absent = *source;
  absent.sequence = 99;
  auto absent_result = writer.append_stream_timing(absent, 0, clock, epoch);
  EXPECT_FALSE(absent_result);
  EXPECT_EQ(absent_result.error().code, codec::ErrorCode::invalid_argument);

  auto wrong_hash = *source;
  wrong_hash.hash[0] ^= 0xffU;
  auto hash_result =
      writer.append_stream_timing(wrong_hash, 0, clock, epoch);
  EXPECT_FALSE(hash_result);
  EXPECT_EQ(hash_result.error().code, codec::ErrorCode::invalid_argument);

  auto wrong_stream = *source;
  wrong_stream.stream = stream_id(82);
  auto stream_result =
      writer.append_stream_timing(wrong_stream, 0, clock, epoch);
  EXPECT_FALSE(stream_result);
  EXPECT_EQ(stream_result.error().code, codec::ErrorCode::invalid_argument);

  auto wrong_type = *source;
  wrong_type.type = codec::RecordType::pcm16;
  auto type_result =
      writer.append_stream_timing(wrong_type, 0, clock, epoch);
  EXPECT_FALSE(type_result);
  EXPECT_EQ(type_result.error().code, codec::ErrorCode::invalid_argument);

  EXPECT_TRUE(writer.append_stream_timing(*source, 0, clock, epoch));
  EXPECT_TRUE(writer.finalize());
  std::filesystem::remove(path);
}

TEST(writer_rejects_invalid_timebase_and_unannounced_sequence_jump) {
  const auto path = test_path("continuity-sequence.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id(83);
  auto writer = std::move(*codec::CodaWriter::create(path));
  auto first = writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                             bytes("sequence zero"));
  EXPECT_TRUE(first);
  codec::StreamClock clock{
      .monotonic_ns = 10,
      .observed_utc_ns = 20,
      .observed_utc_uncertainty_ns = 1,
      .source_timestamp = 30,
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 1000,
  };
  const codec::StreamEpoch epoch{};

  clock.source_timebase_denominator = 0;
  auto zero_denominator =
      writer.append_stream_timing(*first, 0, clock, epoch);
  EXPECT_FALSE(zero_denominator);
  EXPECT_EQ(zero_denominator.error().code,
            codec::ErrorCode::invalid_argument);
  clock.source_timebase_denominator = 1000;
  clock.source_timebase_numerator = -1;
  auto negative_numerator =
      writer.append_stream_timing(*first, 0, clock, epoch);
  EXPECT_FALSE(negative_numerator);
  EXPECT_EQ(negative_numerator.error().code,
            codec::ErrorCode::invalid_argument);
  clock.source_timebase_numerator = 1;
  EXPECT_TRUE(writer.append_stream_timing(*first, 0, clock, epoch));

  auto second = writer.append(codec::RecordType::source_bytes, stream, 3, 4,
                              bytes("sequence two"));
  EXPECT_TRUE(second);
  clock.monotonic_ns = 11;
  auto jumped = writer.append_stream_timing(*second, 2, clock, epoch);
  EXPECT_FALSE(jumped);
  EXPECT_EQ(jumped.error().code, codec::ErrorCode::invalid_argument);
  EXPECT_TRUE(writer.append_stream_gap(stream, 1, 1, epoch));
  EXPECT_TRUE(writer.append_stream_timing(*second, 2, clock, epoch));
  EXPECT_TRUE(writer.finalize());
  std::filesystem::remove(path);
}

TEST(writer_rejects_invalid_gap_and_epoch_regression) {
  const auto path = test_path("continuity-gap-epoch.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id(84);
  auto writer = std::move(*codec::CodaWriter::create(path));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                              bytes("epoch source"));
  EXPECT_TRUE(source);
  const codec::StreamClock clock{
      .monotonic_ns = 100,
      .observed_utc_ns = 200,
      .observed_utc_uncertainty_ns = 2,
      .source_timestamp = 300,
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 1000,
  };
  const codec::StreamEpoch epoch{.connection = 1, .format = 1};
  EXPECT_TRUE(writer.append_stream_timing(*source, 0, clock, epoch));

  auto zero = writer.append_stream_gap(stream, 1, 0, epoch);
  EXPECT_FALSE(zero);
  EXPECT_EQ(zero.error().code, codec::ErrorCode::invalid_argument);
  auto wrong_start = writer.append_stream_gap(stream, 2, 1, epoch);
  EXPECT_FALSE(wrong_start);
  EXPECT_EQ(wrong_start.error().code, codec::ErrorCode::invalid_argument);
  auto overflow = writer.append_stream_gap(
      stream, 1, std::numeric_limits<std::uint64_t>::max(), epoch);
  EXPECT_FALSE(overflow);
  EXPECT_EQ(overflow.error().code, codec::ErrorCode::invalid_argument);
  auto connection_regression = writer.append_stream_gap(
      stream, 1, 1, codec::StreamEpoch{.connection = 0, .format = 1});
  EXPECT_FALSE(connection_regression);
  EXPECT_EQ(connection_regression.error().code,
            codec::ErrorCode::invalid_argument);
  auto format_regression = writer.append_stream_gap(
      stream, 1, 1, codec::StreamEpoch{.connection = 1, .format = 0});
  EXPECT_FALSE(format_regression);
  EXPECT_EQ(format_regression.error().code, codec::ErrorCode::invalid_argument);

  EXPECT_TRUE(writer.append_stream_gap(stream, 1, 1, epoch));
  EXPECT_TRUE(writer.finalize());
  std::filesystem::remove(path);
}

TEST(writer_enforces_monotonic_clock_only_within_a_connection_epoch) {
  const auto path = test_path("continuity-clock.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id(85);
  auto writer = std::move(*codec::CodaWriter::create(path));
  auto first = writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                             bytes("clock zero"));
  EXPECT_TRUE(first);
  codec::StreamClock clock{
      .monotonic_ns = 100,
      .observed_utc_ns = 200,
      .observed_utc_uncertainty_ns = 2,
      .source_timestamp = 300,
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 1000,
  };
  EXPECT_TRUE(writer.append_stream_timing(
      *first, 0, clock, codec::StreamEpoch{.connection = 0, .format = 0}));
  auto second = writer.append(codec::RecordType::source_bytes, stream, 3, 4,
                              bytes("clock one"));
  EXPECT_TRUE(second);

  clock.monotonic_ns = 99;
  auto regressed = writer.append_stream_timing(
      *second, 1, clock,
      codec::StreamEpoch{.connection = 0, .format = 0});
  EXPECT_FALSE(regressed);
  EXPECT_EQ(regressed.error().code, codec::ErrorCode::invalid_argument);
  EXPECT_TRUE(writer.append_stream_timing(
      *second, 1, clock,
      codec::StreamEpoch{.connection = 1, .format = 0}));
  EXPECT_TRUE(writer.finalize());
  std::filesystem::remove(path);
}

TEST(reader_rejects_malformed_hash_valid_continuity_payloads) {
  const codec::StreamClock clock{
      .monotonic_ns = 10,
      .observed_utc_ns = 20,
      .observed_utc_uncertainty_ns = 1,
      .source_timestamp = 30,
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 1000,
  };
  for (std::uint8_t corruption = 0; corruption < 4; ++corruption) {
    expect_invalid_continuity_metadata(
        "continuity-malformed-" + std::to_string(corruption),
        stream_id(static_cast<std::uint8_t>(90 + corruption)),
        [clock, corruption](codec::CodaWriter& writer,
                            const codec::RecordInfo& source) {
          auto payload = timing_payload(0, clock, {}, source.sequence,
                                        source.hash);
          if (corruption == 0) payload[0] = std::byte{'X'};
          if (corruption == 1) put_le<std::uint16_t>(payload, 4, 2);
          if (corruption == 2) put_le<std::uint16_t>(payload, 6, 1);
          if (corruption == 3) payload.resize(payload.size() - 1);
          EXPECT_TRUE(writer.append_raw(
              codec::record_type_code(codec::RecordType::stream_timing),
              source.stream, 10, 10, payload));
        });
  }
}

TEST(reader_rejects_invalid_source_record_integrity_links) {
  const codec::StreamClock clock{
      .monotonic_ns = 10,
      .observed_utc_ns = 20,
      .observed_utc_uncertainty_ns = 1,
      .source_timestamp = 30,
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 1000,
  };
  expect_invalid_continuity_metadata(
      "continuity-missing-source", stream_id(95),
      [clock](codec::CodaWriter& writer, const codec::RecordInfo& source) {
        EXPECT_TRUE(writer.append_raw(
            codec::record_type_code(codec::RecordType::stream_timing),
            source.stream, 10, 10,
            timing_payload(0, clock, {}, 99, source.hash)));
      });
  expect_invalid_continuity_metadata(
      "continuity-wrong-source-hash", stream_id(96),
      [clock](codec::CodaWriter& writer, const codec::RecordInfo& source) {
        auto wrong_hash = source.hash;
        wrong_hash[0] ^= 0xffU;
        EXPECT_TRUE(writer.append_raw(
            codec::record_type_code(codec::RecordType::stream_timing),
            source.stream, 10, 10,
            timing_payload(0, clock, {}, source.sequence, wrong_hash)));
      });
  expect_invalid_continuity_metadata(
      "continuity-wrong-source-stream", stream_id(97),
      [clock](codec::CodaWriter& writer, const codec::RecordInfo& source) {
        EXPECT_TRUE(writer.append_raw(
            codec::record_type_code(codec::RecordType::stream_timing),
            stream_id(98), 10, 10,
            timing_payload(0, clock, {}, source.sequence, source.hash)));
      });
  expect_invalid_continuity_metadata(
      "continuity-non-source-link", stream_id(99),
      [clock](codec::CodaWriter& writer, const codec::RecordInfo& source) {
        auto pcm = writer.append(codec::RecordType::pcm16, source.stream, 3, 4,
                                 bytes("not source bytes"));
        EXPECT_TRUE(pcm);
        EXPECT_TRUE(writer.append_raw(
            codec::record_type_code(codec::RecordType::stream_timing),
            source.stream, 10, 10,
            timing_payload(0, clock, {}, pcm->sequence, pcm->hash)));
      });
  expect_invalid_continuity_metadata(
      "continuity-later-source-link", stream_id(100),
      [clock](codec::CodaWriter& writer, const codec::RecordInfo& source) {
        codec::Sha256 future_hash{};
        EXPECT_TRUE(writer.append_raw(
            codec::record_type_code(codec::RecordType::stream_timing),
            source.stream, 10, 10,
            timing_payload(0, clock, {}, source.sequence + 2, future_hash)));
        EXPECT_TRUE(writer.append(codec::RecordType::source_bytes,
                                  source.stream, 5, 6, bytes("future")));
      });
}

TEST(reader_rejects_invalid_stream_ledger_semantics) {
  const codec::StreamClock clock{
      .monotonic_ns = 100,
      .observed_utc_ns = 200,
      .observed_utc_uncertainty_ns = 2,
      .source_timestamp = 300,
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 1000,
  };
  expect_invalid_continuity_metadata(
      "continuity-sequence-jump", stream_id(101),
      [clock](codec::CodaWriter& writer, const codec::RecordInfo& source) {
        EXPECT_TRUE(writer.append_raw(
            codec::record_type_code(codec::RecordType::stream_timing),
            source.stream, 100, 100,
            timing_payload(1, clock, {}, source.sequence, source.hash)));
      });
  expect_invalid_continuity_metadata(
      "continuity-invalid-timebase", stream_id(102),
      [clock](codec::CodaWriter& writer, const codec::RecordInfo& source) {
        auto invalid_clock = clock;
        invalid_clock.source_timebase_denominator = 0;
        EXPECT_TRUE(writer.append_raw(
            codec::record_type_code(codec::RecordType::stream_timing),
            source.stream, 100, 100,
            timing_payload(0, invalid_clock, {}, source.sequence,
                           source.hash)));
      });
  expect_invalid_continuity_metadata(
      "continuity-zero-gap", stream_id(103),
      [](codec::CodaWriter& writer, const codec::RecordInfo& source) {
        EXPECT_TRUE(writer.append_raw(
            codec::record_type_code(codec::RecordType::gap), source.stream, 0,
            0, gap_payload(0, 0, {})));
      });
  expect_invalid_continuity_metadata(
      "continuity-overflow-gap", stream_id(104),
      [clock](codec::CodaWriter& writer, const codec::RecordInfo& source) {
        EXPECT_TRUE(writer.append_stream_timing(source, 0, clock, {}));
        EXPECT_TRUE(writer.append_raw(
            codec::record_type_code(codec::RecordType::gap), source.stream, 0,
            0, gap_payload(1, std::numeric_limits<std::uint64_t>::max(),
                           {})));
      });
  expect_invalid_continuity_metadata(
      "continuity-epoch-regression", stream_id(105),
      [clock](codec::CodaWriter& writer, const codec::RecordInfo& source) {
        EXPECT_TRUE(writer.append_stream_timing(
            source, 0, clock,
            codec::StreamEpoch{.connection = 1, .format = 1}));
        EXPECT_TRUE(writer.append_raw(
            codec::record_type_code(codec::RecordType::gap), source.stream, 0,
            0, gap_payload(1, 1,
                           codec::StreamEpoch{.connection = 0, .format = 1})));
      });
  expect_invalid_continuity_metadata(
      "continuity-clock-regression", stream_id(106),
      [clock](codec::CodaWriter& writer, const codec::RecordInfo& source) {
        EXPECT_TRUE(writer.append_stream_timing(source, 0, clock, {}));
        auto second = writer.append(codec::RecordType::source_bytes,
                                    source.stream, 3, 4, bytes("second"));
        EXPECT_TRUE(second);
        auto regressed_clock = clock;
        regressed_clock.monotonic_ns = 99;
        EXPECT_TRUE(writer.append_raw(
            codec::record_type_code(codec::RecordType::stream_timing),
            source.stream, 99, 99,
            timing_payload(1, regressed_clock, {}, second->sequence,
                           second->hash)));
      });
}

TEST(repair_preserves_valid_stream_continuity_and_source_links) {
  const auto source_path = test_path("continuity-repair-source.coda");
  const auto repaired_path = test_path("continuity-repair-output.coda");
  std::filesystem::remove(source_path);
  std::filesystem::remove(repaired_path);
  const auto stream = stream_id(107);
  const auto first_bytes = bytes("first source");
  const auto second_bytes = bytes("second source");

  auto writer = std::move(*codec::CodaWriter::create(source_path));
  auto first = writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                             first_bytes);
  EXPECT_TRUE(first);
  const codec::StreamClock first_clock{
      .monotonic_ns = 100,
      .observed_utc_ns = 200,
      .observed_utc_uncertainty_ns = 2,
      .source_timestamp = 300,
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 1000,
  };
  EXPECT_TRUE(writer.append_stream_timing(*first, 0, first_clock, {}));
  EXPECT_TRUE(writer.append_stream_gap(stream, 1, 2, {}));
  auto second = writer.append(codec::RecordType::source_bytes, stream, 3, 4,
                              second_bytes);
  EXPECT_TRUE(second);
  auto second_clock = first_clock;
  second_clock.monotonic_ns = 50;
  const codec::StreamEpoch second_epoch{.connection = 1, .format = 1};
  EXPECT_TRUE(
      writer.append_stream_timing(*second, 3, second_clock, second_epoch));
  auto torn = writer.append(codec::RecordType::source_bytes, stream, 5, 6,
                            bytes("torn tail"));
  EXPECT_TRUE(torn);
  EXPECT_TRUE(writer.finalize());
  std::filesystem::resize_file(
      source_path,
      torn->file_offset + codec::coda_record_envelope_size + std::uint64_t{1});

  auto repair = codec::CodaArchive::repair(source_path, repaired_path);
  EXPECT_TRUE(repair);
  EXPECT_EQ(repair->recovered_records, std::uint64_t{5});
  auto archive = std::move(*codec::CodaArchive::open(repaired_path));
  EXPECT_TRUE(archive.verify().ok);
  auto continuity = archive.continuity();
  EXPECT_TRUE(continuity);
  EXPECT_EQ(continuity->size(), std::size_t{3});
  EXPECT_EQ(continuity->at(0).timing.source_record_sequence,
            first->sequence);
  EXPECT_EQ(continuity->at(0).timing.source_record_hash, first->hash);
  EXPECT_EQ(continuity->at(1).gap.first_sequence, std::uint64_t{1});
  EXPECT_EQ(continuity->at(1).gap.missing_count, std::uint64_t{2});
  EXPECT_EQ(continuity->at(2).timing.source_record_sequence,
            second->sequence);
  EXPECT_EQ(continuity->at(2).timing.source_record_hash, second->hash);
  auto extracted = archive.extract_stream(stream);
  EXPECT_TRUE(extracted);
  auto expected = first_bytes;
  expected.insert(expected.end(), second_bytes.begin(), second_bytes.end());
  EXPECT_EQ(*extracted, expected);
  std::filesystem::remove(source_path);
  std::filesystem::remove(repaired_path);
}

TEST(invalid_stream_descriptor_does_not_corrupt_committed_s0) {
  const auto path = test_path("invalid-stream-descriptor.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id(9);
  const auto payload = bytes("committed before optional metadata");

  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                            payload));
  const codec::StreamDescriptor invalid{
      .id = stream,
      .type = codec::StreamType::telemetry,
      .label = "missing-payload-type",
      .source_id = "sensor-9",
      .payload_type = "",
  };
  auto appended = writer.append_stream_descriptor(invalid, 3);
  EXPECT_FALSE(appended);
  EXPECT_EQ(appended.error().code, codec::ErrorCode::invalid_argument);
  EXPECT_TRUE(writer.finalize());

  auto archive = std::move(*codec::CodaArchive::open(path));
  EXPECT_TRUE(archive.verify().ok);
  auto extracted = archive.extract_stream(stream);
  EXPECT_TRUE(extracted);
  EXPECT_EQ(*extracted, payload);
  std::filesystem::remove(path);
}

TEST(malformed_stream_descriptor_does_not_replace_committed_s0) {
  const auto path = test_path("malformed-stream-descriptor.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id(10);
  const auto payload = bytes("source remains independently readable");

  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                            payload));
  EXPECT_TRUE(writer.append(codec::RecordType::stream_descriptor, stream, 3,
                            3, bytes("not an SDS1 descriptor")));
  EXPECT_TRUE(writer.finalize());

  auto archive = std::move(*codec::CodaArchive::open(path));
  EXPECT_TRUE(archive.verify().ok);
  auto streams = archive.streams();
  EXPECT_FALSE(streams);
  EXPECT_EQ(streams.error().code, codec::ErrorCode::archive_corrupt);
  auto extracted = archive.extract_stream(stream);
  EXPECT_TRUE(extracted);
  EXPECT_EQ(*extracted, payload);
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

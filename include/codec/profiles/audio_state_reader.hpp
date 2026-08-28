#pragma once

#include <codec/archive.hpp>
#include <codec/audio.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace codec::profiles::audio {

struct Pcm16StateQuery {
  std::optional<StreamId> stream;
  std::optional<RecordTimeRange> time;
  std::size_t maximum_results{1024};
  std::uint64_t maximum_encoded_bytes{64ULL * 1024ULL * 1024ULL};
};

struct VerifiedPcm16State {
  Pcm16State state;
  RecordInfo state_record;
  RecordInfo source_record;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedPcm16State>> query_verified_pcm16_states(
    const CodaArchive& archive, const Pcm16StateQuery& query = {});

}  // namespace codec::profiles::audio

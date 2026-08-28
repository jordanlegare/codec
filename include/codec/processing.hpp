#pragma once

#include <codec/archive.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace codec {

// One owned S0 record emitted by a profile adapter.
struct AdapterRecord {
  StreamId stream{};
  RecordTypeCode type{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::vector<std::byte> payload;
};

struct AdapterReadLimits {
  std::uint64_t maximum_payload_bytes{16ULL * 1024ULL * 1024ULL};
};

class StreamAdapter {
 public:
  virtual ~StreamAdapter() = default;
  virtual std::string name() const = 0;
  virtual Result<std::optional<AdapterRecord>> next() = 0;
};

// Pulls one S0 record with caller-controlled backpressure. An empty optional
// means end-of-stream; provider failures remain explicit errors.
Result<std::optional<AdapterRecord>> pull_adapter_record(
    StreamAdapter& adapter, AdapterReadLimits limits = {});

// One owned S1 or D artifact returned by a profile processor. Every input
// supplied to process() is a direct supporting input for every output.
struct ProcessorOutput {
  StreamId stream{};
  RecordTypeCode type{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  TruthClass truth{TruthClass::derived};
  std::vector<std::byte> payload;
  ProvenanceProcess process;
};

struct ProcessorRunLimits {
  std::size_t maximum_outputs{1024};
  std::uint64_t maximum_output_bytes{64ULL * 1024ULL * 1024ULL};
};

class StreamProcessor {
 public:
  virtual ~StreamProcessor() = default;
  virtual std::string name() const = 0;
  virtual Result<std::vector<ProcessorOutput>> process(
      std::span<const ExtractedRecord> inputs) = 0;
};

// Validates exact input ownership, output truth/process metadata, and caller
// resource bounds. This function never writes an archive.
Result<std::vector<ProcessorOutput>> invoke_processor(
    StreamProcessor& processor, std::span<const ExtractedRecord> inputs,
    ProcessorRunLimits limits = {});

}  // namespace codec

#pragma once

#include <codec/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace codec {

struct FeedStatement {
  std::string feed_uuid;
  std::uint16_t acoustic_code{};
  std::int64_t issued_at_seconds{};
  std::int64_t not_before_seconds{};
  std::int64_t expires_at_seconds{};
  std::string issuer;
  std::string key_id;
};

enum class StatementState {
  valid,
  invalid_signature,
  not_yet_valid,
  expired,
};

const char* statement_state_name(StatementState state) noexcept;

struct StatementVerification {
  StatementState state{StatementState::invalid_signature};
  FeedStatement statement;
  bool verified() const noexcept { return state == StatementState::valid; }
};

Result<void> generate_ed25519_keypair(
    const std::filesystem::path& private_key_path,
    const std::filesystem::path& public_key_path);

Result<std::vector<std::byte>> issue_statement(
    const FeedStatement& statement,
    const std::filesystem::path& private_key_path);

Result<StatementVerification> verify_statement(
    std::span<const std::byte> cose_sign1,
    const std::filesystem::path& public_key_path,
    std::int64_t at_seconds);

}  // namespace codec


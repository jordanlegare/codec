#include "test.hpp"

#include <codec/statement.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::filesystem::path key_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-test-" + std::string{name});
}

struct Keys {
  std::filesystem::path private_key;
  std::filesystem::path public_key;

  explicit Keys(std::string_view name)
      : private_key(key_path(std::string{name} + ".key")),
        public_key(key_path(std::string{name} + ".pub")) {
    std::filesystem::remove(private_key);
    std::filesystem::remove(public_key);
    auto result = codec::generate_ed25519_keypair(private_key, public_key);
    if (!result) throw std::runtime_error(result.error().message);
  }

  ~Keys() {
    std::filesystem::remove(private_key);
    std::filesystem::remove(public_key);
  }
};

codec::FeedStatement statement() {
  return {.feed_uuid = "7c2b2f74-7e31-4a1d-b469-d88d63fc8fcb",
          .acoustic_code = 0x4a31,
          .issued_at_seconds = 1'000,
          .not_before_seconds = 1'000,
          .expires_at_seconds = 2'000,
          .issuer = "authorized-test-issuer",
          .key_id = "test-key-7"};
}

}  // namespace

TEST(valid_cose_statement_binds_the_feed_and_acoustic_code) {
  Keys keys("statement-valid");
  auto issued = codec::issue_statement(statement(), keys.private_key);
  EXPECT_TRUE(issued);
  auto verified = codec::verify_statement(*issued, keys.public_key, 1'500);
  EXPECT_TRUE(verified);
  EXPECT_EQ(verified->state, codec::StatementState::valid);
  EXPECT_EQ(verified->statement.feed_uuid, statement().feed_uuid);
  EXPECT_EQ(verified->statement.acoustic_code, std::uint16_t{0x4a31});
  EXPECT_EQ(verified->statement.key_id, std::string{"test-key-7"});
}

TEST(changed_cose_signature_is_never_a_verified_feed) {
  Keys keys("statement-tampered");
  auto issued = codec::issue_statement(statement(), keys.private_key);
  EXPECT_TRUE(issued);
  issued->back() ^= std::byte{0x01};
  auto verified = codec::verify_statement(*issued, keys.public_key, 1'500);
  EXPECT_TRUE(verified);
  EXPECT_EQ(verified->state, codec::StatementState::invalid_signature);
  EXPECT_FALSE(verified->verified());
}

TEST(wrong_public_key_does_not_validate_a_statement) {
  Keys signer("statement-signer");
  Keys stranger("statement-stranger");
  auto issued = codec::issue_statement(statement(), signer.private_key);
  EXPECT_TRUE(issued);
  auto verified = codec::verify_statement(*issued, stranger.public_key, 1'500);
  EXPECT_TRUE(verified);
  EXPECT_EQ(verified->state, codec::StatementState::invalid_signature);
}

TEST(valid_signature_still_obeys_not_before_and_expiry) {
  Keys keys("statement-time");
  auto issued = codec::issue_statement(statement(), keys.private_key);
  EXPECT_TRUE(issued);
  auto early = codec::verify_statement(*issued, keys.public_key, 999);
  auto late = codec::verify_statement(*issued, keys.public_key, 2'001);
  EXPECT_TRUE(early);
  EXPECT_TRUE(late);
  EXPECT_EQ(early->state, codec::StatementState::not_yet_valid);
  EXPECT_EQ(late->state, codec::StatementState::expired);
  EXPECT_FALSE(early->verified());
  EXPECT_FALSE(late->verified());
}

TEST(noncanonical_cbor_container_encoding_is_rejected) {
  Keys keys("statement-noncanonical");
  auto issued = codec::issue_statement(statement(), keys.private_key);
  EXPECT_TRUE(issued);
  issued->erase(issued->begin());
  issued->insert(issued->begin(), std::byte{0x04});
  issued->insert(issued->begin(), std::byte{0x98});
  auto verified = codec::verify_statement(*issued, keys.public_key, 1'500);
  EXPECT_FALSE(verified);
  EXPECT_EQ(verified.error().code,
            codec::ErrorCode::watermark_signature_invalid);
}

TEST(key_generation_refuses_to_replace_an_existing_private_key_path) {
  const auto private_key = key_path("existing-private.key");
  const auto public_key = key_path("existing-private.pub");
  std::filesystem::remove(private_key);
  std::filesystem::remove(public_key);
  {
    std::ofstream output(private_key, std::ios::binary | std::ios::trunc);
    output << "sentinel";
  }
  auto generated = codec::generate_ed25519_keypair(private_key, public_key);
  EXPECT_FALSE(generated);
  std::ifstream input(private_key, std::ios::binary);
  std::string actual((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_EQ(actual, std::string{"sentinel"});
  EXPECT_FALSE(std::filesystem::exists(public_key));
  std::filesystem::remove(private_key);
}

TEST(statement_issuer_rejects_invalid_utf8_before_signing) {
  Keys keys("statement-invalid-utf8");
  auto invalid = statement();
  invalid.issuer = std::string{"\xc0\xaf", 2};
  auto issued = codec::issue_statement(invalid, keys.private_key);
  EXPECT_FALSE(issued);
  EXPECT_EQ(issued.error().code, codec::ErrorCode::invalid_argument);
}

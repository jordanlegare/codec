#include <codec/statement.hpp>

#include "../core/internal.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/buffer.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace codec {
namespace {

using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

void add_head(std::vector<std::byte>& output, std::uint8_t major,
              std::uint64_t value) {
  const auto prefix = static_cast<std::uint8_t>(major << 5U);
  if (value < 24U) {
    output.push_back(static_cast<std::byte>(prefix | value));
  } else if (value <= 0xffU) {
    output.push_back(static_cast<std::byte>(prefix | 24U));
    output.push_back(static_cast<std::byte>(value));
  } else if (value <= 0xffffU) {
    output.push_back(static_cast<std::byte>(prefix | 25U));
    output.push_back(static_cast<std::byte>(value >> 8U));
    output.push_back(static_cast<std::byte>(value));
  } else if (value <= 0xffffffffULL) {
    output.push_back(static_cast<std::byte>(prefix | 26U));
    for (int shift = 24; shift >= 0; shift -= 8) {
      output.push_back(static_cast<std::byte>(value >> shift));
    }
  } else {
    output.push_back(static_cast<std::byte>(prefix | 27U));
    for (int shift = 56; shift >= 0; shift -= 8) {
      output.push_back(static_cast<std::byte>(value >> shift));
    }
  }
}

void add_uint(std::vector<std::byte>& output, std::uint64_t value) {
  add_head(output, 0, value);
}

void add_int(std::vector<std::byte>& output, std::int64_t value) {
  if (value >= 0) {
    add_head(output, 0, static_cast<std::uint64_t>(value));
  } else {
    add_head(output, 1, static_cast<std::uint64_t>(-1 - value));
  }
}

void add_bytes(std::vector<std::byte>& output,
               std::span<const std::byte> value) {
  add_head(output, 2, value.size());
  output.insert(output.end(), value.begin(), value.end());
}

void add_text(std::vector<std::byte>& output, std::string_view value) {
  add_head(output, 3, value.size());
  for (const unsigned char ch : value) {
    output.push_back(static_cast<std::byte>(ch));
  }
}

std::vector<std::byte> protected_header() {
  return {std::byte{0xa1}, std::byte{0x01}, std::byte{0x27}};
}

std::vector<std::byte> encode_payload(const FeedStatement& statement) {
  std::vector<std::byte> payload;
  add_head(payload, 5, 7);
  add_uint(payload, 1);
  add_text(payload, statement.feed_uuid);
  add_uint(payload, 2);
  add_uint(payload, statement.acoustic_code);
  add_uint(payload, 3);
  add_int(payload, statement.issued_at_seconds);
  add_uint(payload, 4);
  add_int(payload, statement.not_before_seconds);
  add_uint(payload, 5);
  add_int(payload, statement.expires_at_seconds);
  add_uint(payload, 6);
  add_text(payload, statement.issuer);
  add_uint(payload, 7);
  add_text(payload, statement.key_id);
  return payload;
}

std::vector<std::byte> signature_structure(
    std::span<const std::byte> protected_bytes,
    std::span<const std::byte> payload) {
  std::vector<std::byte> output;
  add_head(output, 4, 4);
  add_text(output, "Signature1");
  add_bytes(output, protected_bytes);
  add_bytes(output, {});
  add_bytes(output, payload);
  return output;
}

bool valid_utf8(std::span<const std::byte> input) {
  std::size_t index = 0;
  const auto byte = [&](std::size_t at) {
    return static_cast<std::uint8_t>(input[at]);
  };
  while (index < input.size()) {
    const auto first = byte(index);
    if (first <= 0x7fU) {
      ++index;
      continue;
    }
    if (first >= 0xc2U && first <= 0xdfU) {
      if (index + 1 >= input.size() || (byte(index + 1) & 0xc0U) != 0x80U)
        return false;
      index += 2;
      continue;
    }
    if (first >= 0xe0U && first <= 0xefU) {
      if (index + 2 >= input.size() ||
          (byte(index + 1) & 0xc0U) != 0x80U ||
          (byte(index + 2) & 0xc0U) != 0x80U ||
          (first == 0xe0U && byte(index + 1) < 0xa0U) ||
          (first == 0xedU && byte(index + 1) >= 0xa0U)) {
        return false;
      }
      index += 3;
      continue;
    }
    if (first >= 0xf0U && first <= 0xf4U) {
      if (index + 3 >= input.size() ||
          (byte(index + 1) & 0xc0U) != 0x80U ||
          (byte(index + 2) & 0xc0U) != 0x80U ||
          (byte(index + 3) & 0xc0U) != 0x80U ||
          (first == 0xf0U && byte(index + 1) < 0x90U) ||
          (first == 0xf4U && byte(index + 1) >= 0x90U)) {
        return false;
      }
      index += 4;
      continue;
    }
    return false;
  }
  return true;
}

class Reader {
 public:
  explicit Reader(std::span<const std::byte> input) : input_(input) {}

  Result<std::uint64_t> length(std::uint8_t expected_major) {
    if (position_ >= input_.size()) {
      return fail<std::uint64_t>(ErrorCode::watermark_signature_invalid,
                                 "truncated CBOR item");
    }
    const auto initial = static_cast<std::uint8_t>(input_[position_++]);
    const auto major = static_cast<std::uint8_t>(initial >> 5U);
    const auto additional = static_cast<std::uint8_t>(initial & 0x1fU);
    if (major != expected_major || additional == 31U) {
      return fail<std::uint64_t>(ErrorCode::watermark_signature_invalid,
                                 "unexpected or indefinite CBOR item");
    }
    if (additional < 24U) return additional;
    std::size_t bytes = 0;
    if (additional == 24U) bytes = 1;
    else if (additional == 25U) bytes = 2;
    else if (additional == 26U) bytes = 4;
    else if (additional == 27U) bytes = 8;
    else {
      return fail<std::uint64_t>(ErrorCode::watermark_signature_invalid,
                                 "reserved CBOR length encoding");
    }
    if (bytes > input_.size() - position_) {
      return fail<std::uint64_t>(ErrorCode::watermark_signature_invalid,
                                 "truncated CBOR length");
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < bytes; ++index) {
      value = (value << 8U) |
              static_cast<std::uint8_t>(input_[position_++]);
    }
    if ((additional == 24U && value < 24U) ||
        (additional == 25U && value <= 0xffU) ||
        (additional == 26U && value <= 0xffffU) ||
        (additional == 27U && value <= 0xffffffffULL)) {
      return fail<std::uint64_t>(ErrorCode::watermark_signature_invalid,
                                 "non-canonical CBOR length or integer");
    }
    return value;
  }

  Result<std::uint64_t> uint() { return length(0); }

  Result<std::int64_t> integer() {
    if (position_ >= input_.size()) {
      return fail<std::int64_t>(ErrorCode::watermark_signature_invalid,
                                "truncated CBOR integer");
    }
    const auto major =
        static_cast<std::uint8_t>(input_[position_]) >> 5U;
    if (major == 0U) {
      auto value = length(0);
      if (!value || *value >
                        static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max())) {
        return fail<std::int64_t>(ErrorCode::watermark_signature_invalid,
                                  "CBOR integer out of range");
      }
      return static_cast<std::int64_t>(*value);
    }
    if (major == 1U) {
      auto value = length(1);
      if (!value || *value >
                        static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max())) {
        return fail<std::int64_t>(ErrorCode::watermark_signature_invalid,
                                  "CBOR integer out of range");
      }
      return -1 - static_cast<std::int64_t>(*value);
    }
    return fail<std::int64_t>(ErrorCode::watermark_signature_invalid,
                              "expected CBOR integer");
  }

  Result<std::vector<std::byte>> bytes() {
    auto size = length(2);
    if (!size) return size.error();
    if (*size > input_.size() - position_) {
      return fail<std::vector<std::byte>>(
          ErrorCode::watermark_signature_invalid, "truncated CBOR byte string");
    }
    std::vector<std::byte> value(input_.begin() + position_,
                                 input_.begin() + position_ + *size);
    position_ += static_cast<std::size_t>(*size);
    return value;
  }

  Result<std::string> text() {
    auto size = length(3);
    if (!size) return size.error();
    if (*size > input_.size() - position_) {
      return fail<std::string>(ErrorCode::watermark_signature_invalid,
                               "truncated CBOR text string");
    }
    const auto encoded = input_.subspan(position_, static_cast<std::size_t>(*size));
    if (!valid_utf8(encoded)) {
      return fail<std::string>(ErrorCode::watermark_signature_invalid,
                               "CBOR text is not valid UTF-8");
    }
    std::string value;
    value.reserve(static_cast<std::size_t>(*size));
    for (std::size_t index = 0; index < *size; ++index) {
      value.push_back(static_cast<char>(input_[position_ + index]));
    }
    position_ += static_cast<std::size_t>(*size);
    return value;
  }

  bool complete() const noexcept { return position_ == input_.size(); }

 private:
  std::span<const std::byte> input_;
  std::size_t position_{};
};

struct DecodedCose {
  std::vector<std::byte> protected_bytes;
  std::vector<std::byte> payload;
  std::vector<std::byte> signature;
  std::vector<std::byte> key_id;
};

Result<DecodedCose> decode_cose(std::span<const std::byte> input) {
  Reader reader(input);
  auto array_size = reader.length(4);
  if (!array_size || *array_size != 4) {
    return fail<DecodedCose>(ErrorCode::watermark_signature_invalid,
                             "COSE_Sign1 must be a four-item array");
  }
  auto protected_bytes = reader.bytes();
  auto map_size = reader.length(5);
  if (!protected_bytes || !map_size || *map_size != 1) {
    return fail<DecodedCose>(ErrorCode::watermark_signature_invalid,
                             "invalid COSE protected/unprotected headers");
  }
  auto kid_label = reader.uint();
  auto kid = reader.bytes();
  auto payload = reader.bytes();
  auto signature = reader.bytes();
  if (!kid_label || *kid_label != 4 || !kid || !payload || !signature ||
      !reader.complete()) {
    return fail<DecodedCose>(ErrorCode::watermark_signature_invalid,
                             "malformed COSE_Sign1 object");
  }
  const auto expected = protected_header();
  if (*protected_bytes != expected || signature->size() != 64) {
    return fail<DecodedCose>(ErrorCode::watermark_signature_invalid,
                             "COSE statement is not EdDSA/Ed25519");
  }
  return DecodedCose{std::move(*protected_bytes), std::move(*payload),
                     std::move(*signature), std::move(*kid)};
}

Result<FeedStatement> decode_payload(std::span<const std::byte> input) {
  Reader reader(input);
  auto map_size = reader.length(5);
  if (!map_size || *map_size != 7) {
    return fail<FeedStatement>(ErrorCode::watermark_signature_invalid,
                               "W0 payload must contain seven fields");
  }
  FeedStatement statement;
  for (std::uint64_t expected_key = 1; expected_key <= 7; ++expected_key) {
    auto key = reader.uint();
    if (!key || *key != expected_key) {
      return fail<FeedStatement>(ErrorCode::watermark_signature_invalid,
                                 "W0 payload is not canonical");
    }
    if (expected_key == 1) {
      auto value = reader.text();
      if (!value) return value.error();
      statement.feed_uuid = std::move(*value);
    } else if (expected_key == 2) {
      auto value = reader.uint();
      if (!value || *value > 0xffffU) {
        return fail<FeedStatement>(ErrorCode::watermark_signature_invalid,
                                   "W0 acoustic code is invalid");
      }
      statement.acoustic_code = static_cast<std::uint16_t>(*value);
    } else if (expected_key >= 3 && expected_key <= 5) {
      auto value = reader.integer();
      if (!value) return value.error();
      if (expected_key == 3) statement.issued_at_seconds = *value;
      if (expected_key == 4) statement.not_before_seconds = *value;
      if (expected_key == 5) statement.expires_at_seconds = *value;
    } else {
      auto value = reader.text();
      if (!value) return value.error();
      if (expected_key == 6) statement.issuer = std::move(*value);
      if (expected_key == 7) statement.key_id = std::move(*value);
    }
  }
  if (!reader.complete() || statement.feed_uuid.empty() ||
      statement.issuer.empty() || statement.key_id.empty() ||
      statement.not_before_seconds > statement.expires_at_seconds ||
      statement.issued_at_seconds > statement.expires_at_seconds) {
    return fail<FeedStatement>(ErrorCode::watermark_signature_invalid,
                               "W0 payload fields are invalid");
  }
  return statement;
}

Result<Key> read_key(const std::filesystem::path& path, bool private_key) {
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return fail<Key>(ErrorCode::archive_io,
                     "cannot open key: " + path.string());
  }
  EVP_PKEY* raw = private_key
                      ? PEM_read_PrivateKey(file, nullptr, nullptr, nullptr)
                      : PEM_read_PUBKEY(file, nullptr, nullptr, nullptr);
  std::fclose(file);
  if (raw == nullptr || EVP_PKEY_base_id(raw) != EVP_PKEY_ED25519) {
    if (raw != nullptr) EVP_PKEY_free(raw);
    return fail<Key>(ErrorCode::watermark_signature_invalid,
                     "key is not a valid Ed25519 PEM key");
  }
  return Key{raw, &EVP_PKEY_free};
}

}  // namespace

Result<void> generate_ed25519_keypair(
    const std::filesystem::path& private_key_path,
    const std::filesystem::path& public_key_path) {
  if (private_key_path.empty() || public_key_path.empty() ||
      private_key_path == public_key_path) {
    return fail(ErrorCode::invalid_argument,
                "private and public key paths must be distinct");
  }
  KeyContext context{EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr),
                     &EVP_PKEY_CTX_free};
  EVP_PKEY* generated = nullptr;
  if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
      EVP_PKEY_keygen(context.get(), &generated) != 1) {
    return fail(ErrorCode::internal, "Ed25519 key generation failed");
  }
  Key key{generated, &EVP_PKEY_free};
  auto encode_pem = [&](bool private_key) -> Result<std::vector<std::byte>> {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio{BIO_new(BIO_s_mem()),
                                                  &BIO_free};
    if (!bio) {
      return fail<std::vector<std::byte>>(ErrorCode::internal,
                                          "cannot allocate PEM buffer");
    }
    const auto encoded =
        private_key
            ? PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr,
                                       0, nullptr, nullptr)
            : PEM_write_bio_PUBKEY(bio.get(), key.get());
    BUF_MEM* memory = nullptr;
    BIO_get_mem_ptr(bio.get(), &memory);
    if (encoded != 1 || memory == nullptr || memory->data == nullptr ||
        memory->length == 0) {
      return fail<std::vector<std::byte>>(ErrorCode::internal,
                                          "Ed25519 PEM encoding failed");
    }
    std::vector<std::byte> output(memory->length);
    std::copy_n(reinterpret_cast<const std::byte*>(memory->data),
                memory->length, output.begin());
    return output;
  };
  auto private_pem = encode_pem(true);
  if (!private_pem) return private_pem.error();
  auto public_pem = encode_pem(false);
  if (!public_pem) return public_pem.error();
  auto private_written = detail::write_private_file(private_key_path,
                                                     *private_pem);
  if (!private_written) return private_written.error();
  auto public_written = detail::write_file(public_key_path, *public_pem);
  if (!public_written) {
    std::error_code ignored;
    std::filesystem::remove(private_key_path, ignored);
    return public_written.error();
  }
  return {};
}

Result<std::vector<std::byte>> issue_statement(
    const FeedStatement& statement,
    const std::filesystem::path& private_key_path) {
  const auto text_is_valid = [](const std::string& value) {
    return valid_utf8(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(value.data()), value.size()});
  };
  if (statement.feed_uuid.empty() || statement.issuer.empty() ||
      statement.key_id.empty() || statement.feed_uuid.size() > 256 ||
      statement.issuer.size() > 256 || statement.key_id.size() > 128 ||
      !text_is_valid(statement.feed_uuid) ||
      !text_is_valid(statement.issuer) || !text_is_valid(statement.key_id) ||
      statement.not_before_seconds > statement.expires_at_seconds ||
      statement.issued_at_seconds > statement.expires_at_seconds) {
    return fail<std::vector<std::byte>>(ErrorCode::invalid_argument,
                                        "invalid W0 feed statement");
  }
  auto key = read_key(private_key_path, true);
  if (!key) return key.error();
  const auto protected_bytes = protected_header();
  const auto payload = encode_payload(statement);
  const auto to_sign = signature_structure(protected_bytes, payload);
  DigestContext context{EVP_MD_CTX_new(), &EVP_MD_CTX_free};
  std::array<std::byte, 64> signature{};
  std::size_t signature_size = signature.size();
  if (!context ||
      EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr,
                         key->get()) != 1 ||
      EVP_DigestSign(context.get(),
                     reinterpret_cast<unsigned char*>(signature.data()),
                     &signature_size,
                     reinterpret_cast<const unsigned char*>(to_sign.data()),
                     to_sign.size()) != 1 ||
      signature_size != signature.size()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::watermark_signature_invalid, "Ed25519 signing failed");
  }
  std::vector<std::byte> output;
  add_head(output, 4, 4);
  add_bytes(output, protected_bytes);
  add_head(output, 5, 1);
  add_uint(output, 4);
  std::vector<std::byte> key_id(statement.key_id.size());
  for (std::size_t index = 0; index < statement.key_id.size(); ++index) {
    key_id[index] = static_cast<std::byte>(statement.key_id[index]);
  }
  add_bytes(output, key_id);
  add_bytes(output, payload);
  add_bytes(output, signature);
  return output;
}

Result<StatementVerification> verify_statement(
    std::span<const std::byte> cose_sign1,
    const std::filesystem::path& public_key_path, std::int64_t at_seconds) {
  auto cose = decode_cose(cose_sign1);
  if (!cose) return cose.error();
  auto statement = decode_payload(cose->payload);
  if (!statement) return statement.error();
  if (cose->key_id.size() != statement->key_id.size() ||
      !std::equal(cose->key_id.begin(), cose->key_id.end(),
                  statement->key_id.begin(),
                  [](std::byte left, char right) {
                    return static_cast<unsigned char>(left) ==
                           static_cast<unsigned char>(right);
                  })) {
    return fail<StatementVerification>(
        ErrorCode::watermark_signature_invalid,
        "COSE key identifier does not match the W0 payload");
  }
  auto key = read_key(public_key_path, false);
  if (!key) return key.error();
  const auto to_verify =
      signature_structure(cose->protected_bytes, cose->payload);
  DigestContext context{EVP_MD_CTX_new(), &EVP_MD_CTX_free};
  if (!context ||
      EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr,
                           key->get()) != 1) {
    return fail<StatementVerification>(
        ErrorCode::watermark_signature_invalid,
        "Ed25519 verification could not initialize");
  }
  const auto valid = EVP_DigestVerify(
      context.get(),
      reinterpret_cast<const unsigned char*>(cose->signature.data()),
      cose->signature.size(),
      reinterpret_cast<const unsigned char*>(to_verify.data()),
      to_verify.size());
  StatementVerification verification;
  verification.statement = std::move(*statement);
  if (valid != 1) {
    verification.state = StatementState::invalid_signature;
  } else if (at_seconds < verification.statement.not_before_seconds) {
    verification.state = StatementState::not_yet_valid;
  } else if (at_seconds > verification.statement.expires_at_seconds) {
    verification.state = StatementState::expired;
  } else {
    verification.state = StatementState::valid;
  }
  return verification;
}

const char* statement_state_name(StatementState state) noexcept {
  switch (state) {
    case StatementState::valid: return "valid";
    case StatementState::invalid_signature: return "invalid_signature";
    case StatementState::not_yet_valid: return "not_yet_valid";
    case StatementState::expired: return "expired";
  }
  return "invalid_signature";
}

}  // namespace codec

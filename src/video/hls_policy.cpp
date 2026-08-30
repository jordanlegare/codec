#include "hls_policy.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace codec::profiles::video::detail {
namespace {

bool ascii_space(char ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string_view trim_ascii(std::string_view value) noexcept {
  while (!value.empty() && ascii_space(value.front())) value.remove_prefix(1);
  while (!value.empty() && ascii_space(value.back())) value.remove_suffix(1);
  return value;
}

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string_view byte_text(std::span<const std::byte> bytes) noexcept {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

Result<void> encrypted_hls_unsupported() {
  return fail(ErrorCode::model_incompatible, "encrypted HLS is unsupported");
}

Result<HlsOrigin> invalid_origin(ErrorCode code, std::string message) {
  return fail<HlsOrigin>(code, std::move(message));
}

struct CurlUrl {
  CurlUrl() = default;
  CURLU* value{curl_url()};
  ~CurlUrl() {
    if (value != nullptr) curl_url_cleanup(value);
  }

  CurlUrl(const CurlUrl&) = delete;
  CurlUrl& operator=(const CurlUrl&) = delete;
};

Result<std::string> url_part(CURLU* url, CURLUPart part,
                             std::string_view label) {
  char* raw = nullptr;
  const auto code = curl_url_get(url, part, &raw, 0);
  if (code != CURLUE_OK || raw == nullptr) {
    if (raw != nullptr) curl_free(raw);
    return fail<std::string>(ErrorCode::protocol,
                             "HLS URL is missing " + std::string{label});
  }
  std::string value{raw};
  curl_free(raw);
  return value;
}

}  // namespace

bool looks_like_hls_manifest(std::span<const std::byte> bytes) noexcept {
  auto text = byte_text(bytes);
  if (text.size() >= 3U &&
      static_cast<unsigned char>(text[0]) == 0xefU &&
      static_cast<unsigned char>(text[1]) == 0xbbU &&
      static_cast<unsigned char>(text[2]) == 0xbfU) {
    text.remove_prefix(3U);
  }
  while (!text.empty() && ascii_space(text.front())) text.remove_prefix(1U);
  constexpr std::string_view header = "#EXTM3U";
  if (!text.starts_with(header)) return false;
  return text.find("#EXT-X-", header.size()) != std::string_view::npos;
}

Result<void> validate_hls_manifest_security(std::span<const std::byte> bytes) {
  const auto text = byte_text(bytes);
  std::size_t line_start = 0U;
  while (line_start <= text.size()) {
    const auto line_end = text.find('\n', line_start);
    auto line = text.substr(
        line_start, line_end == std::string_view::npos
                        ? std::string_view::npos
                        : line_end - line_start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);

    constexpr std::string_view key_prefix = "#EXT-X-KEY:";
    if (line.starts_with(key_prefix)) {
      auto attributes = line.substr(key_prefix.size());
      bool method_seen = false;
      std::size_t field_start = 0U;
      bool quoted = false;
      for (std::size_t index = 0U; index <= attributes.size(); ++index) {
        const bool at_end = index == attributes.size();
        if (!at_end && attributes[index] == '"') quoted = !quoted;
        if (!at_end && (attributes[index] != ',' || quoted)) continue;

        auto field = trim_ascii(attributes.substr(field_start, index - field_start));
        const auto equal = field.find('=');
        if (equal != std::string_view::npos) {
          const auto name = trim_ascii(field.substr(0U, equal));
          const auto value = trim_ascii(field.substr(equal + 1U));
          if (name == "METHOD") {
            method_seen = true;
            if (value != "NONE") return encrypted_hls_unsupported();
          }
        }
        field_start = index + 1U;
      }
      if (!method_seen) return encrypted_hls_unsupported();
    }

    if (line_end == std::string_view::npos) break;
    line_start = line_end + 1U;
  }
  return {};
}

Result<HlsOrigin> parse_hls_http_origin(std::string_view uri) {
  if (uri.empty()) {
    return invalid_origin(ErrorCode::protocol, "HLS URL is empty");
  }
  CurlUrl parsed;
  if (parsed.value == nullptr) {
    return invalid_origin(ErrorCode::resource_exhausted,
                          "cannot allocate HLS URL parser");
  }
  const std::string owned{uri};
  if (curl_url_set(parsed.value, CURLUPART_URL, owned.c_str(), 0) != CURLUE_OK) {
    return invalid_origin(ErrorCode::protocol, "HLS URL is malformed");
  }

  auto scheme = url_part(parsed.value, CURLUPART_SCHEME, "scheme");
  if (!scheme) return scheme.error();
  *scheme = lower_ascii(std::move(*scheme));
  if (*scheme != "http" && *scheme != "https") {
    return invalid_origin(ErrorCode::protocol,
                          "HLS child URL must use HTTP or HTTPS");
  }

  auto host = url_part(parsed.value, CURLUPART_HOST, "host");
  if (!host) return host.error();
  *host = lower_ascii(std::move(*host));
  if (host->empty()) {
    return invalid_origin(ErrorCode::protocol, "HLS URL host is empty");
  }

  std::uint16_t port = *scheme == "https" ? 443U : 80U;
  char* raw_port = nullptr;
  const auto port_code = curl_url_get(parsed.value, CURLUPART_PORT, &raw_port, 0);
  if (port_code == CURLUE_OK && raw_port != nullptr) {
    try {
      const auto parsed_port = std::stoul(raw_port);
      curl_free(raw_port);
      raw_port = nullptr;
      if (parsed_port == 0UL || parsed_port > 65535UL) {
        return invalid_origin(ErrorCode::protocol,
                              "HLS URL port is outside the supported range");
      }
      port = static_cast<std::uint16_t>(parsed_port);
    } catch (...) {
      if (raw_port != nullptr) curl_free(raw_port);
      return invalid_origin(ErrorCode::protocol, "HLS URL port is invalid");
    }
  } else if (port_code != CURLUE_NO_PORT) {
    if (raw_port != nullptr) curl_free(raw_port);
    return invalid_origin(ErrorCode::protocol, "HLS URL port is invalid");
  }

  return HlsOrigin{
      .scheme = std::move(*scheme),
      .host = std::move(*host),
      .port = port,
  };
}

Result<void> require_same_hls_origin(const HlsOrigin& primary,
                                     std::string_view child_uri) {
  auto child = parse_hls_http_origin(child_uri);
  if (!child) return child.error();
  if (*child != primary) {
    return fail(ErrorCode::unauthorized_source,
                "HLS child URL is outside the primary origin");
  }
  return {};
}

StreamId derive_hls_child_stream_id(const StreamId& parent,
                                    std::size_t ordinal) {
  std::string identity = "codec.video.hls-resource.v1\n";
  identity += to_string(parent);
  identity += '\n';
  identity += std::to_string(ordinal);
  return derive_stream_id(identity);
}

std::string hls_child_label(std::string_view parent_label,
                            std::size_t ordinal) {
  std::ostringstream output;
  output << parent_label << ":hls-resource-" << std::setfill('0')
         << std::setw(4) << ordinal;
  return output.str();
}

}  // namespace codec::profiles::video::detail

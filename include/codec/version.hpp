#pragma once

#define CODEC_VERSION_MAJOR 0
#define CODEC_VERSION_MINOR 1
#define CODEC_VERSION_PATCH 0
#define CODEC_VERSION_STRING "0.1.0"

namespace codec {

inline constexpr int version_major = CODEC_VERSION_MAJOR;
inline constexpr int version_minor = CODEC_VERSION_MINOR;
inline constexpr int version_patch = CODEC_VERSION_PATCH;
inline constexpr const char* version_string = CODEC_VERSION_STRING;

}  // namespace codec

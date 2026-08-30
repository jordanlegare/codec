#ifndef CODEC_CODEC_C_H
#define CODEC_CODEC_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CODEC_ABI_VERSION 1U

typedef enum codec_status {
  CODEC_STATUS_OK = 0,
  CODEC_STATUS_INVALID_ARGUMENT = 1,
  CODEC_STATUS_IO = 2,
  CODEC_STATUS_CORRUPT = 3,
  CODEC_STATUS_NETWORK = 4,
  CODEC_STATUS_UNAVAILABLE = 5,
  CODEC_STATUS_UNAUTHORIZED = 6,
  CODEC_STATUS_RESOURCE_EXHAUSTED = 7,
  CODEC_STATUS_DECODE = 8,
  CODEC_STATUS_INTERNAL = 255
} codec_status_t;

typedef struct codec_error {
  size_t size;
  uint32_t abi_version;
  int32_t code;
  int32_t retryable;
  char* message;
} codec_error_t;

#define CODEC_ERROR_INIT \
  { sizeof(codec_error_t), CODEC_ABI_VERSION, 0, 0, NULL }

typedef struct codec_engine_config {
  size_t size;
  uint32_t abi_version;
  size_t capture_chunk_bytes;
  uint64_t maximum_feed_bytes;
  uint32_t maximum_redirects;
  int32_t deny_private_network;
} codec_engine_config_t;

#define CODEC_ENGINE_CONFIG_INIT                                             \
  {                                                                         \
    sizeof(codec_engine_config_t), CODEC_ABI_VERSION, 256U * 1024U,         \
        16ULL * 1024ULL * 1024ULL * 1024ULL, 5U, 1                          \
  }

typedef struct codec_verification_report {
  size_t size;
  uint32_t abi_version;
  int32_t ok;
  int32_t finalized;
  uint64_t committed_records;
  uint64_t verified_payload_bytes;
  uint64_t valid_prefix_bytes;
  uint64_t file_bytes;
} codec_verification_report_t;

#define CODEC_VERIFICATION_REPORT_INIT                                      \
  {                                                                         \
    sizeof(codec_verification_report_t), CODEC_ABI_VERSION, 0, 0, 0, 0, 0, \
        0                                                                   \
  }

typedef struct codec_engine codec_engine_t;
typedef struct codec_archive codec_archive_t;

codec_status_t codec_engine_create(const codec_engine_config_t* config,
                                   codec_engine_t** output,
                                   codec_error_t* error);
codec_status_t codec_engine_record_file(codec_engine_t* engine,
                                        const char* input_path,
                                        const char* feed_label,
                                        const char* archive_path,
                                        codec_error_t* error);
void codec_engine_destroy(codec_engine_t* engine);

codec_status_t codec_archive_open(const char* archive_path,
                                  codec_archive_t** output,
                                  codec_error_t* error);
codec_status_t codec_archive_verify(codec_archive_t* archive,
                                    codec_verification_report_t* report,
                                    codec_error_t* error);
codec_status_t codec_archive_extract_feed(codec_archive_t* archive,
                                          const char* feed_label,
                                          const char* output_path,
                                          codec_error_t* error);
void codec_archive_destroy(codec_archive_t* archive);

void codec_error_clear(codec_error_t* error);
const char* codec_version_string(void);

#ifdef __cplusplus
}
#endif

#endif

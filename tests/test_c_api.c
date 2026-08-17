#include <codec/codec_c.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char* message, const codec_error_t* error) {
  fprintf(stderr, "%s: %s\n", message,
          error != NULL && error->message != NULL ? error->message : "no error");
  return 1;
}

int main(void) {
  const char* input_path = "/tmp/codec-c-api-input.bin";
  const char* archive_path = "/tmp/codec-c-api.coda";
  const char* output_path = "/tmp/codec-c-api-output.bin";
  const char expected[] = "C ABI exact bytes\n";
  remove(archive_path);
  remove(output_path);
  FILE* input = fopen(input_path, "wb");
  if (input == NULL) return 2;
  fwrite(expected, 1, sizeof(expected) - 1, input);
  fclose(input);

  codec_error_t error = CODEC_ERROR_INIT;
  codec_engine_t* engine = NULL;
  codec_engine_config_t config = CODEC_ENGINE_CONFIG_INIT;
  if (codec_engine_create(&config, &engine, &error) != CODEC_STATUS_OK) {
    return fail("engine create", &error);
  }
  if (codec_engine_record_file(engine, input_path, "c-feed", archive_path,
                               &error) != CODEC_STATUS_OK) {
    codec_engine_destroy(engine);
    return fail("record file", &error);
  }
  codec_engine_destroy(engine);

  codec_archive_t* archive = NULL;
  if (codec_archive_open(archive_path, &archive, &error) != CODEC_STATUS_OK) {
    return fail("archive open", &error);
  }
  codec_verification_report_t report = CODEC_VERIFICATION_REPORT_INIT;
  if (codec_archive_verify(archive, &report, &error) != CODEC_STATUS_OK ||
      report.ok == 0 || report.committed_records < 3) {
    codec_archive_destroy(archive);
    return fail("archive verify", &error);
  }
  if (codec_archive_extract_feed(archive, "c-feed", output_path, &error) !=
      CODEC_STATUS_OK) {
    codec_archive_destroy(archive);
    return fail("extract feed", &error);
  }
  codec_archive_destroy(archive);
  codec_error_clear(&error);

  FILE* output = fopen(output_path, "rb");
  char actual[sizeof(expected)] = {0};
  if (output == NULL ||
      fread(actual, 1, sizeof(expected) - 1, output) != sizeof(expected) - 1 ||
      memcmp(actual, expected, sizeof(expected) - 1) != 0) {
    if (output != NULL) fclose(output);
    return 3;
  }
  fclose(output);
  remove(input_path);
  remove(archive_path);
  remove(output_path);
  return 0;
}

cmake_minimum_required(VERSION 3.20)

project(CODEC VERSION 0.3.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(CODEC_BUILD_TESTS "Build CODEC tests" ON)
option(CODEC_BUILD_EXAMPLES "Build CODEC examples" ON)
option(CODEC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CODEC_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(CODEC_ENABLE_FFMPEG_VIDEO
  "Enable the optional FFmpeg Video Profile ingest backend" ON)
set(CODEC_ONNXRUNTIME_ROOT "" CACHE PATH
    "Optional ONNX Runtime distribution root for the Audio CPU backend")

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)

set(CODEC_HAS_FFMPEG_VIDEO OFF)
if(CODEC_ENABLE_FFMPEG_VIDEO)
  pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
  pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
  pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
  pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
  pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
  set(CODEC_HAS_FFMPEG_VIDEO ON)
endif()

set_source_files_properties(src/video/ffmpeg_ingest.cpp PROPERTIES
  COMPILE_DEFINITIONS "ingest_video_ffmpeg=ingest_video_ffmpeg_direct")

add_library(codec_core
  src/archive/archive.cpp
  src/archive/follow.cpp
  src/audio/flac_decoder.cpp
  src/audio/flac_encoder.cpp
  src/audio/offline_pcm16_separation.cpp
  src/audio/onnx_cpu_separation.cpp
  src/audio/pcm16_flac_export.cpp
  src/audio/pcm16_flac_ingest.cpp
  src/audio/pcm16_ingest.cpp
  src/audio/separation_model_bundle.cpp
  src/audio/pcm16_state.cpp
  src/audio/pcm16_state_reader.cpp
  src/audio/pcm16_wav_export.cpp
  src/audio/wav.cpp
  src/capi/codec_c.cpp
  src/capture/capture.cpp
  src/core/engine.cpp
  src/core/feed_descriptor.cpp
  src/core/processing.cpp
  src/core/sha256.cpp
  src/core/stream_continuity.cpp
  src/core/stream_provenance.cpp
  src/distributed/identity.cpp
  src/distributed/location_index.cpp
  src/distributed/partition.cpp
  src/distributed/remote_worker.cpp
  src/distributed/retrieval.cpp
  src/distributed/scheduler.cpp
  src/distributed/wire.cpp
  src/distributed/worker.cpp
  src/inference/backend.cpp
  src/transport/multiplex.cpp
  src/transport/recovery.cpp
  src/transport/streaming_repair.cpp
  src/transport/xor_recovery.cpp
  src/video/audio_state_reader.cpp
  src/video/encoded_audio_capture.cpp
  src/video/encoded_audio_state.cpp
  src/video/encoded_audio_state_reader.cpp
  src/video/encoded_video_state.cpp
  src/video/encoded_video_state_reader.cpp
  src/video/ffmpeg_export_dispatch.cpp
  src/video/ffmpeg_ingest.cpp
  src/video/ffmpeg_ingest_dispatch.cpp
  src/video/hls_policy.cpp
  src/video/frame_state.cpp
  src/video/frame_state_reader.cpp
)
add_library(codec::codec ALIAS codec_core)
set_target_properties(codec_core PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
  EXPORT_NAME codec
  OUTPUT_NAME codec
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(codec_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(codec_core PUBLIC OpenSSL::Crypto CURL::libcurl PkgConfig::FLAC)
target_link_libraries(codec_core PRIVATE ${CMAKE_DL_LIBS})

if(CODEC_HAS_FFMPEG_VIDEO)
  target_compile_definitions(codec_core PRIVATE CODEC_HAS_FFMPEG_VIDEO=1)
  target_link_libraries(codec_core PUBLIC
    PkgConfig::AVFORMAT PkgConfig::AVCODEC PkgConfig::AVUTIL
    PkgConfig::SWSCALE PkgConfig::SWRESAMPLE)
  message(STATUS "CODEC FFmpeg Video Profile ingest backend: enabled")
else()
  message(STATUS "CODEC FFmpeg Video Profile ingest backend: unavailable (set CODEC_ENABLE_FFMPEG_VIDEO=ON to enable)")
endif()

set(CODEC_HAS_ONNXRUNTIME OFF)
if(CODEC_ONNXRUNTIME_ROOT)
  find_path(CODEC_ONNXRUNTIME_INCLUDE_DIR NAMES onnxruntime_c_api.h
    PATHS "${CODEC_ONNXRUNTIME_ROOT}/include" NO_DEFAULT_PATH)
  if(WIN32)
    find_file(CODEC_ONNXRUNTIME_LIBRARY NAMES onnxruntime.dll
      PATHS "${CODEC_ONNXRUNTIME_ROOT}/lib" "${CODEC_ONNXRUNTIME_ROOT}/bin" NO_DEFAULT_PATH)
  else()
    find_library(CODEC_ONNXRUNTIME_LIBRARY NAMES onnxruntime
      PATHS "${CODEC_ONNXRUNTIME_ROOT}/lib" NO_DEFAULT_PATH)
  endif()
  if(NOT CODEC_ONNXRUNTIME_INCLUDE_DIR OR NOT CODEC_ONNXRUNTIME_LIBRARY)
    message(FATAL_ERROR "CODEC_ONNXRUNTIME_ROOT must contain include/onnxruntime_c_api.h and the ONNX Runtime shared library")
  endif()
  set(CODEC_HAS_ONNXRUNTIME ON)
  target_include_directories(codec_core PRIVATE ${CODEC_ONNXRUNTIME_INCLUDE_DIR})
  target_compile_definitions(codec_core PRIVATE CODEC_HAS_ONNXRUNTIME=1)
  message(STATUS "CODEC Audio ONNX Runtime CPU backend: enabled")
else()
  message(STATUS "CODEC Audio ONNX Runtime CPU backend: unavailable (set CODEC_ONNXRUNTIME_ROOT to enable)")
endif()

function(codec_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(CODEC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(CODEC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(codec_enable_sanitizers target)
  if(CODEC_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

codec_enable_warnings(codec_core)
codec_enable_sanitizers(codec_core)

add_executable(codec_cli src/cli/main.cpp)
set_target_properties(codec_cli PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO OUTPUT_NAME codec)
target_include_directories(codec_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(codec_cli PRIVATE CODEC_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(codec_cli PRIVATE codec::codec)
codec_enable_warnings(codec_cli)
codec_enable_sanitizers(codec_cli)

if(CODEC_BUILD_TESTS)
  enable_testing()
  add_executable(codec_tests
    tests/test_main.cpp
    tests/test_archive.cpp
    tests/test_archive_follow.cpp
    tests/test_audio_model_bundle.cpp
    tests/test_audio_onnx_cpu_runtime.cpp
    tests/test_audio_profile.cpp
    tests/test_audio_state_reader.cpp
    tests/test_audio_export.cpp
    tests/test_audio_flac_export.cpp
    tests/test_audio_flac_ingest.cpp
    tests/test_audio_flac_ingest_validation.cpp
    tests/test_audio_offline_separation.cpp
    tests/test_engine.cpp
    tests/test_engine_concurrency.cpp
    tests/test_distributed_location_index.cpp
    tests/test_distributed_partition.cpp
    tests/test_distributed_remote_worker.cpp
    tests/test_distributed_wire.cpp
    tests/test_distributed_wire_strict.cpp
    tests/test_distributed_retrieval.cpp
    tests/test_distributed_scheduler.cpp
    tests/test_distributed_worker.cpp
    tests/test_processing.cpp
    tests/test_exporter.cpp
    tests/test_stream.cpp
    tests/test_transport_multiplex.cpp
    tests/test_transport_recovery.cpp
    tests/test_transport_recovery_integration.cpp
    tests/test_transport_streaming_repair.cpp
    tests/test_transport_xor_recovery.cpp
    tests/test_video_audio_state_reader.cpp
    tests/test_video_audio_state_reader_strict.cpp
    tests/test_video_audio_review_regressions.cpp
    tests/test_video_encoded_audio_state.cpp
    tests/test_video_encoded_audio_state_reader.cpp
    tests/test_video_encoded_audio_capture.cpp
    tests/test_video_encoded_video_state_reader.cpp
    tests/test_video_export.cpp
    tests/test_video_ffmpeg_audio.cpp
    tests/test_video_ffmpeg_ingest.cpp
    tests/test_video_ffmpeg_ingest_limits.cpp
    tests/test_video_ffmpeg_layouts.cpp
    tests/test_video_hls_ingest.cpp
    tests/test_video_hls_failures.cpp
    tests/test_video_hls_policy.cpp
    tests/test_video_profile.cpp
    tests/test_video_state_reader.cpp)
  set_target_properties(codec_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
  target_include_directories(codec_tests PRIVATE tests)
  target_link_libraries(codec_tests PRIVATE codec::codec PkgConfig::FLAC)
  if(CODEC_HAS_FFMPEG_VIDEO)
    target_compile_definitions(codec_tests PRIVATE CODEC_TEST_HAS_FFMPEG_VIDEO=1)
  endif()
  if(CODEC_HAS_ONNXRUNTIME)
    target_compile_definitions(codec_tests PRIVATE CODEC_TEST_ONNXRUNTIME_LIBRARY="${CODEC_ONNXRUNTIME_LIBRARY}")
  endif()
  codec_enable_warnings(codec_tests)
  codec_enable_sanitizers(codec_tests)
  if(CODEC_ENABLE_SANITIZERS AND CODEC_HAS_ONNXRUNTIME AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_test(NAME codec-unit COMMAND codec_tests --exclude-prefix audio_onnx_cpu_runtime_)
    add_test(NAME codec-onnx-runtime COMMAND codec_tests --include-prefix audio_onnx_cpu_runtime_)
    set_tests_properties(codec-onnx-runtime PROPERTIES ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan_onnxruntime.supp:print_suppressions=1")
  else()
    add_test(NAME codec-unit COMMAND codec_tests)
  endif()
  add_executable(codec_c_api_test tests/test_c_api.c)
  set_target_properties(codec_c_api_test PROPERTIES LINKER_LANGUAGE CXX)
  target_link_libraries(codec_c_api_test PRIVATE codec::codec)
  codec_enable_warnings(codec_c_api_test)
  codec_enable_sanitizers(codec_c_api_test)
  add_test(NAME codec-c-api COMMAND codec_c_api_test)
  add_test(NAME codec-cli-integration COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_integration.sh $<TARGET_FILE:codec_cli> ${PROJECT_VERSION})
  add_test(NAME codec-ai-contract COMMAND ${CMAKE_COMMAND} -DCODEC_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_contract.cmake)
endif()

if(CODEC_BUILD_EXAMPLES)
  add_executable(codec_capture_example examples/capture.cpp)
  set_target_properties(codec_capture_example PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
  target_link_libraries(codec_capture_example PRIVATE codec::codec)
  codec_enable_warnings(codec_capture_example)
  codec_enable_sanitizers(codec_capture_example)
endif()

install(TARGETS codec_core codec_cli EXPORT codec-targets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/codec DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
set(CODEC_PACKAGE_HAS_FFMPEG_VIDEO ${CODEC_HAS_FFMPEG_VIDEO})
configure_package_config_file(cmake/codec-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/codec-config.cmake INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/codec)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/codec-config-version.cmake VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/codec-config.cmake ${CMAKE_CURRENT_BINARY_DIR}/codec-config-version.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/codec)
install(EXPORT codec-targets FILE codec-targets.cmake NAMESPACE codec:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/codec)

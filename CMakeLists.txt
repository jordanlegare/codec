cmake_minimum_required(VERSION 3.20)

project(CODEC VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(CODEC_BUILD_TESTS "Build CODEC tests" ON)
option(CODEC_BUILD_EXAMPLES "Build CODEC examples" ON)
option(CODEC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CODEC_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_package(CURL REQUIRED)

add_library(codec_core
  src/archive/archive.cpp
  src/audio/wav.cpp
  src/capi/codec_c.cpp
  src/capture/capture.cpp
  src/core/engine.cpp
  src/core/feed_descriptor.cpp
  src/core/processing.cpp
  src/core/sha256.cpp
  src/core/stream_continuity.cpp
  src/core/stream_provenance.cpp
  src/inference/backend.cpp
  src/watermark/carrier.cpp
  src/watermark/statement.cpp
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
target_include_directories(codec_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(codec_core PUBLIC OpenSSL::Crypto CURL::libcurl)

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
set_target_properties(codec_cli PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
  OUTPUT_NAME codec
)
target_include_directories(codec_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(codec_cli PRIVATE codec::codec)
codec_enable_warnings(codec_cli)
codec_enable_sanitizers(codec_cli)

if(CODEC_BUILD_TESTS)
  enable_testing()
  add_executable(codec_tests
    tests/test_main.cpp
    tests/test_archive.cpp
    tests/test_engine.cpp
    tests/test_processing.cpp
    tests/test_stream.cpp
    tests/test_statement.cpp
    tests/test_watermark.cpp
  )
  set_target_properties(codec_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )
  target_include_directories(codec_tests PRIVATE tests)
  target_link_libraries(codec_tests PRIVATE codec::codec)
  codec_enable_warnings(codec_tests)
  codec_enable_sanitizers(codec_tests)
  add_test(NAME codec-unit COMMAND codec_tests)

  add_executable(codec_c_api_test tests/test_c_api.c)
  set_target_properties(codec_c_api_test PROPERTIES LINKER_LANGUAGE CXX)
  target_link_libraries(codec_c_api_test PRIVATE codec::codec)
  codec_enable_warnings(codec_c_api_test)
  codec_enable_sanitizers(codec_c_api_test)
  add_test(NAME codec-c-api COMMAND codec_c_api_test)

  add_test(NAME codec-cli-integration
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_integration.sh
                 $<TARGET_FILE:codec_cli>)

  add_test(NAME codec-ai-contract
    COMMAND ${CMAKE_COMMAND}
            -DCODEC_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_contract.cmake)
endif()

if(CODEC_BUILD_EXAMPLES)
  add_executable(codec_capture_example examples/capture.cpp)
  set_target_properties(codec_capture_example PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )
  target_link_libraries(codec_capture_example PRIVATE codec::codec)
  codec_enable_warnings(codec_capture_example)
  codec_enable_sanitizers(codec_capture_example)
endif()

install(TARGETS codec_core codec_cli
  EXPORT codec-targets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/codec DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
  cmake/codec-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/codec-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/codec
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/codec-config-version.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/codec-config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/codec-config-version.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/codec
)
install(EXPORT codec-targets
  FILE codec-targets.cmake
  NAMESPACE codec::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/codec
)

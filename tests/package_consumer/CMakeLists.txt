cmake_minimum_required(VERSION 3.20)
project(codec_package_consumer LANGUAGES CXX)

find_package(codec 0.1 CONFIG REQUIRED)

add_executable(codec_package_consumer main.cpp)
target_compile_features(codec_package_consumer PRIVATE cxx_std_20)
target_link_libraries(codec_package_consumer PRIVATE codec::codec)

add_executable(codec_package_remote_consumer remote_worker.cpp)
target_compile_features(codec_package_remote_consumer PRIVATE cxx_std_20)
target_link_libraries(codec_package_remote_consumer PRIVATE codec::codec)
add_custom_command(TARGET codec_package_remote_consumer POST_BUILD
  COMMAND $<TARGET_FILE:codec_package_remote_consumer>
  VERBATIM)
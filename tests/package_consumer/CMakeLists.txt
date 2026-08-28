cmake_minimum_required(VERSION 3.20)
project(codec_package_consumer LANGUAGES CXX)

find_package(codec 0.1 CONFIG REQUIRED)

add_executable(codec_package_consumer main.cpp)
target_compile_features(codec_package_consumer PRIVATE cxx_std_20)
target_link_libraries(codec_package_consumer PRIVATE codec::codec)

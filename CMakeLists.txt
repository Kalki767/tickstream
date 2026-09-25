cmake_minimum_required(VERSION 3.24)
project(tickstream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++17, not -std=gnu++17
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # compile_commands.json for clangd / IDEs

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# System packages (apt): Boost (Beast + Asio are header-only), OpenSSL.
# Fetched at configure time: nlohmann/json, spdlog, GoogleTest. Pinning exact
# versions here means every machine builds against the same code.
# SYSTEM marks their headers as system headers, so -Wall -Wextra only reports
# warnings in *our* code.
# ---------------------------------------------------------------------------
find_package(Boost 1.74 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)

FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  SYSTEM)

FetchContent_Declare(spdlog
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz
  SYSTEM)

FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  SYSTEM)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(json spdlog googletest)

# Warnings for our own targets only.
set(TICKSTREAM_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

# ---------------------------------------------------------------------------
# tickstream_core: pure logic (parsing, backoff). No networking, so the unit
# tests can link it without pulling in Boost or OpenSSL.
# ---------------------------------------------------------------------------
add_library(tickstream_core
  src/binance.cpp
  src/backoff.cpp)
target_include_directories(tickstream_core PUBLIC include)
target_link_libraries(tickstream_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(tickstream_core PRIVATE ${TICKSTREAM_WARNINGS})

# ---------------------------------------------------------------------------
# tickstream: the service binary.
# ---------------------------------------------------------------------------
add_executable(tickstream
  src/main.cpp
  src/feed_client.cpp)
target_link_libraries(tickstream PRIVATE
  tickstream_core
  Boost::headers
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
  spdlog::spdlog)
target_compile_options(tickstream PRIVATE ${TICKSTREAM_WARNINGS})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(tickstream_tests
  tests/binance_test.cpp
  tests/backoff_test.cpp)
target_link_libraries(tickstream_tests PRIVATE tickstream_core GTest::gtest_main)
target_compile_options(tickstream_tests PRIVATE ${TICKSTREAM_WARNINGS})

include(GoogleTest)
gtest_discover_tests(tickstream_tests)

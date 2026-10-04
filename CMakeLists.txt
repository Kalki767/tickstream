cmake_minimum_required(VERSION 3.25)
project(tickstream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++20, not -std=gnu++20
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # compile_commands.json for clangd / IDEs

# Default to an optimized build with symbols (benchmarks and profiling).
# Set as a cache variable so it is visible to tools and later configures.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Sanitizers: -DTICKSTREAM_SANITIZE=address,undefined  or  =thread
# Use a separate build directory per sanitizer (ASan and TSan can't be
# combined). Applied before the dependencies are fetched so they are
# instrumented too: code TSan can't see is a classic source of false reports.
# Never benchmark these builds.
# ---------------------------------------------------------------------------
set(TICKSTREAM_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")
if(TICKSTREAM_SANITIZE)
  if(TICKSTREAM_SANITIZE MATCHES "thread" AND TICKSTREAM_SANITIZE MATCHES "address")
    message(FATAL_ERROR "ThreadSanitizer and AddressSanitizer can't be combined")
  endif()
  # -fno-sanitize-recover: a UBSan finding aborts, so tests can't pass over it.
  add_compile_options(-fsanitize=${TICKSTREAM_SANITIZE} -fno-sanitize-recover=all
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${TICKSTREAM_SANITIZE})
  if(TICKSTREAM_SANITIZE MATCHES "thread")
    # Asio's std_fenced_block uses atomic_thread_fence, which TSan can't model,
    # and GCC warns about every inlined use. Asio runs on a single thread here;
    # the only cross-thread structure (BoundedQueue) uses a mutex, which TSan
    # does understand.
    add_compile_options(-Wno-tsan)
    # GCC 13's TSan runtime aborts ("unexpected memory mapping") under the
    # high mmap ASLR entropy of recent kernels. Running the binary with ASLR
    # disabled for that one process (setarch -R, no root needed) avoids it;
    # ctest and gtest_discover_tests use this launcher.
    set(TICKSTREAM_TEST_LAUNCHER setarch ${CMAKE_SYSTEM_PROCESSOR} -R)
  endif()
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# System packages (apt): Boost (Beast + Asio are header-only), OpenSSL, libpq.
# Fetched at configure time: nlohmann/json, spdlog, GoogleTest, libpqxx.
# Pinning exact versions here means every machine builds against the same code.
# SYSTEM marks their headers as system headers, so -Wall -Wextra only reports
# warnings in *our* code.
# ---------------------------------------------------------------------------
find_package(Boost 1.80 REQUIRED)  # 1.80+: mature awaitable + as_tuple support
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PostgreSQL REQUIRED)  # libpq (apt: libpq-dev)

option(TICKSTREAM_BUILD_TESTS "Build the unit tests (fetches GoogleTest)" ON)
option(TICKSTREAM_BUILD_BENCH "Build the micro-benchmarks (fetches Google Benchmark)" ON)

include(FetchContent)

FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  SYSTEM)

FetchContent_Declare(spdlog
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz
  SYSTEM)

# libpqxx is built from source rather than taken from apt: its headers pick
# different overloads under C++20 (std::source_location) than the C++17 the
# distro library was compiled with, which fails at link time. libpqxx needs
# to be built with the same C++ standard as the code using it.
FetchContent_Declare(libpqxx
  URL https://github.com/jtv/libpqxx/archive/refs/tags/7.8.1.tar.gz
  SYSTEM)
set(SKIP_BUILD_TEST ON CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
set(BUILD_TEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(json spdlog libpqxx)

if(TICKSTREAM_BUILD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    SYSTEM)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

if(TICKSTREAM_BUILD_BENCH)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.0.tar.gz
    SYSTEM)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
endif()

# Warnings for our own targets only.
set(TICKSTREAM_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

# ---------------------------------------------------------------------------
# tickstream_core: pure logic (parsing, backoff, gap detection, options,
# queue, replay, writer loop). No networking or database, so the unit
# tests can link it without pulling in Boost or OpenSSL.
# ---------------------------------------------------------------------------
add_library(tickstream_core
  src/binance.cpp
  src/backoff.cpp
  src/gap_detector.cpp
  src/options.cpp
  src/replay.cpp
  src/sql.cpp
  src/writer_loop.cpp)
target_include_directories(tickstream_core PUBLIC include)
target_link_libraries(tickstream_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(tickstream_core PRIVATE ${TICKSTREAM_WARNINGS})

# ---------------------------------------------------------------------------
# tickstream: the service binary.
# ---------------------------------------------------------------------------
add_executable(tickstream
  src/main.cpp
  src/feed_client.cpp
  src/pg_writer.cpp)
target_link_libraries(tickstream PRIVATE
  tickstream_core
  pqxx
  Boost::headers
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
  spdlog::spdlog)
target_compile_options(tickstream PRIVATE ${TICKSTREAM_WARNINGS})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(TICKSTREAM_BUILD_TESTS)
  enable_testing()
  add_executable(tickstream_tests
    tests/binance_test.cpp
    tests/backoff_test.cpp
    tests/gap_detector_test.cpp
    tests/options_test.cpp
    tests/bounded_queue_test.cpp
    tests/pipeline_test.cpp
    tests/flush_policy_test.cpp
    tests/sql_test.cpp
    tests/parser_equivalence_test.cpp)
  target_link_libraries(tickstream_tests PRIVATE tickstream_core GTest::gtest_main)
  target_compile_definitions(tickstream_tests PRIVATE
    TICKSTREAM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(tickstream_tests PRIVATE ${TICKSTREAM_WARNINGS})
  if(TICKSTREAM_TEST_LAUNCHER)
    set_target_properties(tickstream_tests PROPERTIES
      CROSSCOMPILING_EMULATOR "${TICKSTREAM_TEST_LAUNCHER}")
  endif()

  include(GoogleTest)
  gtest_discover_tests(tickstream_tests)
endif()

# ---------------------------------------------------------------------------
# Micro-benchmarks (Google Benchmark). Has its own counting operator new, so
# it reports heap allocations per operation.
# ---------------------------------------------------------------------------
if(TICKSTREAM_BUILD_BENCH)
  add_executable(tickstream_bench bench/micro/micro_bench.cpp)
  target_link_libraries(tickstream_bench PRIVATE tickstream_core benchmark::benchmark)
  target_compile_options(tickstream_bench PRIVATE ${TICKSTREAM_WARNINGS})
  target_compile_definitions(tickstream_bench PRIVATE
    TICKSTREAM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()

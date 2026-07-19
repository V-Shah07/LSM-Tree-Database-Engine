cmake_minimum_required(VERSION 3.16)
project(lsmdb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
add_compile_options(-Wall -Wextra)

# ---- Core engine library ----------------------------------------------------
# Sources are added phase by phase as the engine grows.
add_library(lsmdb
  src/skiplist.cpp
  src/crc32.cpp
  src/hash.cpp
  src/bloom.cpp
  src/cache.cpp
  src/sstable.cpp
  src/merging_iterator.cpp
  src/wal.cpp
  src/db.cpp
)
target_include_directories(lsmdb PUBLIC ${CMAKE_SOURCE_DIR}/include)

find_package(Threads REQUIRED)
target_link_libraries(lsmdb PUBLIC Threads::Threads)

# ---- Tests (GoogleTest) -----------------------------------------------------
enable_testing()
find_package(GTest QUIET)

add_executable(lsmdb_tests
  tests/test_skiplist.cpp
  tests/test_crc32.cpp
  tests/test_bloom.cpp
  tests/test_sstable.cpp
  tests/test_recovery.cpp
  tests/test_compaction.cpp
  tests/test_cache.cpp
  tests/test_scan.cpp
)
target_link_libraries(lsmdb_tests PRIVATE lsmdb)
if(GTest_FOUND)
  target_link_libraries(lsmdb_tests PRIVATE GTest::gtest GTest::gtest_main)
else()
  # Fall back to system libraries (Ubuntu ships libgtest.a directly).
  target_link_libraries(lsmdb_tests PRIVATE gtest gtest_main)
endif()

include(GoogleTest)
gtest_discover_tests(lsmdb_tests)

# ---- Benchmarks / measurement drivers ---------------------------------------
# Standalone drivers that produce the numbers behind the resume bullets.
add_executable(bench_bloom bench/bench_bloom.cpp)
target_link_libraries(bench_bloom PRIVATE lsmdb)

add_executable(bench_compaction bench/bench_compaction.cpp)
target_link_libraries(bench_compaction PRIVATE lsmdb)

add_executable(bench_cache bench/bench_cache.cpp)
target_link_libraries(bench_cache PRIVATE lsmdb)

# Google Benchmark microbenchmarks (throughput + latency percentiles).
find_library(BENCHMARK_LIB benchmark)
if(BENCHMARK_LIB)
  add_executable(bench_gbench bench/bench_gbench.cpp)
  target_link_libraries(bench_gbench PRIVATE lsmdb ${BENCHMARK_LIB} Threads::Threads)
  message(STATUS "Google Benchmark found: building bench_gbench")
else()
  message(STATUS "Google Benchmark not found: skipping bench_gbench")
endif()

# The RocksDB comparison benchmark only builds if RocksDB is present.
find_library(ROCKSDB_LIB rocksdb)
find_path(ROCKSDB_INCLUDE rocksdb/db.h)
if(ROCKSDB_LIB AND ROCKSDB_INCLUDE)
  add_executable(bench_vs_rocksdb bench/bench_vs_rocksdb.cpp)
  target_include_directories(bench_vs_rocksdb PRIVATE ${ROCKSDB_INCLUDE})
  target_link_libraries(bench_vs_rocksdb PRIVATE lsmdb ${ROCKSDB_LIB}
    Threads::Threads ${CMAKE_DL_LIBS})
  message(STATUS "RocksDB found: building bench_vs_rocksdb")
else()
  message(STATUS "RocksDB not found: skipping bench_vs_rocksdb")
endif()

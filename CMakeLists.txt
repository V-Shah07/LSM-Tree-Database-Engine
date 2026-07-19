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
)
target_include_directories(lsmdb PUBLIC ${CMAKE_SOURCE_DIR}/include)

find_package(Threads REQUIRED)
target_link_libraries(lsmdb PUBLIC Threads::Threads)

# ---- Tests (GoogleTest) -----------------------------------------------------
enable_testing()
find_package(GTest QUIET)

add_executable(lsmdb_tests
  tests/test_skiplist.cpp
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

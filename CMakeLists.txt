cmake_minimum_required(VERSION 3.20)
project(lsmdb CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(lsmdb src/main.cpp)

target_include_directories(lsmdb PRIVATE include)
cmake_minimum_required(VERSION 3.20)
project(PeerCore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(PEERCORE_BUILD_TESTS "Build tests" ON)

# Find dependencies
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Libsodium REQUIRED)

# Core library
add_subdirectory(src)

# Tests
if(PEERCORE_BUILD_TESTS)
  enable_testing()
  list(APPEND CMAKE_CTEST_ARGUMENTS "--verbose")
  add_subdirectory(tests)
endif()

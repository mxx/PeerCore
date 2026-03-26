cmake_minimum_required(VERSION 3.20)
project(PeerCore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(PEERCORE_BUILD_TESTS    "Build tests"             ON)
option(PEERCORE_COVERAGE       "Enable coverage reports" OFF)

# Find dependencies
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Libsodium REQUIRED)

# Coverage instrumentation
if(PEERCORE_COVERAGE)
  if(NOT PEERCORE_BUILD_TESTS)
    message(FATAL_ERROR "PEERCORE_COVERAGE requires PEERCORE_BUILD_TESTS=ON")
  endif()
  find_program(LCOV_BIN    lcov    REQUIRED)
  find_program(GENHTML_BIN genhtml REQUIRED)
  add_compile_options(--coverage -O0 -fno-inline)
  add_link_options(--coverage)
  message(STATUS "Coverage enabled: lcov=${LCOV_BIN}  genhtml=${GENHTML_BIN}")
endif()

# Core library
add_subdirectory(src)

# Tests
if(PEERCORE_BUILD_TESTS)
  enable_testing()
  list(APPEND CMAKE_CTEST_ARGUMENTS "--verbose")
  add_subdirectory(tests)
endif()

# Coverage target: cmake --build build --target coverage
if(PEERCORE_COVERAGE)
  set(COVERAGE_DIR "${CMAKE_BINARY_DIR}/coverage")

  add_custom_target(coverage
    # 1. Reset counters
    COMMAND ${LCOV_BIN} --zerocounters --directory ${CMAKE_BINARY_DIR}

    # 2. Run tests
    COMMAND ${CMAKE_CTEST_COMMAND} --verbose

    # 3. Capture coverage data (src only, exclude tests / deps)
    COMMAND ${LCOV_BIN}
            --capture
            --directory ${CMAKE_BINARY_DIR}
            --output-file ${COVERAGE_DIR}/coverage.info
            --ignore-errors source,inconsistent,unsupported,format,count

    # 4-6. Filter noise, generate HTML, print summary (via shell script for
    #      reliable error suppression with Apple LLVM gcov compatibility)
    COMMAND ${CMAKE_SOURCE_DIR}/cmake/coverage.sh
            ${LCOV_BIN} ${GENHTML_BIN}
            ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR} ${COVERAGE_DIR}

    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating coverage report → ${COVERAGE_DIR}/html/index.html"
  )

  # Create output dir before the target runs
  file(MAKE_DIRECTORY ${COVERAGE_DIR})
endif()

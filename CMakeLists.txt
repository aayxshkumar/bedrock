cmake_minimum_required(VERSION 3.24)
project(bedrock LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(BEDROCK_BUILD_TESTS "Build unit tests" ON)
# e.g. -DBEDROCK_SANITIZERS=address,undefined
set(BEDROCK_SANITIZERS "" CACHE STRING "Comma-separated -fsanitize= list")

if(BEDROCK_SANITIZERS)
  add_compile_options(-fsanitize=${BEDROCK_SANITIZERS} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${BEDROCK_SANITIZERS})
endif()

set(BEDROCK_WARNINGS -Wall -Wextra -Wpedantic -Werror)

add_library(bedrock_core src/cpu.cpp src/assembler.cpp)
target_include_directories(bedrock_core PUBLIC include)
target_compile_options(bedrock_core PRIVATE ${BEDROCK_WARNINGS})

add_executable(bedrock tools/bedrock.cpp)
target_link_libraries(bedrock PRIVATE bedrock_core)
target_compile_options(bedrock PRIVATE ${BEDROCK_WARNINGS})
target_compile_definitions(bedrock PRIVATE BEDROCK_PROGRAMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/programs")

if(BEDROCK_BUILD_TESTS)
  include(FetchContent)
  # Vendored tarball when present (offline machines), else GitHub; same hash.
  set(_gtest_tarball ${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest-1.15.2.tar.gz)
  if(EXISTS ${_gtest_tarball})
    set(_gtest_url ${_gtest_tarball})
  else()
    set(_gtest_url https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
  endif()
  FetchContent_Declare(googletest
    URL ${_gtest_url}
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(bedrock_tests tests/cpu_test.cpp tests/assembler_test.cpp)
  target_link_libraries(bedrock_tests PRIVATE bedrock_core GTest::gtest_main)
  target_compile_options(bedrock_tests PRIVATE ${BEDROCK_WARNINGS})
  include(GoogleTest)
  gtest_discover_tests(bedrock_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60)
  add_test(NAME machine_checks COMMAND bedrock --check)
endif()

# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
project(zlrs_host_tests LANGUAGES C CXX)
include(CTest)
include(FetchContent)
include(GoogleTest)

# Pin both the release and its bytes. FETCHCONTENT_SOURCE_DIR_GOOGLETEST can
# point at a pre-fetched source tree for offline builds.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
set(BUILD_GMOCK OFF CACHE BOOL "Build GoogleMock" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest" FORCE)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
  URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5)
FetchContent_MakeAvailable(googletest)

option(ZLRS_HOST_SANITIZERS "Build host tests with AddressSanitizer and UBSan" ON)
option(ZLRS_HOST_LEAKS "Enable LeakSanitizer during CTest" OFF)
set(ZLRS_HOST_FUZZ_ITERS 20000 CACHE STRING "Iterations per malformed-input fuzz loop")

if(NOT ZLRS_HOST_FUZZ_ITERS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "ZLRS_HOST_FUZZ_ITERS must be a positive integer")
endif()
if(ZLRS_HOST_LEAKS AND NOT ZLRS_HOST_SANITIZERS)
  message(FATAL_ERROR "ZLRS_HOST_LEAKS requires ZLRS_HOST_SANITIZERS")
endif()

# Compile production code once. The interface target gives every test and library
# the same warnings, sanitizer instrumentation and include paths.
add_library(zlrs_test_options INTERFACE)
target_compile_features(zlrs_test_options INTERFACE c_std_11)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
target_include_directories(zlrs_test_options INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../support"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../app/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../core/proto")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(zlrs_test_options INTERFACE -O2 -Wall -Wextra -Werror)
  # C++20 designated initializers deliberately zero unspecified C struct fields.
  target_compile_options(zlrs_test_options INTERFACE
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers>")
  if(ZLRS_HOST_SANITIZERS)
    target_compile_options(zlrs_test_options INTERFACE
      -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1)
    target_link_options(zlrs_test_options INTERFACE -fsanitize=address,undefined)
  endif()
elseif(ZLRS_HOST_SANITIZERS)
  message(FATAL_ERROR "ZLRS_HOST_SANITIZERS requires GCC or Clang")
endif()

add_library(zlrs_test_core STATIC
  ../support/sha256_soft.c
  ../../app/src/power.c
  ../../app/src/band.c
  ../../app/src/pairing_save.c
  ../../app/src/timing.c
  ../../core/mgmt/mgmt.c
  ../../core/region/region.c
  ../../core/proto/crypto.c
  ../../core/proto/crc.c
  ../../core/proto/fhss.c
  ../../core/proto/frame.c
  ../../core/proto/mod.c
  ../../core/link/link.c
  ../../core/link/pairing.c
  ../../core/link/session.c
  ../../core/link/data.c)
target_link_libraries(zlrs_test_core PUBLIC zlrs_test_options)

function(zlrs_gtest_module module)
  add_executable("${module}_tests" ${ARGN})
  target_link_libraries("${module}_tests" PRIVATE zlrs_test_core GTest::gtest_main)
  if(BUILD_TESTING)
    set(test_properties TIMEOUT 120)
    if(ZLRS_HOST_SANITIZERS)
      list(APPEND test_properties ENVIRONMENT "ASAN_OPTIONS=detect_leaks=$<BOOL:${ZLRS_HOST_LEAKS}>")
    endif()
    gtest_discover_tests("${module}_tests"
      TEST_PREFIX "host.${module}."
      EXTRA_ARGS --gtest_fail_if_no_test_selected
      PROPERTIES LABELS "host.${module}" ${test_properties}
      NO_PRETTY_VALUES
      DISCOVERY_MODE PRE_TEST)
  endif()
endfunction()

zlrs_gtest_module(proto test_crypto.cpp test_frame.cpp test_fhss.cpp test_mod.cpp)
zlrs_gtest_module(mgmt test_mgmt.cpp)
zlrs_gtest_module(region test_region.cpp)
zlrs_gtest_module(policy test_power.cpp test_band.cpp test_pairing_save.cpp test_timing.cpp)
zlrs_gtest_module(support test_data_pattern.cpp)
zlrs_gtest_module(radio test_radio.cpp)
zlrs_gtest_module(fuzz test_fuzz.cpp)
target_compile_definitions(fuzz_tests PRIVATE FUZZ_ITERS=${ZLRS_HOST_FUZZ_ITERS})
zlrs_gtest_module(link test_link.cpp)

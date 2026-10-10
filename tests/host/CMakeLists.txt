# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
project(zlrs_host_tests LANGUAGES C)

include(CTest)

option(ZLRS_HOST_SANITIZERS "Build host tests with AddressSanitizer and UBSan" ON)
option(ZLRS_HOST_LEAKS "Enable LeakSanitizer during CTest" OFF)
set(ZLRS_HOST_FUZZ_ITERS 20000 CACHE STRING "Iterations per malformed-input fuzz loop")

if(NOT ZLRS_HOST_FUZZ_ITERS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "ZLRS_HOST_FUZZ_ITERS must be a positive integer")
endif()

add_executable(zlrs_host_tests
  main.c
  test_crypto.c
  test_frame.c
  test_fhss.c
  test_link.c
  test_data_pattern.c
  test_radio.c
  test_mgmt.c
  test_region.c
  test_mod.c
  test_fuzz.c
  sha256_soft.c
  ../../core/mgmt/mgmt.c
  ../../core/region/region.c
  ../../core/proto/crypto.c
  ../../core/proto/crc.c
  ../../core/proto/fhss.c
  ../../core/proto/frame.c
  ../../core/proto/mod.c
  ../../core/link/link.c
  ../../core/link/status.c
)

set_target_properties(zlrs_host_tests PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
)
target_compile_definitions(zlrs_host_tests PRIVATE FUZZ_ITERS=${ZLRS_HOST_FUZZ_ITERS})
target_include_directories(zlrs_host_tests PRIVATE
  ../../include
  ../../core/proto
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(zlrs_host_tests PRIVATE -O2 -Wall -Wextra -Werror)

  if(ZLRS_HOST_SANITIZERS)
    target_compile_options(zlrs_host_tests PRIVATE
      -fsanitize=address,undefined
      -fno-sanitize-recover=all
      -fno-omit-frame-pointer
      -g
      -O1
    )
    target_link_options(zlrs_host_tests PRIVATE -fsanitize=address,undefined)
  elseif(ZLRS_HOST_LEAKS)
    message(FATAL_ERROR "ZLRS_HOST_LEAKS requires ZLRS_HOST_SANITIZERS")
  endif()
elseif(ZLRS_HOST_SANITIZERS)
  message(FATAL_ERROR "ZLRS_HOST_SANITIZERS requires GCC or Clang")
endif()

if(BUILD_TESTING)
  add_test(NAME host COMMAND zlrs_host_tests)
  if(ZLRS_HOST_SANITIZERS)
    if(ZLRS_HOST_LEAKS)
      set(_zlrs_detect_leaks 1)
    else()
      set(_zlrs_detect_leaks 0)
    endif()
    set_tests_properties(host PROPERTIES
      ENVIRONMENT "ASAN_OPTIONS=detect_leaks=${_zlrs_detect_leaks}"
    )
  endif()
endif()

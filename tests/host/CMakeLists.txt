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
target_include_directories(zlrs_test_options INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../support"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../app/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../core/proto")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(zlrs_test_options INTERFACE -O2 -Wall -Wextra -Werror)
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

function(zlrs_host_case name entry)
  set(target "host_${entry}")
  add_executable("${target}" main.c ${ARGN})
  target_link_libraries("${target}" PRIVATE zlrs_test_core)
  target_compile_definitions("${target}" PRIVATE
    "ZLRS_TEST_ENTRY=${entry}" "ZLRS_TEST_NAME=\"${name}\"")
  if(BUILD_TESTING)
    string(REGEX REPLACE "\\..*$" "" family "${name}")
    add_test(NAME "host.${name}" COMMAND "${target}")
    set_tests_properties("host.${name}" PROPERTIES LABELS "host;${family}" TIMEOUT 120)
    if(ZLRS_HOST_SANITIZERS)
      set_tests_properties("host.${name}" PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=$<BOOL:${ZLRS_HOST_LEAKS}>")
    endif()
  endif()
endfunction()

zlrs_host_case(proto.crypto test_crypto test_crypto.c)
zlrs_host_case(proto.frame test_frame test_frame.c)
zlrs_host_case(proto.fhss test_fhss test_fhss.c)
zlrs_host_case(proto.mod test_mod test_mod.c)
zlrs_host_case(mgmt.protocol test_mgmt test_mgmt.c)
zlrs_host_case(region test_region test_region.c)
zlrs_host_case(policy.power test_power test_power.c)
zlrs_host_case(policy.band test_band test_band.c)
zlrs_host_case(support.data_pattern test_data_pattern test_data_pattern.c)
zlrs_host_case(radio.request test_radio test_radio.c)
zlrs_host_case(fuzz.malformed_inputs test_fuzz test_fuzz.c)
target_compile_definitions(host_test_fuzz PRIVATE FUZZ_ITERS=${ZLRS_HOST_FUZZ_ITERS})

# Each link scenario gets a fresh process, independent static state, a timeout and
# its own CTest result. Build their shared two-node fixture only once.
add_library(zlrs_link_scenarios STATIC test_link.c)
target_link_libraries(zlrs_link_scenarios PUBLIC zlrs_test_core)
foreach(scenario IN ITEMS
    duplex replay_and_counters response_binding pairing_context
    confirmation_loss_and_exhaustion entropy_failure nonce_reuse
    band_plan_security capture_waits_for_phase automatic_bands
    quality_window_boundary data_sequence_wrap)
  zlrs_host_case("link.${scenario}" "test_${scenario}")
  target_link_libraries("host_test_${scenario}" PRIVATE zlrs_link_scenarios)
endforeach()

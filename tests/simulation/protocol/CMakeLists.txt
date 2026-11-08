# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(protocol_tests LANGUAGES C CXX)
get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include("${zlrs_root}/app/src/CMakeLists.txt")
target_sources(app PRIVATE
  test_band.cpp
  test_crypto.cpp
  test_data_pattern.cpp
  test_fhss.cpp
  test_frame.cpp
  test_fuzz.cpp
  test_link.cpp
  test_mod.cpp
  test_pairing_save.cpp
  test_power.cpp
  test_radio.cpp
  test_region.cpp
  "${zlrs_root}/app/src/power.c"
  "${zlrs_root}/app/src/band.c"
  "${zlrs_root}/app/src/pairing_save.c"
  "${zlrs_root}/tests/support/sha256_soft.c"
)
target_include_directories(app PRIVATE
  "${zlrs_root}/tests/support"
)
target_compile_options(app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-sign-compare>)
target_compile_definitions(app PRIVATE FUZZ_ITERS=${CONFIG_ZLRS_TEST_FUZZ_ITERATIONS})

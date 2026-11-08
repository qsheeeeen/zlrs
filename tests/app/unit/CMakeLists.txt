# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(protocol_tests LANGUAGES C CXX)
get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include("${zlrs_root}/app/src/CMakeLists.txt")
target_sources(app PRIVATE
  "${zlrs_root}/tests/app/band/test_band.cpp"
  "${zlrs_root}/tests/app/proto/test_crypto.cpp"
  "${zlrs_root}/tests/app/link/test_data_pattern.cpp"
  "${zlrs_root}/tests/app/proto/test_fhss.cpp"
  "${zlrs_root}/tests/app/proto/test_frame.cpp"
  "${zlrs_root}/tests/app/proto/test_fuzz.cpp"
  "${zlrs_root}/tests/app/link/test_link.cpp"
  "${zlrs_root}/tests/app/proto/test_mod.cpp"
  "${zlrs_root}/tests/app/lifecycle/test_pairing_save.cpp"
  "${zlrs_root}/tests/app/power/test_power.cpp"
  "${zlrs_root}/tests/app/platform/lr2021/test_helpers.cpp"
  "${zlrs_root}/tests/app/region/test_region.cpp"
  "${zlrs_root}/app/src/power.c"
  "${zlrs_root}/app/src/band.c"
  "${zlrs_root}/app/src/lifecycle.c"
  "${zlrs_root}/tests/app/support/sha256_soft.c"
)
target_include_directories(app PRIVATE
  "${zlrs_root}/tests/app/support"
)
target_compile_options(app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-sign-compare>)
target_compile_definitions(app PRIVATE FUZZ_ITERS=${CONFIG_ZLRS_TEST_FUZZ_ITERATIONS})

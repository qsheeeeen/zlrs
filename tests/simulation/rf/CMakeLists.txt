# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rf_simulation)
get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
target_sources(app PRIVATE test_rf.c "${zlrs_root}/app/src/rf.c"
 "${zlrs_root}/app/src/platform/temperature.c"
 "${zlrs_root}/app/src/diagnostics.c"
 "${zlrs_root}/app/src/timing.c"
 "${zlrs_root}/app/src/power.c" "${zlrs_root}/app/src/band.c"
 "${zlrs_root}/tests/support/sha256_soft.c")
target_include_directories(app PRIVATE "${zlrs_root}/app/src" "${zlrs_root}/core/proto")
target_link_options(app PUBLIC "-Wl,--wrap=zlrs_link_init"
 "-Wl,--wrap=z_impl_sys_rand_get")
target_compile_definitions(app PRIVATE
 RF_TEST_ROLE_FOLLOWER=$<BOOL:${RF_TEST_ROLE_FOLLOWER}>
 RF_TEST_BAND_AUTO=$<BOOL:${RF_TEST_BAND_AUTO}>
 RF_TEST_BAND_SUBG=$<BOOL:${RF_TEST_BAND_SUBG}>)

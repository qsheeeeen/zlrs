# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mgmt_simulation)
get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
target_sources(app PRIVATE test_mgmt.c "${zlrs_root}/app/src/mgmt.c"
 "${zlrs_root}/app/src/persist.c" "${zlrs_root}/tests/support/sha256_soft.c")
target_include_directories(app PRIVATE "${zlrs_root}/app/src" "${zlrs_root}/core/proto")
target_link_options(app PUBLIC "-Wl,--wrap=nvs_write")

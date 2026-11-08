# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(settings_simulation)
get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include("${zlrs_root}/app/src/CMakeLists.txt")
target_sources(app PRIVATE
  "${zlrs_root}/tests/app/persist/test_persist.c"
  "${zlrs_root}/app/src/persist.c"
)
target_include_directories(app PRIVATE
  "${zlrs_root}/app/src"
)

target_link_options(app PUBLIC "-Wl,--wrap=settings_save_one" "-Wl,--wrap=nvs_clear"
    "-Wl,--wrap=settings_load_subtree")

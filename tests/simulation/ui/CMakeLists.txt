# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ui_simulation)
get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
target_sources(app PRIVATE test_ui.c "${zlrs_root}/app/src/ui.c"
  "${zlrs_root}/tests/hardware/app/ui/test_led.c")
target_include_directories(app PRIVATE "${zlrs_root}/app/src" "${zlrs_root}/include")

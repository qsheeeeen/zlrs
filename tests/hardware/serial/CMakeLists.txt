# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(serial)
get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
target_sources(app PRIVATE
  ../ztest_main.c
  test_serial.c
  "${zlrs_root}/app/src/serial.c")
target_include_directories(app PRIVATE "${zlrs_root}/app/src")

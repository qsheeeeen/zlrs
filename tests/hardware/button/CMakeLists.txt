# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(CONF_FILE "${zlrs_root}/app/prj.conf" "${zlrs_root}/app/debug.conf" "${CMAKE_CURRENT_SOURCE_DIR}/prj.conf")
set(DTC_OVERLAY_FILE "${zlrs_root}/app/debug.overlay")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(button LANGUAGES C)
include("${zlrs_root}/app/CMakeLists.txt")
target_sources(app PRIVATE control.c)

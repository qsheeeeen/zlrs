# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(application_simulation)
get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
target_sources(app PRIVATE
  test_application.c
  "${zlrs_root}/app/src/main.c"
  "${zlrs_root}/app/src/boot.c")
set_source_files_properties("${zlrs_root}/app/src/main.c"
  PROPERTIES COMPILE_DEFINITIONS "main=zlrs_application_main")
target_include_directories(app PRIVATE "${zlrs_root}/app/src" "${zlrs_root}/include")

# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(CONF_FILE
  "${zlrs_root}/app/prj.conf"
  "${zlrs_root}/app/debug.conf"
  "${CMAKE_CURRENT_SOURCE_DIR}/prj.conf")
set(DTC_OVERLAY_FILE "${zlrs_root}/app/debug.overlay")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radio LANGUAGES C)
include("${zlrs_root}/app/CMakeLists.txt")

target_sources(app PRIVATE ../app/link/test_data.c ../app/platform/test_radio.c
 ../app/storage/test_pairing_save_fault.c)
target_include_directories(app PRIVATE . "${zlrs_root}/tests/support")
# Replace public interfaces only in the test image; the production firmware has no hooks.
set(zlrs_test_wrappers zlrs_link_init zlrs_link_tx_space zlrs_serial_read zlrs_serial_receive
  zlrs_link_complete zlrs_persist_get_pairing zlrs_persist_save_pairing)
if(CONFIG_ZLRS_TEST_RAW_MS GREATER 0)
  list(APPEND zlrs_test_wrappers zlrs_lr2021_init)
endif()
foreach(symbol IN LISTS zlrs_test_wrappers)
  target_link_options(app PUBLIC "-Wl,--wrap=${symbol}")
endforeach()
zephyr_code_relocate(FILES ../app/link/test_data.c ../app/platform/test_radio.c LOCATION RAM_TEXT)

# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(CONF_FILE
  "${zlrs_root}/app/prj.conf"
  "${zlrs_root}/app/debug.conf"
  "${CMAKE_CURRENT_SOURCE_DIR}/prj.conf"
)
set(DTC_OVERLAY_FILE
  "${zlrs_root}/app/debug.overlay"
)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radio LANGUAGES C)
include("${zlrs_root}/app/CMakeLists.txt")

target_sources(app PRIVATE
  "${zlrs_root}/tests/app/rf/hardware/control.c"
  "${zlrs_root}/tests/app/link/hardware/test_data.c"
  "${zlrs_root}/tests/app/platform/lr2021/hardware/test_radio.c"
  "${zlrs_root}/tests/app/rf/hardware/test_phy_pin.c"
  "${zlrs_root}/tests/app/lifecycle/hardware/test_pairing_save_fault.c"
)
target_include_directories(app PRIVATE
  .
  "${zlrs_root}/tests/app/support"
)
# Replace public interfaces only in the test image; the production firmware has no hooks.
set(zlrs_test_wrappers zlrs_link_init zlrs_link_tx_space zlrs_serial_read zlrs_serial_receive
  zlrs_link_complete zlrs_persist_state zlrs_persist_save_pairing)
if(CONFIG_ZLRS_TEST_RAW_MS GREATER 0)
  list(APPEND zlrs_test_wrappers zlrs_lr2021_init)
endif()
list(APPEND zlrs_test_wrappers zlrs_band_policy_update)
foreach(symbol IN LISTS zlrs_test_wrappers)
  target_link_options(app PUBLIC "-Wl,--wrap=${symbol}")
endforeach()
zephyr_code_relocate(
  FILES
  "${zlrs_root}/tests/app/link/hardware/test_data.c"
  LOCATION RAM_TEXT
)
# Native SMP report formatting is outside the timed radio path. Keep the RF probes
# in RAM without spending the 32 KiB hot-code budget on this control handler.
zephyr_code_relocate(
  FILES
  "${zlrs_root}/tests/app/platform/lr2021/hardware/test_radio.c"
  FILTER "^(?!.*zlrs_test_radio_control).*"
  LOCATION RAM_TEXT
)

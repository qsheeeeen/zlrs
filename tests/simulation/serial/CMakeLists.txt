# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(serial_simulation)
get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
target_sources(app PRIVATE
  test_serial.c
  "${zlrs_root}/app/src/serial.c"
  "${zlrs_root}/subsys/usb/usb_state.c")
# Deliver real USBD messages without constructing a physical USB controller.
set_source_files_properties("${zlrs_root}/app/src/serial.c"
  PROPERTIES COMPILE_DEFINITIONS "CONFIG_ZLRS_USB=1")
target_include_directories(app PRIVATE
  "${zlrs_root}/app/src" "${zlrs_root}/subsys/usb")

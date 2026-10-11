# SPDX-License-Identifier: Apache-2.0
set(mcuboot_EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/../../../app/sysbuild/mcuboot.conf"
  CACHE STRING "Shared ZLRS MCUboot configuration" FORCE)
set(mcuboot_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/../../../app/sysbuild/mcuboot.overlay"
  CACHE STRING "Shared ZLRS MCUboot devicetree overlay" FORCE)

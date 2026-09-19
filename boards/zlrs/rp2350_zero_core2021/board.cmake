# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# The RP2350-Zero has BOOTSEL, so dragging a UF2 is the easiest way to flash; debug is SWD.

if("${RPI_PICO_DEBUG_ADAPTER}" STREQUAL "")
  set(RPI_PICO_DEBUG_ADAPTER "cmsis-dap")
endif()

board_runner_args(openocd --cmd-pre-init "source [find interface/${RPI_PICO_DEBUG_ADAPTER}.cfg]")
board_runner_args(openocd --cmd-pre-init "source [find target/rp2350.cfg]")
# Adapter speed comes from the interface config, defaulting to 5000 as the Raspberry Pi docs say
board_runner_args(openocd --cmd-pre-init "set_adapter_speed_if_not_set 5000")

board_runner_args(probe-rs "--chip=RP235x")
board_runner_args(jlink "--device=RP2350_M33_0")
board_runner_args(uf2 "--board-id=RP2350")

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/probe-rs.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/uf2.board.cmake)

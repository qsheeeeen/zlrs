# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_UART_PL011 AND CONFIG_UART_INTERRUPT_DRIVEN)
  find_package(Git REQUIRED)
  set(uart_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/zephyr/uart-pl011-nonblocking-tx.patch")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ZEPHYR_BASE}/drivers/serial/uart_pl011.c" "${uart_patch}")
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${ZEPHYR_BASE}"
    apply --reverse --check "${uart_patch}"
    RESULT_VARIABLE uart_patch_status OUTPUT_QUIET ERROR_QUIET)
  if(NOT uart_patch_status EQUAL 0)
    message(FATAL_ERROR "Zephyr PL011 needs the nonblocking TX patch. Run: "
      "git -C ${ZEPHYR_BASE} apply ${uart_patch}")
  endif()
endif()

add_subdirectory_ifdef(CONFIG_ZLRS_RP2350_TRNG entropy)
add_subdirectory_ifdef(CONFIG_ZLRS_RP2350_SHA256 crypto)

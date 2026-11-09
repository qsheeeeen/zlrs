# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
# An including test application initializes its own Zephyr project first.
if("${CMAKE_CURRENT_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_LIST_DIR}")
  find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
  project(app LANGUAGES C)
endif()

set(zlrs_app_dir "${CMAKE_CURRENT_LIST_DIR}")
include("${zlrs_app_dir}/src/CMakeLists.txt")

target_sources(app PRIVATE
  ${zlrs_app_dir}/src/mgmt.c
  ${zlrs_app_dir}/src/serial.c
  ${zlrs_app_dir}/src/power.c
  ${zlrs_app_dir}/src/band.c
  ${zlrs_app_dir}/src/main.c
  ${zlrs_app_dir}/src/rf.c
  ${zlrs_app_dir}/src/persist.c
  ${zlrs_app_dir}/src/lifecycle.c
  ${zlrs_app_dir}/src/platform/sha256.c
  ${zlrs_app_dir}/src/platform/slot_timer.c
  ${zlrs_app_dir}/src/platform/watchdog.c
  ${zlrs_app_dir}/src/platform/lr2021.c
  ${zlrs_app_dir}/src/log.c
  ${zlrs_app_dir}/src/ui.c
)

target_sources_ifdef(CONFIG_ZLRS_APP_DEBUG_TRACE app PRIVATE
  ${zlrs_app_dir}/src/trace.c
)

if(CONFIG_ZLRS_APP_DEBUG_TRACE)
  # Zephyr 4.5's GPIO function hooks do not consult CONFIG_TRACING_GPIO.
  # Use the tracing macros' API-type masks without replacing the CTF hooks.
  if(NOT CONFIG_TRACING_GPIO)
    zephyr_compile_definitions(
      sys_port_trace_gpio_is_disabled=1
      sys_port_trace_gpio_pin_is_disabled=1
      sys_port_trace_gpio_port_is_disabled=1
    )
  endif()
  # SPI CS calls k_busy_wait(0) twice per transaction. Mask this source only;
  # the rest of the kernel retains k_thread scheduling and lifecycle events.
  set_property(SOURCE ${ZEPHYR_BASE}/kernel/busy_wait.c
    DIRECTORY ${ZEPHYR_BASE}/kernel APPEND PROPERTY COMPILE_DEFINITIONS
    sys_port_trace_k_thread_is_disabled=1
  )
endif()

# App-internal headers use their paths on disk, such as "platform/lr2021.h" and "serial.h"
# (the layers are described in doc/05-architecture.md).
target_include_directories(app PRIVATE
  "${zlrs_app_dir}/src"
)

if(CONFIG_CODE_DATA_RELOCATION)
  set(zlrs_ram_sources
    ${zlrs_app_dir}/src/rf.c
    ${zlrs_app_dir}/src/platform/sha256.c
    ${zlrs_app_dir}/src/platform/lr2021.c
    ${zlrs_app_dir}/src/serial.c
    ${zlrs_app_dir}/src/power.c
    ${zlrs_app_dir}/src/band.c
    ${zlrs_app_dir}/src/link/link.c
    ${zlrs_app_dir}/src/link/handshake.c
    ${zlrs_app_dir}/src/link/data.c
    ${zlrs_app_dir}/src/proto/crypto.c
    ${zlrs_app_dir}/src/proto/frame.c
    ${zlrs_app_dir}/src/proto/fhss.c
  )
  if(CONFIG_ZLRS_RP2350_SHA256)
    list(APPEND zlrs_ram_sources
      ${zlrs_app_dir}/../drivers/crypto/rp2350_sha256.c
    )
  endif()
  zephyr_code_relocate(
    FILES
    ${zlrs_ram_sources}
    LOCATION RAM_TEXT
  )
endif()

# Make the application version depend on the git index so that APP_VERSION_STRING
# reflects the commit even when nothing else in the build changed.
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --absolute-git-dir
    WORKING_DIRECTORY "${zlrs_app_dir}"
    OUTPUT_VARIABLE application_git_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE return_code
  )
  if(NOT return_code)
    set_property(TARGET app_version_h
                 PROPERTY APP_VERSION_DEPENDS ${application_git_dir}/index)
  endif()
endif()

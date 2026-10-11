# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
# An including test application initializes its own Zephyr project first.
if("${CMAKE_CURRENT_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_LIST_DIR}")
  find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
  project(app LANGUAGES C)
endif()

set(zlrs_app_dir "${CMAKE_CURRENT_LIST_DIR}")

target_sources(app PRIVATE
  ${zlrs_app_dir}/src/mgmt.c
  ${zlrs_app_dir}/src/serial.c
  ${zlrs_app_dir}/src/power.c
  ${zlrs_app_dir}/src/main.c
  ${zlrs_app_dir}/src/boot.c
  ${zlrs_app_dir}/src/rf.c
  ${zlrs_app_dir}/src/persist.c
  ${zlrs_app_dir}/src/platform/sha256.c
  ${zlrs_app_dir}/src/platform/slot_timer.c
  ${zlrs_app_dir}/src/platform/watchdog.c
  ${zlrs_app_dir}/src/platform/lr2021.c
  ${zlrs_app_dir}/src/log.c
  ${zlrs_app_dir}/src/ui.c
)

target_sources_ifdef(CONFIG_ZLRS_APP_DEBUG_SHELL app PRIVATE ${zlrs_app_dir}/src/shell.c)
target_sources_ifdef(CONFIG_ZLRS_APP_DEBUG app PRIVATE ${zlrs_app_dir}/src/diagnostics.c)

# App-internal headers are included the way they sit on disk: "platform/lr2021.h",
# "serial.h", "platform/caps.h", ... (the layers are described in doc/06-architecture.md).
# core/proto supplies the private SHA declaration used by the platform implementation.
target_include_directories(app PRIVATE "${zlrs_app_dir}/src" "${zlrs_app_dir}/../core/proto")

if(CONFIG_ZLRS_APP_RAM_RADIO)
  set(zlrs_ram_sources
    ${zlrs_app_dir}/src/rf.c
    ${zlrs_app_dir}/src/platform/sha256.c
    ${zlrs_app_dir}/src/platform/lr2021.c
    ${zlrs_app_dir}/src/serial.c
    ${zlrs_app_dir}/src/power.c
    ${zlrs_app_dir}/../core/link/link.c
    ${zlrs_app_dir}/../core/link/pairing.c
    ${zlrs_app_dir}/../core/link/session.c
    ${zlrs_app_dir}/../core/link/data.c
    ${zlrs_app_dir}/../core/proto/crypto.c
    ${zlrs_app_dir}/../core/proto/frame.c
    ${zlrs_app_dir}/../core/proto/fhss.c
  )
  if(CONFIG_ZLRS_RP2350_SHA256)
    list(APPEND zlrs_ram_sources ${zlrs_app_dir}/../drivers/crypto/rp2350_sha256.c)
  endif()
  zephyr_code_relocate(FILES ${zlrs_ram_sources} LOCATION RAM_TEXT)
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

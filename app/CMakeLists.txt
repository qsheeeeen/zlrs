# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)

target_sources(app PRIVATE
  src/mgmt/mode.c
  src/mgmt/port.c
  src/link/crsf.c
  src/link/data.c
  src/link/power.c
  src/link/rate.c
  src/main.c
  src/platform/recovery.c
  src/platform/watchdog.c
  src/ports/radio.c
  src/ports/serial.c
  src/state/sys_bus.c
  src/storage/log.c
  src/storage/settings.c
  src/ui/button.c
  src/ui/led.c
)

# App-internal headers are included the way they sit on disk: "ports/serial.h",
# "link/data.h", "platform/caps.h", ... (the layers are described in doc/07-architecture.md 5).
target_include_directories(app PRIVATE src)

# Make the application version depend on the git index so that APP_VERSION_STRING
# reflects the commit even when nothing else in the build changed.
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --absolute-git-dir
    WORKING_DIRECTORY .
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

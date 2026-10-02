# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)

target_sources(app PRIVATE
  src/mgmt/mode.c
  src/link/crsf.c
  src/link/data.c
  src/link/power.c
  src/link/rate.c
  src/main.c
  src/platform/slot_timer.c
  src/platform/recovery.c
  src/platform/watchdog.c
  src/platform/radio.c
  src/state/sys_bus.c
  src/storage/log.c
  src/storage/settings.c
  src/ui/button.c
  src/ui/led.c
)

# App-internal headers are included the way they sit on disk: "platform/radio.h",
# "link/data.h", "platform/caps.h", ... (the layers are described in doc/07-architecture.md 5).
target_include_directories(app PRIVATE src)

target_sources_ifdef(CONFIG_ZLRS_APP_RP2350_CRYPTO app PRIVATE src/platform/crypto_rp2350.c)

if(CONFIG_ZLRS_APP_RP2350_RADIO_IO)
  # Select our implementation of the public Semtech HAL without editing the module.
  # Keep its board/BSP and radio driver sources. Fail if the module layout changes.
  get_property(zlrs_libraries GLOBAL PROPERTY ZEPHYR_LIBS)
  set(zlrs_hal_replaced FALSE)
  foreach(zlrs_library IN LISTS zlrs_libraries)
    get_target_property(zlrs_sources ${zlrs_library} SOURCES)
    foreach(zlrs_source IN LISTS zlrs_sources)
      if(zlrs_source MATCHES "(^|/)lr20xx/lr20xx_hal\\.c$")
        list(REMOVE_ITEM zlrs_sources ${zlrs_source})
        set_property(TARGET ${zlrs_library} PROPERTY SOURCES "${zlrs_sources}")
        set(zlrs_hal_replaced TRUE)
      endif()
    endforeach()
  endforeach()
  if(NOT zlrs_hal_replaced)
    message(FATAL_ERROR "Cannot select the ZLRS LR2021 HAL: module source not found")
  endif()
  target_sources(app PRIVATE src/platform/radio_io.c)
endif()

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

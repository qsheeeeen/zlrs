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
  ${zlrs_app_dir}/src/mgmt/mode.c
  ${zlrs_app_dir}/src/link/crsf.c
  ${zlrs_app_dir}/src/link/data.c
  ${zlrs_app_dir}/src/link/power.c
  ${zlrs_app_dir}/src/link/rate.c
  ${zlrs_app_dir}/src/main.c
  ${zlrs_app_dir}/src/platform/crypto.c
  ${zlrs_app_dir}/src/platform/slot_timer.c
  ${zlrs_app_dir}/src/platform/recovery.c
  ${zlrs_app_dir}/src/platform/watchdog.c
  ${zlrs_app_dir}/src/platform/radio.c
  ${zlrs_app_dir}/src/state/sys_bus.c
  ${zlrs_app_dir}/src/storage/log.c
  ${zlrs_app_dir}/src/storage/settings.c
  ${zlrs_app_dir}/src/ui/button.c
  ${zlrs_app_dir}/src/ui/led.c
)

# App-internal headers are included the way they sit on disk: "platform/radio.h",
# "link/data.h", "platform/caps.h", ... (the layers are described in doc/06-architecture.md 5).
target_include_directories(app PRIVATE "${zlrs_app_dir}/src")

if(CONFIG_ZLRS_APP_RAM_RADIO)
  set(zlrs_ram_sources
    ${zlrs_app_dir}/src/main.c
    ${zlrs_app_dir}/src/platform/crypto.c
    ${zlrs_app_dir}/src/platform/radio.c
    ${zlrs_app_dir}/src/link/data.c
    ${zlrs_app_dir}/../core/link/link.c
    ${zlrs_app_dir}/../core/proto/frame.c
    ${zlrs_app_dir}/../core/proto/frag.c
    ${zlrs_app_dir}/../core/proto/fhss.c
  )
  if(CONFIG_ZLRS_APP_RP2350_RADIO_IO)
    list(APPEND zlrs_ram_sources ${zlrs_app_dir}/src/platform/radio_io.c)
  endif()
  # The RF owner also calls the public radio, DMA and crypto helpers. Relocate their compiled
  # text through Zephyr's build API; upstream files and behavior remain unchanged.
  get_property(zlrs_driver_libraries GLOBAL PROPERTY ZEPHYR_LIBS)
  foreach(zlrs_driver_library IN LISTS zlrs_driver_libraries)
    get_target_property(zlrs_driver_sources ${zlrs_driver_library} SOURCES)
    get_target_property(zlrs_driver_dir ${zlrs_driver_library} SOURCE_DIR)
    foreach(zlrs_driver_source IN LISTS zlrs_driver_sources)
      if(zlrs_driver_source MATCHES "(^|/)(rp2350_sha256|dma_rpi_pico|lr20xx_radio_(common|fifo|flrc)|lr20xx_system|ral_lr20xx|ralf_lr20xx)\\.c$")
        if(NOT IS_ABSOLUTE "${zlrs_driver_source}")
          set(zlrs_driver_source "${zlrs_driver_dir}/${zlrs_driver_source}")
        endif()
        list(APPEND zlrs_ram_sources "${zlrs_driver_source}")
      endif()
    endforeach()
  endforeach()
  zephyr_code_relocate(FILES ${zlrs_ram_sources} LOCATION RAM_TEXT)
endif()

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
  target_sources(app PRIVATE ${zlrs_app_dir}/src/platform/radio_io.c)
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

# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(unit)

get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
zephyr_get(ZLRS_TEST_ENTROPY_ONLY)
option(ZLRS_TEST_ENTROPY_ONLY "Run only hardware entropy acceptance without storage tests" OFF)
set(ZLRS_TEST_ENTROPY_ONLY "${ZLRS_TEST_ENTROPY_ONLY}" CACHE BOOL
  "Run only hardware entropy acceptance without storage tests" FORCE)
zephyr_get(ZLRS_TEST_UI_ONLY)
option(ZLRS_TEST_UI_ONLY "Run only LED acceptance without storage tests" OFF)
set(ZLRS_TEST_UI_ONLY "${ZLRS_TEST_UI_ONLY}" CACHE BOOL
  "Run only LED acceptance without storage tests" FORCE)
if(ZLRS_TEST_ENTROPY_ONLY AND ZLRS_TEST_UI_ONLY)
  message(FATAL_ERROR "Select only one isolated hardware fixture")
endif()
target_sources(app PRIVATE
  ../ztest_main.c
  "${zlrs_root}/app/src/platform/sha256.c")
target_include_directories(app PRIVATE "${zlrs_root}/app/src" "${zlrs_root}/core/proto" ..)

if(ZLRS_TEST_UI_ONLY)
  target_sources(app PRIVATE
    "${zlrs_root}/tests/contracts/test_led.c"
    "${zlrs_root}/app/src/ui.c")
else()
  target_sources(app PRIVATE ../drivers/entropy/test_rp2350_trng.c)
endif()

if(NOT ZLRS_TEST_ENTROPY_ONLY AND NOT ZLRS_TEST_UI_ONLY)
  target_sources(app PRIVATE
    ../drivers/crypto/test_rp2350_sha256.c
    ../core/proto/test_crypto.c
    ../app/storage/test_log.c
    "${zlrs_root}/tests/contracts/test_persist.c"
    ../core/link/test_link.c
    ../app/link/test_power.c
    ../app/platform/test_slot_timer.c
    "${zlrs_root}/tests/contracts/test_led.c"
    "${zlrs_root}/app/src/persist.c"
    "${zlrs_root}/app/src/power.c"
    "${zlrs_root}/app/src/platform/slot_timer.c"
    "${zlrs_root}/app/src/ui.c"
    "${zlrs_root}/app/src/log.c")
  target_link_options(app PUBLIC "-Wl,--wrap=nvs_write" "-Wl,--wrap=nvs_clear"
    "-Wl,--wrap=nvs_mount")
endif()

# Match production placement of the protocol algorithms, SHA operation and driver.
zephyr_code_relocate(FILES
  "${zlrs_root}/app/src/platform/sha256.c"
  "${zlrs_root}/core/proto/crypto.c"
  "${zlrs_root}/drivers/crypto/rp2350_sha256.c"
  LOCATION RAM_TEXT)

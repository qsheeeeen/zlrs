# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(crypto)

get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
target_sources(app PRIVATE src/main.c src/test_crypto.c
  "${zlrs_root}/app/src/platform/crypto.c")
target_include_directories(app PRIVATE "${zlrs_root}/app/src")

# Match the production placement of the two implementation files being measured.
zephyr_code_relocate(FILES
  "${zlrs_root}/app/src/platform/crypto.c"
  "${zlrs_root}/drivers/crypto/rp2350_sha256.c"
  LOCATION RAM_TEXT)

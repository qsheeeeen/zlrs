# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(
  crc.c
  fhss.c
  frag.c
  frame.c
  mod.c
  tlm.c
)
if(NOT CONFIG_ZLRS_APP_RP2350_CRYPTO)
  zephyr_library_sources(crypto_psa.c)
endif()

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
# The Zephyr driver follows devicetree; boards without it retain the PSA backend.
if(NOT CONFIG_ZLRS_RP2350_SHA256)
  zephyr_library_sources(crypto_psa.c)
endif()

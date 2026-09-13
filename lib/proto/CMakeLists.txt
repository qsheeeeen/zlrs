# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(
  crypto.c
  fhss.c
  frag.c
  frame.c
)

# 密码学原语来自 mbedTLS（Zephyr 自带），见 zlrs/proto/crypto.h
zephyr_library_link_libraries_ifdef(CONFIG_MBEDTLS mbedTLS)

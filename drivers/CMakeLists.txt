# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_ZLRS_RP2350_TRNG entropy)
add_subdirectory_ifdef(CONFIG_ZLRS_RP2350_SHA256 crypto)

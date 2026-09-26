# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_ZLRS_PROTO proto)
add_subdirectory_ifdef(CONFIG_ZLRS_LINK link)
add_subdirectory_ifdef(CONFIG_ZLRS_CRSF crsf)
add_subdirectory_ifdef(CONFIG_ZLRS_MGMT mgmt)
add_subdirectory_ifdef(CONFIG_ZLRS_REGION region)
add_subdirectory_ifdef(CONFIG_ZLRS_LED led)

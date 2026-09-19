# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# Zephyr picks this file up as the module CMake entry point, see
# zephyr/module.yml. It pulls the out-of-tree libraries and drivers of this
# repository into any application that includes zlrs as a module.

zephyr_include_directories(include)

add_subdirectory(lib)
add_subdirectory(drivers)

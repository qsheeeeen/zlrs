# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# Zephyr picks this file up as the module CMake entry point, see
# zephyr/module.yml. It registers shared subsystems and hardware drivers
# in the Zephyr build.

zephyr_include_directories(
  include
)

# Shared subsystems and hardware drivers use Zephyr.
add_subdirectory_ifdef(CONFIG_ZLRS_USB subsys/usb)
add_subdirectory_ifdef(CONFIG_ZLRS_BOOTGUARD subsys/bootguard)
add_subdirectory(drivers)

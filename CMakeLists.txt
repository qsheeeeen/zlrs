# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# Zephyr picks this file up as the module CMake entry point, see
# zephyr/module.yml. It pulls the out-of-tree libraries and drivers of this
# repository into any application that includes zlrs as a module.

zephyr_include_directories(include)

# core/ is the pure C feature core, subsys/ and drivers/ are the Zephyr-side
# pieces this module owns. Every image in the workspace (application and
# bootloader) gets them; the -ifdef guards keep them out of builds that do not
# enable the corresponding feature.
add_subdirectory(core)
add_subdirectory_ifdef(CONFIG_ZLRS_USB subsys/usb)
add_subdirectory_ifdef(CONFIG_ZLRS_BOOTGUARD subsys/bootguard)
add_subdirectory(drivers)

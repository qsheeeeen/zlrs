#!/bin/bash
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# Tree checks that need no build (README.md, "Gates"):
#
#   1. core/ and its public headers stay free of Zephyr. They compile on the host; that is
#      what makes the protocol testable and keeps the platform out of the link layer.
#   2. app/ never branches on a board. What the hardware is comes from devicetree through
#      app/src/caps.h, so supporting a board is a devicetree job, not a code change.
#   3. shipped configurations carry no ZLRS_APP_DEBUG_* option. Those exist for bench images;
#      the marker symbol CONFIG_ZLRS_APP_DEBUG lives in app/debug.conf only.
#   4. the application is never installed as a UF2: BOOTSEL carries the bootloader image only,
#      every application image goes through the bootloader (doc/08-firmware.md 5). A document or
#      tool that brings the app-UF2 path back fails here.
#
# Usage: tools/gates.sh

set -u

cd "$(dirname "$0")/.." || exit 1

fail=0

bad() {
	echo "gates: $1"
	fail=1
}

if hit=$(grep -rn '#include <zephyr/' core include 2>/dev/null); then
	bad "core/ or include/ pulled in a Zephyr header:"
	echo "$hit"
fi

if hit=$(grep -rn -E '\b(CONFIG_)?BOARD_[A-Z0-9_]+' app 2>/dev/null); then
	bad "app/ knows a board name:"
	echo "$hit"
fi

# Shipped configurations: everything that produces a firmware image. The debug and bring-up
# configurations are the only places allowed to name a bench switch.
shipped=$(git ls-files 'app' 'boards' | grep -E '\.(conf|defconfig)$' | grep -v -e debug -e bringup)

if [ -n "$shipped" ] && hit=$(printf '%s\n' "$shipped" | xargs grep -n 'ZLRS_APP_DEBUG'); then
	bad "a shipped configuration carries a debug option:"
	echo "$hit"
fi

hit=$(grep -rn -e 'signed\.uf2' -e 'make-signed-uf2' doc README.md boards/*/*/README.md tools \
	2>/dev/null | grep -v '^tools/gates.sh:')

if [ -n "$hit" ]; then
	bad "the application-as-UF2 path is forbidden:"
	echo "$hit"
fi

if [ "$fail" -eq 0 ]; then
	echo "gates: ok (core is pure C, app knows no boards, no debug option in shipped configs)"
fi

exit "$fail"

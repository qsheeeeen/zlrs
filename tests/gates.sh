#!/bin/bash
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# Tree checks that need no build (doc/10-debug.md#host-and-tool-checks):
#
#   1. core/ and its public headers stay free of Zephyr. They compile on the host; that is
#      what makes the protocol testable and keeps the platform out of the link layer.
#   2. app/ never branches on a board. Modules read hardware properties and aliases directly
#      from devicetree, so supporting a board is a devicetree job, not a code change.
#   3. shipped configurations carry no ZLRS_APP_DEBUG_* option. Those exist for bench images;
#      the marker symbol CONFIG_ZLRS_APP_DEBUG lives in app/debug.conf only.
#   4. the application is never installed as a UF2: BOOTSEL carries the bootloader image only,
#      every application image goes through the bootloader (doc/06-firmware.md). A document or
#      tool that brings the app-UF2 path back fails here.
#   5. the log ring fits its partition: the file backend's FILE_SIZE × FILES_LIMIT has to equal
#      the log_partition in the board's flash layout, or the ring either wastes flash or
#      overwrites the coredump partition next to it (doc/06-firmware.md).
#   6. RF owns link instances; frontends use business interfaces and own their channels.
#
# Usage: tests/gates.sh

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

if hit=$(rg -n 'zlrs_link_t|zlrs/link/link.h|zlrs_storage_settings_' \
	app/src/serial.c app/src/mgmt.c app/src/shell.c app/src/ui.c app/src/ui.h app/src/main.c); then
	bad "a frontend reaches into RF or private configuration storage:"
	echo "$hit"
fi

if hit=$(rg -n 'link_data\.' app/src/rf.c); then
	bad "RF accesses link storage directly instead of using link operations/views:"
	echo "$hit"
fi

if [ -e app/src/state/sys_bus.c ] || [ -e app/src/state/sys_bus.h ]; then
	bad "centralized module channels returned"
fi

# The core region's RF design value is allowed; application RF APIs/state are not.
if hit=$(rg -n --pcre2 '"rf.h"|zlrs_rf_(?!design_t\b)|zbus' app/src/persist.c app/src/persist.h); then
 bad "persist depends on RF or state publication:"
 echo "$hit"
fi

if hit=$(rg -n 'zlrs/mgmt|ZLRS_MGMT' app/src/log.c app/src/log.h); then
	bad "log storage depends on the management protocol:"
	echo "$hit"
fi

if hit=$(rg -n 'zlrs_link_t|zlrs_link_complete|zlrs/link/link.h' app/src/platform/lr2021.c app/src/platform/lr2021.h); then
	bad "LR2021 hardware depends on a protocol context:"
	echo "$hit"
fi

if hit=$(rg -n 'zephyr/|zlrs/link|LOG_|CONFIG_' app/src/power.c app/src/power.h); then
	bad "power policy depends on application wiring:"
	echo "$hit"
fi

if hit=$(rg -n 'zlrs/mgmt|ZLRS_MGMT' app/src/rf.c app/src/diagnostics.c app/src/diagnostics.h); then
	bad "RF or diagnostics depends on the management wire protocol:"
	echo "$hit"
fi

# Shipped configurations: everything that produces a firmware image. A debug configuration is
# the only place allowed to name a bench switch.
shipped=$(git ls-files 'app' 'boards' | grep -E '\.(conf|defconfig)$' | grep -v -e debug)

if [ -n "$shipped" ] && hit=$(printf '%s\n' "$shipped" | xargs grep -n 'ZLRS_APP_DEBUG'); then
	bad "a shipped configuration carries a debug option:"
	echo "$hit"
fi

hit=$(grep -rn -e 'signed\.uf2' -e 'make-signed-uf2' doc README.md boards/*/*/README.md tools \
	2>/dev/null)

if [ -n "$hit" ]; then
	bad "the application-as-UF2 path is forbidden:"
	echo "$hit"
fi

app_conf=app/prj.conf
partitions=boards/zlrs/rp2350_zero_core2021/rp2350_zero_core2021_partitions.dtsi

ring_size=$(sed -n 's/^CONFIG_LOG_BACKEND_FS_FILE_SIZE=\([0-9]*\)$/\1/p' "$app_conf")
ring_files=$(sed -n 's/^CONFIG_LOG_BACKEND_FS_FILES_LIMIT=\([0-9]*\)$/\1/p' "$app_conf")
partition_size=$(awk '
	/^[[:space:]]*log_partition:/ { seen = 1 }
	seen && /reg = </ {
		match($0, /0x[0-9a-fA-F]+ 0x[0-9a-fA-F]+/)
		split(substr($0, RSTART, RLENGTH), halves, " ")
		print halves[2]
		exit
	}' "$partitions")

if [ -z "$ring_size" ] || [ -z "$ring_files" ] || [ -z "$partition_size" ]; then
	bad "cannot check the log ring against its partition (config or layout missing)"
else
	want=$((ring_size * ring_files))
	have=$((partition_size))

	if [ "$want" -ne "$have" ]; then
		bad "the log ring is $want bytes (FILE_SIZE × FILES_LIMIT) but log_partition is $have:"
		echo "  $app_conf and $partitions disagree"
	fi
fi

# LR20xx BSP consumes three calibration frequencies, including zero padding.
for board in boards/zlrs/rp2350_zero_core2021/*_hf.dtsi boards/zlrs/rp2350_zero_core2021/*_lf.dtsi; do
    count=$(awk '/calibration-freqs = </ { gsub(/.*</, ""); gsub(/>.*/, ""); print NF }' "$board")
    if [ "$count" != 3 ]; then
        bad "$board: LR20xx calibration-freqs must contain exactly three entries"
    fi
done

if [ "$fail" -eq 0 ]; then
	echo "gates: ok (core is pure C, app knows no boards, no debug option in shipped configs, log ring matches its partition)"
fi

exit "$fail"

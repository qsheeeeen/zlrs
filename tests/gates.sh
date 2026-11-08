#!/bin/bash
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# Tree checks that need no build (doc/10-debug.md#host-and-tool-checks):
#
#   1. app/ never branches on a board. Modules read hardware properties and aliases directly
#      from devicetree, so supporting a board is a devicetree job, not a code change.
#   2. shipped configurations carry no ZLRS_APP_DEBUG_* option. Those exist for bench images;
#      the marker symbol CONFIG_ZLRS_APP_DEBUG lives in app/debug.conf only.
#   3. the application is never installed as a UF2: BOOTSEL carries the bootloader image only,
#      every application image goes through the bootloader (doc/06-firmware.md). A document or
#      tool that brings the app-UF2 path back fails here.
#   4. the log ring fits its partition: the file backend's FILE_SIZE × FILES_LIMIT has to equal
#      the log_partition in the board's flash layout, or the ring either wastes flash or
#      overwrites the coredump partition next to it (doc/06-firmware.md).
#   5. RF owns link instances; frontends use business interfaces and own their channels.
#   6. tests/app follows firmware modules; tests/tools checks host scripts.
#
# Usage: tests/gates.sh

set -u

cd "$(dirname "$0")/.." || exit 1

fail=0

bad() {
	echo "gates: $1"
	fail=1
}

# A failed search is not evidence that the tree satisfies a boundary.
reject_matches() {
	local message="$1" hit rc
	shift
	if hit=$(rg -n "$@"); then
		bad "$message"
		echo "$hit"
	else
		rc=$?
		if [ "$rc" -ne 1 ]; then
			bad "scan failed (exit $rc): $message"
		fi
	fi
}

reject_matches "app/ knows a board name:" '\b(CONFIG_)?BOARD_[A-Z0-9_]+' app

reject_matches "a frontend reaches into RF or private configuration storage:" \
	'zlrs_link_t|link/link.h|zlrs_storage_settings_' \
	app/src/serial.c app/src/mgmt.c app/src/ui.c app/src/ui.h app/src/main.c

reject_matches "RF accesses link storage directly instead of using link operations/views:" \
	'link_data\.' app/src/rf.c

if [ -e app/src/state/sys_bus.c ] || [ -e app/src/state/sys_bus.h ]; then
	bad "centralized module channels returned"
fi

reject_matches "interactive shell commands returned; use management or MCUmgr interfaces:" \
	'#[[:space:]]*include[[:space:]]*[<"]zephyr/shell/|SHELL_(CMD|STATIC_SUBCMD|SUBCMD)|^CONFIG_SHELL=y' \
	app tests/app --glob '*.[ch]' --glob '*.conf'

# Persistence validates settings without depending on the RF owner.
reject_matches "persist depends on RF or state publication:" \
	--pcre2 '"rf.h"|zlrs_rf_|zbus' app/src/persist.c app/src/persist.h

reject_matches "log storage depends on the management protocol:" \
	'zlrs/mgmt|ZLRS_MGMT' app/src/log.c app/src/log.h

reject_matches "LR2021 hardware depends on a protocol context:" \
	'zlrs_link_t|zlrs_link_complete|link/link.h' app/src/platform/lr2021.c app/src/platform/lr2021.h

reject_matches "RF or diagnostics depends on the management wire protocol:" \
	'zlrs/mgmt|ZLRS_MGMT' app/src/rf.c app/src/diagnostics.c app/src/diagnostics.h

# Shipped configurations: everything that produces a firmware image. A debug configuration is
# the only place allowed to name a bench switch.
reject_matches "a shipped configuration carries a debug option:" \
	'ZLRS_APP_DEBUG' app boards --glob '*.conf' --glob '*defconfig' --glob '!**/*debug*'

reject_matches "the application-as-UF2 path is forbidden:" \
	-e 'signed\.uf2' -e 'make-signed-uf2' doc README.md boards tools

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
	echo "gates: ok (Zephyr application ownership, app knows no boards, no debug option in shipped configs, log ring matches its partition)"
fi

exit "$fail"

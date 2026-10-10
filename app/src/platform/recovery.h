/*
 * Recovery: hand the board to MCUboot's serial recovery so the host can flash it.
 *
 * Reached from the management protocol (a command) and from the debug shell: the same action.
 * See doc/07-firmware.md for the boot chain it lands in.
 *
 * Naming: `<dir>/<file>.c` carries `zlrs_<dir>_<file>_*()`, so this is `zlrs_platform_recovery_*()`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Reboot into the bootloader's recovery port: 0 when the boot mode was written (the reset follows
 * and this never returns), negative when this image has nothing to reboot into.
 */
int zlrs_platform_recovery_enter(void);

/*
 * Recovery, see platform/recovery.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "platform/recovery.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/retention/bootmode.h>

#include "platform/watchdog.h"
#endif

LOG_MODULE_REGISTER(zlrs_platform_recovery, CONFIG_APP_LOG_LEVEL);

/*
 * Set the boot-mode flag (retained SRAM, survives a reset) so MCUboot waits for a signed image.
 * The only entries are the config protocol's recovery command and the debug shell, see
 * doc/10-security.md 10.
 */
int zlrs_platform_recovery_enter(void)
{
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	LOG_WRN("update trigger: rebooting into MCUboot serial recovery");

	/* RP2350's watchdog may still be armed after SYSRESETREQ; leaving it for MCUboot would
	 * cut its serial recovery short (subsys/bootguard is the bootloader-side guard). */
	zlrs_watchdog_disarm();

	if (bootmode_set(BOOT_MODE_TYPE_BOOTLOADER) != 0) {
		LOG_ERR("boot mode write failed; MCUboot will boot the app again");
		return -1;
	}

	/* Let the log reach the UART before resetting */
	k_msleep(50);
	sys_reboot(SYS_REBOOT_COLD);

	return -1; /* not reached */
#else
	LOG_WRN("no bootloader recovery in this image");

	return -1;
#endif
}

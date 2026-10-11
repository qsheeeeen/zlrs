/* SPDX-License-Identifier: Apache-2.0 */

#include "boot.h"

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>

#include "platform/watchdog.h"

LOG_MODULE_REGISTER(zlrs_boot, CONFIG_APP_LOG_LEVEL);

static bool confirmed;

int zlrs_boot_confirm(void)
{
	const int rc = boot_write_img_confirmed();
	if (rc == 0) {
		confirmed = true;
		LOG_INF("image confirmed by host: this build stays after a reset");
	} else {
		LOG_ERR("image confirm failed (%d); MCUboot will revert on next reset", rc);
	}
	return rc;
}

void zlrs_boot_check_rollback(bool fault)
{
	if (confirmed) {
		return;
	}
	if (boot_is_img_confirmed()) {
		confirmed = true;
		return;
	}
	if (fault) {
		LOG_ERR("fault on unconfirmed image: rebooting to roll back");
		k_msleep(2000);
		sys_reboot(SYS_REBOOT_COLD);
	} else if (k_uptime_get_32() >= (uint32_t)CONFIG_ZLRS_APP_MCUBOOT_CONFIRM_TIMEOUT_MS) {
		LOG_ERR("upgrade not confirmed by host: rebooting to roll back");
		sys_reboot(SYS_REBOOT_COLD);
	}
}

int zlrs_boot_recovery(void)
{
	LOG_WRN("update trigger: rebooting into MCUboot serial recovery");
	/* RP2350's watchdog survives SYSRESETREQ and would cut recovery short. */
	const int rc = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
	if (rc != 0) {
		LOG_ERR("boot mode write failed (%d)", rc);
		return rc;
	}
	zlrs_watchdog_disarm();
	k_msleep(50);
	sys_reboot(SYS_REBOOT_COLD);
}

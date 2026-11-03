/* SPDX-License-Identifier: Apache-2.0 */

#include "boot.h"

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(zlrs_boot, CONFIG_APP_LOG_LEVEL);

static bool confirmed;

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

/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#endif

#include "mgmt/mode.h"
#include "platform/slot_timer.h"
#include "rf/rf.h"
#include "runtime/update.h"

LOG_MODULE_REGISTER(zlrs_runtime_update, CONFIG_APP_LOG_LEVEL);

/* Only an explicit host confirmation lets a test upgrade survive its next reset. */
void zlrs_runtime_update_poll(void)
{
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	static bool confirmed;

	if (confirmed) {
		return;
	}
	if (boot_is_img_confirmed()) {
		confirmed = true;
		return;
	}
	if (zlrs_mgmt_mode_image_confirmed()) {
		zlrs_rf_pause();
		const int rc = boot_write_img_confirmed();
		zlrs_rf_resume();
		if (rc == 0) {
			confirmed = true;
			LOG_INF("image confirmed by host: this build stays after a reset");
		} else {
			LOG_ERR("image confirm failed; MCUboot will revert on next reset");
		}
	} else if (zlrs_slot_timer_now_ms() >=
		   (uint32_t)CONFIG_ZLRS_APP_MCUBOOT_CONFIRM_TIMEOUT_MS) {
		LOG_ERR("upgrade not confirmed by host: rebooting to roll back");
		sys_reboot(SYS_REBOOT_COLD);
	}
#endif
}

void zlrs_runtime_update_fault(void)
{
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	if (!boot_is_img_confirmed()) {
		/* Give the host 2 s to read the log, then let MCUboot roll back. */
		LOG_ERR("fault on unconfirmed image: rebooting to roll back");
		k_msleep(2000);
		sys_reboot(SYS_REBOOT_COLD);
	}
#endif
}

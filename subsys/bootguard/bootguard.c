/*
 * Bootloader-side guard: disarm the watchdog at startup. See the Kconfig help.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if DT_NODE_EXISTS(DT_ALIAS(watchdog0))
#include <hardware/watchdog.h>

#include <zephyr/drivers/watchdog.h>

LOG_MODULE_REGISTER(zlrs_bootguard, CONFIG_ZLRS_BOOTGUARD_LOG_LEVEL);

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

/*
 * wdt_disable() is not enough: it returns -EFAULT while data->enabled is false, which is
 * exactly the driver state after a reset (the hardware may still run), so the guard would be
 * useless and the watchdog would cut MCUboot's serial recovery short. Clear the enable bit.
 */
static void bootguard_clear_wdt_enable(void)
{
	hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
}

/*
 * Runs after the wdt driver's own init (PRE_KERNEL_1 + CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
 * 50 by default) so the device is guaranteed to be ready.
 */
static int bootguard_disarm_watchdog(void)
{
	if (!device_is_ready(wdt)) {
		LOG_WRN("no watchdog device; nothing to disarm");
		return 0;
	}

	bootguard_clear_wdt_enable();
	(void)wdt_disable(wdt); /* also mark the driver state stopped; success means both agree */
	LOG_INF("watchdog disarmed for boot");

	return 0;
}

SYS_INIT(bootguard_disarm_watchdog, PRE_KERNEL_1, 51);

#endif /* DT_NODE_EXISTS(DT_ALIAS(watchdog0)) */

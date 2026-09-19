/*
 * Hardware watchdog (RP2350 wdt0), active only when CONFIG_ZLRS_APP_WATCHDOG is on.
 *
 * It does not roll back by itself, it turns a hang into a reset (rollback is decided by the
 * image confirm bit, see ZLRS_APP_MCUBOOT_CONFIRM in app/Kconfig). Zephyr has no
 * RESET_ON_FATAL_ERROR on this board, so a crash just stops the CPU and nothing else resets it.
 *
 * Armed at PRE_KERNEL_1, so even a hang in the POST_KERNEL driver init gets reset.
 * Feeding happens in the blocking loops only: a dedicated feeding thread would defeat the point.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "watchdog.h"

#include <zephyr/kernel.h>

#if IS_ENABLED(CONFIG_ZLRS_APP_WATCHDOG)

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zlrs_wdt, CONFIG_APP_LOG_LEVEL);

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

static int wdt_channel = -1;

void zlrs_watchdog_arm(void)
{
	const struct wdt_timeout_cfg cfg = {
		.window = { .min = 0U, .max = CONFIG_ZLRS_APP_WATCHDOG_TIMEOUT_MS },
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int err;

	if (wdt_channel >= 0) {
		return; /* already armed */
	}

	if (!device_is_ready(wdt)) {
		LOG_ERR("watchdog device not ready");
		return;
	}

	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel < 0) {
		LOG_ERR("watchdog timeout install failed (%d)", wdt_channel);
		wdt_channel = -1;
		return;
	}

	err = wdt_setup(wdt, 0U);
	if (err != 0) {
		LOG_ERR("watchdog setup failed (%d)", err);
		wdt_channel = -1;
		return;
	}

	LOG_INF("watchdog armed: %d ms", CONFIG_ZLRS_APP_WATCHDOG_TIMEOUT_MS);
}

/*
 * Earlier than every POST_KERNEL driver and the application init, so their hangs get reset too.
 * The wdt driver itself sits at PRE_KERNEL_1 + CONFIG_KERNEL_INIT_PRIORITY_DEVICE (50 by
 * default) and we follow it. The priority has to be a literal: SYS_INIT builds a section name
 * from it, and "50 + 1" would trip "Undefined initialization levels used" in the linker.
 */
static int watchdog_arm_init(void)
{
	zlrs_watchdog_arm();

	return 0;
}

SYS_INIT(watchdog_arm_init, PRE_KERNEL_1, 51);

void zlrs_watchdog_feed(void)
{
	if (wdt_channel >= 0) {
		(void)wdt_feed(wdt, wdt_channel);
	}
}

void zlrs_watchdog_disarm(void)
{
	if (wdt_channel >= 0) {
		/* The driver state says enabled (we armed it), so wdt_disable() works this time */
		(void)wdt_disable(wdt);
		wdt_channel = -1;
		LOG_INF("watchdog disarmed");
	}
}

#else /* !CONFIG_ZLRS_APP_WATCHDOG */

void zlrs_watchdog_arm(void)
{
	/* Disabled: empty implementation */
}

void zlrs_watchdog_feed(void)
{
	/* Disabled: empty implementation so call sites need no conditionals */
}

void zlrs_watchdog_disarm(void)
{
	/* Disabled: empty implementation */
}

#endif /* CONFIG_ZLRS_APP_WATCHDOG */

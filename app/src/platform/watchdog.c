/*
 * Crash protection: the RP2350 hardware watchdog plus the fatal-error policy, on in every image
 * (CONFIG_ZLRS_APP_WATCHDOG, set in app/prj.conf).
 *
 * - The watchdog turns a hang into a reset. It does not roll back by itself: the rollback verdict
 *   is the image confirm bit (doc/07-firmware.md). Armed at PRE_KERNEL_1, so even a hang in the
 *   POST_KERNEL driver init gets reset, and fed from the blocking loops only — a dedicated
 *   feeding thread would defeat the point.
 * - A panic would stop the CPU on this board (Zephyr has no RESET_ON_FATAL_ERROR here), so
 *   k_sys_fatal_error_handler() at the bottom of this file reboots instead of halting.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "watchdog.h"

#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

#if IS_ENABLED(CONFIG_ZLRS_APP_WATCHDOG)

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(watchdog0)),
	     "watchdog enabled but this board has no watchdog device");

LOG_MODULE_REGISTER(zlrs_wdt, CONFIG_APP_LOG_LEVEL);

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

static int wdt_channel = -1;

void zlrs_watchdog_arm(void)
{
	const struct wdt_timeout_cfg cfg = {
		.window = {.min = 0U, .max = CONFIG_ZLRS_APP_WATCHDOG_TIMEOUT_MS},
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

/*
 * Fatal errors reboot instead of parking the CPU.
 *
 * Zephyr's default policy is to halt, which on this board means "off the air until someone
 * power-cycles the device": a panic inside the vendored radio HAL (lr20xx_hal_wait_on_busy()
 * calls k_oops() when BUSY never arrives) would be unrecoverable in the field.
 *
 * Rebooting is also what turns "the radio did not come up after an upgrade" into a failed
 * upgrade: an image the host has not confirmed yet still carries MCUboot's test flag, so a reset
 * before the confirm command makes the bootloader roll it back (doc/07-firmware.md). A
 * confirmed image simply comes back up.
 *
 * z_fatal_error() takes the coredump before calling this handler, so rebooting here does not
 * lose it; LOG_PANIC() flushes the log first (CONFIG_DEBUG_COREDUMP, app/prj.conf).
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	LOG_PANIC();
	LOG_ERR("fatal error %u: rebooting", reason);
	sys_reboot(SYS_REBOOT_COLD);
}

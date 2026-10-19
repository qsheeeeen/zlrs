/*
 * Bootloader-side guard: disarm the watchdog at startup. See the Kconfig help.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_IS_BOOTLOADER) && DT_NODE_EXISTS(DT_ALIAS(zlrs_led)) &&                         \
	IS_ENABLED(CONFIG_LED_STRIP)
#include <zephyr/drivers/led_strip.h>
#endif

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

#if defined(CONFIG_IS_BOOTLOADER) && DT_NODE_EXISTS(DT_ALIAS(zlrs_led)) &&                         \
	IS_ENABLED(CONFIG_LED_STRIP)

/* MCUboot has no application LED thread. White at 2 Hz identifies serial recovery and updates. */
#define BOOTGUARD_LED_STACK_SIZE 512U
#define BOOTGUARD_LED_PRIORITY   K_PRIO_PREEMPT(8)
#define BOOTGUARD_LED_PERIOD_MS  500U
#define BOOTGUARD_LED_ON_MS      250U

static const struct device *const bootguard_led = DEVICE_DT_GET(DT_ALIAS(zlrs_led));

static void bootguard_led_thread(void *p1, void *p2, void *p3)
{
	struct led_rgb pixel;
	bool on = true;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!device_is_ready(bootguard_led)) {
		return;
	}

	for (;;) {
		pixel.r = on ? 255U : 0U;
		pixel.g = on ? 255U : 0U;
		pixel.b = on ? 255U : 0U;
		(void)led_strip_update_rgb(bootguard_led, &pixel, 1U);
		on = !on;
		k_msleep(on ? (BOOTGUARD_LED_PERIOD_MS - BOOTGUARD_LED_ON_MS)
			    : BOOTGUARD_LED_ON_MS);
	}
}

K_THREAD_DEFINE(zlrs_bootguard_led_tid, BOOTGUARD_LED_STACK_SIZE, bootguard_led_thread, NULL, NULL,
		NULL, BOOTGUARD_LED_PRIORITY, 0, 0);

#endif /* CONFIG_IS_BOOTLOADER && zlrs_led && LED_STRIP */

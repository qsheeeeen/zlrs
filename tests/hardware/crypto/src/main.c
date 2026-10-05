/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/ztest.h>

static const struct device *const watchdog = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int watchdog_channel;

static void feed_watchdog(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	(void)wdt_feed(watchdog, watchdog_channel);
}

K_TIMER_DEFINE(watchdog_timer, feed_watchdog, NULL);

static int arm_watchdog(void)
{
	const struct wdt_timeout_cfg timeout = {
		.window.max = 8000U,
		.flags = WDT_FLAG_RESET_SOC,
	};

	/* Arm before console/USB initialization so an early fault also causes rollback. */
	watchdog_channel = wdt_install_timeout(watchdog, &timeout);
	if (watchdog_channel < 0) {
		return watchdog_channel;
	}
	return wdt_setup(watchdog, 0U);
}

SYS_INIT(arm_watchdog, PRE_KERNEL_1, 51);

static void restore_firmware(struct k_work *work)
{
	ARG_UNUSED(work);
	/* Leave this image unconfirmed: MCUboot restores the previously confirmed firmware. */
	sys_reboot(SYS_REBOOT_COLD);
}

K_WORK_DELAYABLE_DEFINE(restore_work, restore_firmware);

void test_main(void)
{
	const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0U;

	k_timer_start(&watchdog_timer, K_SECONDS(1), K_SECONDS(1));
	const int64_t deadline = k_uptime_get() + 45000;

	/* Capture every result after the host opens this test image's console CDC. */
	while (uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr) != 0 || dtr == 0U) {
		if (k_uptime_get() >= deadline) {
			sys_reboot(SYS_REBOOT_COLD);
		}
		k_msleep(20);
	}
	k_msleep(100);
	ztest_run_all(NULL, false, 1, 1);
	ztest_verify_all_test_suites_ran();
	printk("ZLRS crypto tests complete; rebooting for MCUboot rollback in 3 s\n");
	k_work_schedule(&restore_work, K_SECONDS(3));
}

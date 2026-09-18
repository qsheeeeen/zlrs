/*
 * 硬件看门狗（RP2350 wdt0），CONFIG_ZLRS_APP_WATCHDOG 打开才生效。
 *
 * 它不负责回滚，只负责"把卡死变成复位"（回滚判据是镜像确认位，见
 * app/Kconfig 的 ZLRS_APP_MCUBOOT_CONFIRM）。这块板上 Zephyr 没有
 * RESET_ON_FATAL_ERROR，崩溃就是 CPU 停住，没有它永远等不到那次复位。
 *
 * PRE_KERNEL_1 就装好：连其后 POST_KERNEL 驱动初始化挂死也能复位。
 * 喂狗只在会阻塞的循环里做——用独立线程喂等于白装。
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
		return; /* 已经武装着 */
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
 * 早于所有 POST_KERNEL 驱动与应用初始化：驱动初始化失败导致的挂死也能被复位。
 * wdt 驱动自己占 PRE_KERNEL_1 + CONFIG_KERNEL_INIT_PRIORITY_DEVICE（默认 50），
 * 我们排它后面。优先级必须写字面量：SYS_INIT 拿它拼 section 名，写 "50 + 1"
 * 会被链接器当成未定义的初始化层（Undefined initialization levels used）。
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
		/* 驱动状态里它是 enabled（我们装的），所以 wdt_disable() 这次能生效 */
		(void)wdt_disable(wdt);
		wdt_channel = -1;
		LOG_INF("watchdog disarmed before reboot");
	}
}

#else /* !CONFIG_ZLRS_APP_WATCHDOG */

void zlrs_watchdog_arm(void)
{
	/* 没开：空实现 */
}

void zlrs_watchdog_feed(void)
{
	/* 没开：空实现，调用点不用写条件编译 */
}

void zlrs_watchdog_disarm(void)
{
	/* 没开：空实现 */
}

#endif /* CONFIG_ZLRS_APP_WATCHDOG */

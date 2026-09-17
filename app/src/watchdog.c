/*
 * 硬件看门狗（RP2350 wdt0），CONFIG_ZLRS_APP_WATCHDOG 打开时才生效。
 *
 * 它**不是**回滚机制本身：MCUboot 的回滚只看"镜像有没有被确认"
 * （app/Kconfig 的 ZLRS_APP_MCUBOOT_CONFIRM），而回滚发生在**复位**时。看门狗
 * 管的只是"把卡死变成复位"：Zephyr 在这块板上没有 RESET_ON_FATAL_ERROR，崩溃就是
 * CPU 停住，没人复位就永远等不到回滚（第一次上板正是如此：USB 掉线，之后只能按
 * BOOTSEL）。
 *
 * 打开时在 PRE_KERNEL_1 就装好（紧跟 wdt 驱动自己的初始化），这样连它之后的
 * POST_KERNEL 驱动初始化挂死也能被复位。
 *
 * 喂狗点只有两处：主循环（超帧）与配置模式的轮询循环。**不要**用定时器或独立线程
 * 喂狗——那样主循环卡死也照样喂，看门狗就白装了。
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

static int watchdog_arm(void)
{
	const struct wdt_timeout_cfg cfg = {
		.window = { .min = 0U, .max = CONFIG_ZLRS_APP_WATCHDOG_TIMEOUT_MS },
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int err;

	if (!device_is_ready(wdt)) {
		LOG_ERR("watchdog device not ready");
		return -ENODEV;
	}

	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel < 0) {
		LOG_ERR("watchdog timeout install failed (%d)", wdt_channel);
		return wdt_channel;
	}

	err = wdt_setup(wdt, 0U);
	if (err != 0) {
		LOG_ERR("watchdog setup failed (%d)", err);
		wdt_channel = -1;
		return err;
	}

	LOG_INF("watchdog armed: %d ms", CONFIG_ZLRS_APP_WATCHDOG_TIMEOUT_MS);

	return 0;
}

/*
 * 早于所有 POST_KERNEL 驱动与应用初始化：驱动初始化失败导致的挂死也能被复位。
 * wdt 驱动自己占 PRE_KERNEL_1 + CONFIG_KERNEL_INIT_PRIORITY_DEVICE（默认 50），
 * 我们排它后面。优先级必须写字面量：SYS_INIT 拿它拼 section 名，写 "50 + 1"
 * 会被链接器当成未定义的初始化层（Undefined initialization levels used）。
 */
SYS_INIT(watchdog_arm, PRE_KERNEL_1, 51);

void zlrs_watchdog_feed(void)
{
	if (wdt_channel >= 0) {
		(void)wdt_feed(wdt, wdt_channel);
	}
}

#else /* !CONFIG_ZLRS_APP_WATCHDOG */

void zlrs_watchdog_feed(void)
{
	/* 看门狗没开：喂狗是空的，调用点不用写条件编译 */
}

#endif /* CONFIG_ZLRS_APP_WATCHDOG */

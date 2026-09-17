/*
 * bootloader 侧护栏：启动时把看门狗关掉。见 Kconfig 里的说明。
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
 * 为什么不用 wdt_disable()：RP2350 的看门狗驱动里那个函数是
 *
 *     if (data->enabled == false) { return -EFAULT; }   // 什么都不做
 *
 * 而复位之后驱动状态是全新的一局（enabled = false），硬件却可能还在跑——上一轮
 * 应用用 ZLRS_APP_WATCHDOG 点着的看门狗不会因为 SYSRESETREQ 就停。于是"护栏"
 * 调用 wdt_disable() 会被它自己拒绝（-EFAULT），硬件继续倒数：MCUboot 刚进串口
 * 恢复，几秒后就被复位打断——现象是"恢复口闪现一下又回到应用"。所以这里直接清
 * 硬件使能位（pico-sdk 的 register 定义，看门狗驱动自己也用它）。
 */
static void bootguard_clear_wdt_enable(void)
{
	hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
}

/*
 * 排在 wdt 驱动自己的初始化后面（PRE_KERNEL_1 + CONFIG_KERNEL_INIT_PRIORITY_DEVICE，
 * 默认 50），这样能确保设备已经 ready。
 */
static int bootguard_disarm_watchdog(void)
{
	if (!device_is_ready(wdt)) {
		LOG_WRN("no watchdog device; nothing to disarm");
		return 0;
	}

	bootguard_clear_wdt_enable();
	(void)wdt_disable(wdt); /* 顺手把驱动状态也标成停：成功才说明两边一致 */
	LOG_INF("watchdog disarmed for boot");

	return 0;
}

SYS_INIT(bootguard_disarm_watchdog, PRE_KERNEL_1, 51);

#endif /* DT_NODE_EXISTS(DT_ALIAS(watchdog0)) */

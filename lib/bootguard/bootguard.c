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
 * 不能用 wdt_disable()：它在 data->enabled == false 时直接 -EFAULT 返回，而复位后
 * 驱动状态正是"没使能"（硬件可能还在跑），于是护栏形同虚设、MCUboot 的串口恢复被
 * 看门狗打断。这里直接清硬件使能位（pico-sdk 寄存器定义）。
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

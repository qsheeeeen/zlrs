/*
 * 硬件看门狗：崩溃或卡死时让芯片自己复位，配合 MCUboot 的 test/revert 回滚。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_APP_WATCHDOG_H_
#define ZLRS_APP_WATCHDOG_H_

/* 喂狗：主循环与配置模式的轮询循环里调用。没开看门狗时是空实现 */
void zlrs_watchdog_feed(void);

#endif /* ZLRS_APP_WATCHDOG_H_ */

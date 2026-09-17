/*
 * 硬件看门狗：崩溃或卡死时让芯片自己复位，配合 MCUboot 的 test/revert 回滚。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_APP_WATCHDOG_H_
#define ZLRS_APP_WATCHDOG_H_

/* 喂狗：主循环与配置模式的轮询循环里调用。没开看门狗时是空实现 */
void zlrs_watchdog_feed(void);

/*
 * 关掉看门狗。重启进 MCUboot 串口恢复之前必须调它：RP2350 的看门狗在
 * SYSRESETREQ 之后可能还在跑，留给 MCUboot 的话，它刚进恢复几秒就被复位打断——
 * 上传永远传不完（上板实测：恢复窗口只有 ~1.5 s）。
 * 没开看门狗时是空实现。
 */
void zlrs_watchdog_disarm(void);

#endif /* ZLRS_APP_WATCHDOG_H_ */

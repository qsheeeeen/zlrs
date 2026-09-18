/*
 * 硬件看门狗：崩溃或卡死时让芯片自己复位，配合 MCUboot 的 test/revert 回滚。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* 喂狗：主循环与配置模式的轮询循环里调用。没开看门狗时是空实现 */
void zlrs_watchdog_feed(void);

/* 关掉看门狗。重启进 MCUboot 串口恢复前必须调：RP2350 的看门狗在 SYSRESETREQ
 * 之后可能还在跑，留给 MCUboot 会把它的串口恢复打断。没开时是空实现。 */
void zlrs_watchdog_disarm(void);

/* 重新武装（配置会话这类长时间阻塞的操作结束后调用）。没开时是空实现 */
void zlrs_watchdog_arm(void);


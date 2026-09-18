/*
 * 射频移植层：zlrs_radio_ops_t 的具体后端，回调契约见 doc/01-system.md 7.2。
 * 由 CONFIG_ZLRS_APP_RADIO 二选一：radio_rac.c（RAC/LR2021）或 radio_null.c。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_APP_RADIO_PORT_H_
#define ZLRS_APP_RADIO_PORT_H_

#include <stdint.h>

#include "zlrs/link/link.h"

/* 传给 zlrs_link_init() 的操作表 */
const zlrs_radio_ops_t *zlrs_radio_control_ops(void);
const zlrs_radio_ops_t *zlrs_radio_data_ops(void);

/* 打开后端并把完成结果接到两条链路上（失败返回负值） */
int zlrs_radio_port_init(zlrs_link_t *control, zlrs_link_t *data);

/*
 * 等一次事务完成；超时返回负值。
 * 用微秒：控制帧率拉到 500 Hz 以后，一个时隙的预算只有几百微秒。
 * 射频关掉时没有可等的东西，立刻返回 0。
 */
int zlrs_radio_port_wait_us(uint32_t timeout_us);

/* 把已完成的事务结果回填给链路层（链路线程里调用） */
void zlrs_radio_port_drain(void);

/* 放弃挂起的事务（时隙预算用完时用，避免迟到的完成事件串到下一次事务上） */
void zlrs_radio_port_abort(void);

/* 与射频后端同一个时间基（k_uptime_get_32） */
uint32_t zlrs_radio_port_now_ms(void);

#endif /* ZLRS_APP_RADIO_PORT_H_ */

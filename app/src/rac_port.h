/*
 * USP/RAC 移植层：把 zlrs 链路层的射频抽象（zlrs_radio_ops_t）落到
 * LR2021 上，回调契约见 doc/01-system.md 第 7.2 节。
 *
 * 两条链路各占一个 RAC 客户端：控制链路 HIGH、数据链路 MEDIUM。
 * 事务提交后由 RAC 线程回调，链路线程用 zlrs_rac_port_wait() 等结果，
 * 再由 zlrs_rac_port_drain() 回填给链路层。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_APP_RAC_PORT_H_
#define ZLRS_APP_RAC_PORT_H_

#include <stdint.h>

#include "zlrs/link/link.h"

/* 射频操作表：传给 zlrs_link_init() 用 */
const zlrs_radio_ops_t *zlrs_rac_control_ops(void);
const zlrs_radio_ops_t *zlrs_rac_data_ops(void);

/* 打开两个 RAC 客户端，并把完成结果接到这两条链路上 */
int zlrs_rac_port_init(zlrs_link_t *control, zlrs_link_t *data);

/* 等一次事务完成；超时返回负值 */
int zlrs_rac_port_wait(uint32_t timeout_ms);

/* 把已完成的事务结果回填给链路层（链路线程里调用） */
void zlrs_rac_port_drain(void);

/* 放弃挂起的事务（时隙预算用完时用，避免迟到的完成事件串到下一次事务上） */
void zlrs_rac_port_abort(void);

/* 与 RAC/USP 同一个时间基（k_uptime_get_32） */
uint32_t zlrs_rac_port_now_ms(void);

#endif /* ZLRS_APP_RAC_PORT_H_ */

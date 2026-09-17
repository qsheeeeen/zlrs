/*
 * 射频移植层：把 zlrs 链路层的射频抽象（zlrs_radio_ops_t）落到具体后端，
 * 回调契约见 doc/01-system.md 第 7.2 节。
 *
 * 两个后端，由 CONFIG_ZLRS_APP_RADIO 二选一：
 *
 *   radio_rac.c   射频在（默认）：两条链路各占一个 RAC 客户端（控制 HIGH、
 *                 数据 MEDIUM），事务提交后由 RAC 线程回调，链路线程用
 *                 zlrs_radio_port_wait_us() 等结果，再由 ..._drain() 回填。
 *   radio_null.c  射频关：Core2021 还没接上时用的空后端——不碰 SPI/BUSY，
 *                 每次事务当场按"发出去、没收到回音"收尾，其余部分照常跑。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_APP_RADIO_PORT_H_
#define ZLRS_APP_RADIO_PORT_H_

#include <stdint.h>

#include "zlrs/link/link.h"

/* 射频操作表：传给 zlrs_link_init() 用 */
const zlrs_radio_ops_t *zlrs_radio_control_ops(void);
const zlrs_radio_ops_t *zlrs_radio_data_ops(void);

/* 打开两条链路的射频后端，并把完成结果接到这两条链路上 */
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

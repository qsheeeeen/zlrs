/*
 * 状态灯的驱动侧：把 lib/led 算出来的颜色写到板上的 WS2812。
 *
 * 板级要提供 `zlrs-led` 别名（板子上是 PIO0 上的 WS2812，见 doc/06-ui.md）；
 * 没有这个别名的板子这里会退化成空操作。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_APP_LED_PORT_H_
#define ZLRS_APP_LED_PORT_H_

#include <stdint.h>

#include "zlrs/led/led.h"

/* 找设备；板上没有状态灯时返回 -1，但后续 update 调用是安全的 */
int zlrs_led_port_init(void);

/* 按状态算出颜色并刷新（内部限速 20 Hz）；now_ms 用链路时基 */
void zlrs_led_port_update(const zlrs_led_input_t *in, uint32_t now_ms);

#endif /* ZLRS_APP_LED_PORT_H_ */

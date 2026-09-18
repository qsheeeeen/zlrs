/*
 * 状态灯的驱动侧：把 lib/led 算出的颜色写到板上的 WS2812（需要 `zlrs-led` 别名，
 * 没有就退化成空操作）。见 doc/06-ui.md。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "zlrs/led/led.h"

/* 找设备；板上没有状态灯时返回 -1，但后续 update 调用是安全的 */
int zlrs_led_port_init(void);

/* 按状态算出颜色并刷新（内部限速 20 Hz）；now_ms 用链路时基 */
void zlrs_led_port_update(const zlrs_led_input_t *in, uint32_t now_ms);


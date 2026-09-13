/*
 * CRSF 端口：手台 ↔ 模块（TX 侧）、接收机 → 飞控（RX 侧）。
 *
 * 具体是哪个串口设备由板级 devicetree 的 `zlrs-crsf-uart` 别名决定：原型板上
 * 是 USB CDC ACM，自定义板接 UART 也可以——驱动层只要实现 Zephyr 的 UART
 * 中断接口就行。端口格式见 doc/01-system.md 2.1。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_APP_CRSF_PORT_H_
#define ZLRS_APP_CRSF_PORT_H_

#include <stdbool.h>
#include <stdint.h>

int zlrs_crsf_port_init(void);

/* 端口是否可用（板上没定义别名或设备没就绪时为 false） */
bool zlrs_crsf_port_ready(void);

/*
 * 组帧并排进发送缓冲：type 决定地址（RC 帧 0xC8 给飞控、链路统计 0xEA 给手台）。
 * 返回整帧长度，失败返回 0。
 */
uint8_t zlrs_crsf_port_write(uint8_t type, const uint8_t *payload, uint8_t payload_len);

/* 从接收缓冲取一帧（主循环里调用），返回载荷长度；没有帧返回 0 */
uint8_t zlrs_crsf_port_read(uint8_t *type, uint8_t *payload, uint8_t max_len);

/* 端口统计：接收缓冲放不下而丢掉的帧数 */
uint32_t zlrs_crsf_port_rx_dropped(void);

#endif /* ZLRS_APP_CRSF_PORT_H_ */

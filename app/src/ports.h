/*
 * 端口层：CRSF 端口、数据口（透传）与配置模式，以及两条链路各自的端点选择。
 *
 * 板级用别名给出每条链路挂在哪块端点上（doc/01-system.md 2.1）：
 *
 *   CRSF：`zlrs-crsf-uart`（UART0 = GP0/GP1）、`zlrs-crsf-cdc`
 *   数据：`zlrs-data-uart`（UART1 = GP8/GP9）、`zlrs-data-cdc`
 *
 * 选择规则：USB 上的主机把该链路的 CDC 打开（DTR 有效）就走 USB，否则走 UART；
 * 别名缺失的端点在编译期裁掉（2.2）。控制台不经过这一层：板级 chosen 的
 * `zephyr,console` 指向 USB 的第三个 CDC。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_APP_PORTS_H_
#define ZLRS_APP_PORTS_H_

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/link/link.h"

/* ---- 数据口：透传字节流（正常模式） ------------------------------------- */

/* baud = 0 表示用板级默认（115200）；非 0 时覆盖（配置工具设过就用它） */
int zlrs_data_port_init(uint32_t baud);

/* 从接收环形缓冲取数据，返回实际取到的字节数 */
uint16_t zlrs_data_port_read(uint8_t *buf, uint16_t len);

/* 放进发送环形缓冲并启动中断发送，返回接受的字节数 */
uint16_t zlrs_data_port_write(const uint8_t *buf, uint16_t len);

/* 没送出去的字节数：缓冲满丢的 + 当前端点之外那块端点收到的 */
uint32_t zlrs_data_port_dropped(void);

/* ---- CRSF 端口：手台 ↔ 模块（TX 侧）、接收机 → 飞控（RX 侧） ------------- */

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

/* ---- 配置模式：开机时按住按键进入，数据口改跑配置协议（doc/02 第 10 节） -- */

/*
 * 阻塞式跑配置会话：到超时或收到 EXIT 为止。
 * 命令作用在 control 链路上；失控值写进 failsafe（应用自己那份缓冲）。
 * 返回 0 = 收到 EXIT，1 = 超时，-1 = 板上没有数据口别名。
 */
int zlrs_config_mode_run(zlrs_link_t *control, uint8_t *failsafe);

#endif /* ZLRS_APP_PORTS_H_ */

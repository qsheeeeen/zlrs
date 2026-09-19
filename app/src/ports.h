/*
 * 端口层：CRSF 端口、数据口（透传）与配置模式，含两条链路的端点二选一。
 *
 * 端点由板级别名给出，选择规则与裁剪方式见 doc/08-firmware.md 第 1、4 节；控制台不走
 * 这一层（板级 chosen 的 `zephyr,console`）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/link/link.h"

/* ---- 数据口：透传字节流（正常模式） ------------------------------------- */

/* 上板调试用：一块数据端点的状态（`zlrs ports` 命令打印它） */
typedef struct {
	const char *name;
	bool ready;      /* 板上有这块端点且设备就绪 */
	bool active;     /* 当前选中的就是它（见 port_active） */
	uint32_t rx_total;   /* 收到过多少字节（主机在用的证据之一） */
	uint32_t rx_dropped; /* 非当前端点被丢掉的字节数 */
} zlrs_port_state_t;

/*
 * 取数据口两块端点的状态，返回填了几条。
 * 端点二选一的判据（DTR / 收到过主机字节）在 port_active()，这里只做展示。
 */
size_t zlrs_data_port_state(zlrs_port_state_t *out, size_t count);

/* baud = 0 表示用板级默认（115200）；非 0 时覆盖（配置工具设过就用它） */
int zlrs_data_port_init(uint32_t baud);

/* 从接收环形缓冲取数据，返回实际取到的字节数 */
uint16_t zlrs_data_port_read(uint8_t *buf, uint16_t len);

/* 放进发送环形缓冲并启动中断发送，返回接受的字节数 */
uint16_t zlrs_data_port_write(const uint8_t *buf, uint16_t len);

/* 没送出去的字节数：缓冲满丢的 + 当前端点之外那块端点收到的 */
uint32_t zlrs_data_port_dropped(void);

/* 配置模式期间用：把两块端点收到的字节丢掉（只计数，不投递） */
void zlrs_data_port_discard_rx(void);

/* 配置协议要动的应用侧动作；应用启动时注册，没注册的动作回 E_STATE */
typedef struct {
	int (*save_region)(uint8_t region);
	int (*save_power)(int8_t dbm);
	int (*erase_pair)(void);
	int (*factory_reset)(void);
	int (*enter_recovery)(void);
	uint8_t (*region)(void);
	int8_t (*power)(void);
} zlrs_config_ops_t;

void zlrs_config_set_ops(const zlrs_config_ops_t *ops);

/*
 * 上位机发过"确认这一版"的命令（见 doc/08-firmware.md 第 6 节的升级流程）。
 * 这是"这一版镜像能不能留下"的唯一判据：升级后主机没确认就不算数，
 * 复位时让 MCUboot 回滚。
 */
bool zlrs_config_image_confirmed(void);

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

/* ---- 配置口：专用的 CDC（doc/08-firmware.md 第 1 节） --------------------- */

int zlrs_config_port_init(void);
bool zlrs_config_port_ready(void);

/*
 * 主循环里非阻塞地看配置口：
 * 收到"进配置模式"返回 true（ACK 已回），其它命令回 E_STATE。
 */
bool zlrs_config_port_poll_enter(void);

/* ---- 配置模式：由配置工具在配置 CDC 上发命令进入（doc/08-firmware.md 第 2 节） -- */

/*
 * 阻塞式跑配置会话：到超时或收到 EXIT 为止。
 * 命令作用在 control 链路上；失控值写进 failsafe（应用自己那份缓冲）。
 * 返回 0 = 收到 EXIT，1 = 超时，-1 = 板上没有数据口别名。
 */
int zlrs_config_mode_run(zlrs_link_t *control, uint8_t *failsafe);

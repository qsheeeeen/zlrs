/*
 * 状态灯：一个 WS2812 像素表达链路状态与告警。颜色与节奏都编码信息（只看颜色在
 * 强光或色觉差异下不可读），优先级见 doc/06-ui.md。这层只算颜色，驱动在
 * app/src/led_port.c。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_LED_LED_H_
#define ZLRS_LED_LED_H_

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/link/link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint8_t r;
	uint8_t g;
	uint8_t b;
} zlrs_led_rgb_t;

typedef struct {
	bool config_mode;  /* 正在跑配置模式 */
	bool configured;   /* 有根密钥 */
	bool paired;       /* 有配对记录 */
	bool failsafe;     /* 正在发/输出失控值（TX 输入超时、RX 未连接） */
	bool drop_recent;  /* 最近一秒有丢弃（透传积压或 ARQ 放弃） */
	bool role_tx;      /* 角色，只在开机自检窗口里用 */
	uint8_t link_state; /* 控制链路状态 ZLRS_STATE_* */
	uint8_t lq_percent;
	uint32_t uptime_ms; /* 上电以来的毫秒数：前 1.5 秒亮角色色 */
} zlrs_led_input_t;

/* 算当前该显示的颜色。now_ms 用毫秒时基，只用于闪烁相位 */
zlrs_led_rgb_t zlrs_led_frame(const zlrs_led_input_t *in, uint32_t now_ms);

/* 被点亮的像素数：原型板是 1 个像素 */
#define ZLRS_LED_PIXELS 1U

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_LED_LED_H_ */

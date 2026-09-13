/*
 * 状态灯的颜色与节奏，见 include/zlrs/led/led.h。
 *
 * 亮度上限刻意压得低（<= 30%）：这是给眼睛看的指示，不是照明；
 * 省电、也不至于在夜里刺眼。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/led/led.h"

#include <stddef.h>

/* 调色板：颜色只取几种，保证互相区分得开 */
static const zlrs_led_rgb_t off = { 0U, 0U, 0U };
static const zlrs_led_rgb_t white = { 255U, 255U, 255U };
static const zlrs_led_rgb_t purple = { 160U, 0U, 200U };
static const zlrs_led_rgb_t yellow = { 255U, 180U, 0U };
static const zlrs_led_rgb_t blue = { 0U, 90U, 255U };
static const zlrs_led_rgb_t cyan = { 0U, 200U, 200U };
static const zlrs_led_rgb_t green = { 0U, 255U, 0U };
static const zlrs_led_rgb_t amber = { 255U, 120U, 0U };
static const zlrs_led_rgb_t red = { 255U, 0U, 0U };
static const zlrs_led_rgb_t orange = { 255U, 80U, 0U };

/* 按百分比调亮度（0–100） */
static zlrs_led_rgb_t scale(zlrs_led_rgb_t color, uint8_t percent)
{
	zlrs_led_rgb_t out;

	out.r = (uint8_t)(((uint16_t)color.r * percent) / 100U);
	out.g = (uint8_t)(((uint16_t)color.g * percent) / 100U);
	out.b = (uint8_t)(((uint16_t)color.b * percent) / 100U);

	return out;
}

/* 闪烁：周期 period_ms 里亮 on_ms，其余时间灭 */
static zlrs_led_rgb_t blink(zlrs_led_rgb_t color, uint32_t now_ms, uint32_t period_ms,
			    uint32_t on_ms)
{
	return ((now_ms % period_ms) < on_ms) ? color : off;
}

/* 每 period_ms 闪一次（单闪），用于在常亮状态上叠加提示 */
static bool single_flash(uint32_t now_ms, uint32_t period_ms, uint32_t on_ms)
{
	return (now_ms % period_ms) < on_ms;
}

/* 链路状态 → 颜色与节奏；只有 CONNECTED 会用到 LQ */
static zlrs_led_rgb_t link_state_frame(const zlrs_led_input_t *in, uint32_t now_ms)
{
	switch (in->link_state) {
	case ZLRS_STATE_CONNECTED:
		if (in->lq_percent >= 80U) {
			return scale(green, 30U);
		}
		if (in->lq_percent >= 50U) {
			return scale(green, 15U);
		}
		return scale(amber, 30U);
	case ZLRS_STATE_PAIRING:
		return blink(scale(blue, 60U), now_ms, 250U, 80U);
	case ZLRS_STATE_SEARCHING:
		return blink(scale(cyan, 40U), now_ms, 500U, 80U);
	case ZLRS_STATE_UNPAIRED:
		return blink(scale(yellow, 40U), now_ms, 1000U, 100U);
	case ZLRS_STATE_UNCONFIGURED:
	default:
		return blink(scale(purple, 40U), now_ms, 1000U, 100U);
	}
}

zlrs_led_rgb_t zlrs_led_frame(const zlrs_led_input_t *in, uint32_t now_ms)
{
	zlrs_led_rgb_t frame;

	if (in == NULL) {
		return off;
	}

	/* 1. 配置模式：白，0.5 Hz 慢闪 */
	if (in->config_mode) {
		return blink(scale(white, 30U), now_ms, 2000U, 200U);
	}

	/* 2. 开机自检：前 1.5 秒用角色色（蓝 = TX，绿 = RX），两块板子分得清 */
	if (in->uptime_ms < 1500U) {
		return scale(in->role_tx ? blue : green, 30U);
	}

	/* 3. 失控保护：红，5 Hz 快闪——"没有可用链路"最要紧 */
	if (in->failsafe && in->link_state != ZLRS_STATE_CONNECTED) {
		return blink(scale(red, 60U), now_ms, 200U, 60U);
	}

	/* 4. 链路状态 */
	frame = link_state_frame(in, now_ms);

	/* 5. 丢弃提示：只在已连接时常亮的基础上每 2 秒单闪一次 */
	if (in->link_state == ZLRS_STATE_CONNECTED && in->drop_recent &&
	    single_flash(now_ms, 2000U, 120U)) {
		frame = scale(orange, 60U);
	}

	return frame;
}

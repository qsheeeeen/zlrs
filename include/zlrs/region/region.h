/*
 * 地区配置层：把"在哪儿用"翻译成信道表、功率上限与占空比上限。
 *
 * 法规约束见 doc/03-protocol.md 5.2。这里的取舍：
 *   - 信道表直接复用 FHSS 的 domain（起点/步进/信道数）
 *   - 占空比用一个滑动窗口记账，只统计"发出去的空中时间"，不猜协议细节
 *   - 校验与限制分开：校验在启动时喊出来，限制在每次发射前问一次
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_REGION_REGION_H_
#define ZLRS_REGION_REGION_H_

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/fhss.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const char *name;
	uint8_t domain;      /* ZLRS_DOMAIN_* */
	uint32_t low_hz;     /* 频段边界，校验信道表用 */
	uint32_t high_hz;
	int8_t max_tx_power_dbm;
	uint8_t duty_percent;    /* 0 = 不限制 */
	uint32_t duty_window_ms; /* 占空比统计窗口 */
} zlrs_region_t;

enum {
	ZLRS_REGION_FCC915 = 0,
	ZLRS_REGION_EU868,
	ZLRS_REGION_CN470,
	ZLRS_REGION_COUNT
};

const zlrs_region_t *zlrs_region_get(uint8_t id);

/*
 * 校验"地区 + 信道表 + 发射功率"是否自洽：信道全在边界内、功率不超上限。
 * 返回 0 表示没问题，负值表示有问题（具体值只用于日志）。
 */
int zlrs_region_validate(const zlrs_region_t *region, int8_t tx_power_dbm);

/* 占空比记账：窗口内累计发射的空中时间，超过上限就不再允许发射 */
typedef struct {
	uint32_t window_ms;
	uint32_t window_start_ms;
	uint32_t used_ms;
	uint8_t limit_percent;
	bool limited;
	uint32_t denied; /* 因为占空比被拦下的次数，便于观测 */
} zlrs_duty_t;

void zlrs_duty_init(zlrs_duty_t *duty, const zlrs_region_t *region, uint32_t now_ms);

/* 申请 airtime_ms 的发射机会；允许返回 true 并记账，否则 false（并计数） */
bool zlrs_duty_reserve(zlrs_duty_t *duty, uint32_t airtime_ms, uint32_t now_ms);

/* 当前窗口还剩多少空中时间（ms）；不限制时返回 UINT32_MAX（会顺带翻窗口） */
uint32_t zlrs_duty_remaining_ms(zlrs_duty_t *duty, uint32_t now_ms);

/* 按帧长与速率估算空中时间（ms），向上取整；用于占空比记账 */
uint32_t zlrs_airtime_ms(uint16_t frame_len, uint32_t rate_bps);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_REGION_REGION_H_ */

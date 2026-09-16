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
#include "zlrs/proto/mod.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const char *name;
	uint8_t domain;      /* ZLRS_DOMAIN_* */
	uint8_t band;        /* ZLRS_BAND_*：这个地区要哪种 sub-G 硬件 */
	uint32_t low_hz;     /* 频段边界，校验信道表用 */
	uint32_t high_hz;
	int8_t max_tx_power_dbm;
	uint8_t duty_percent;    /* 0 = 不限制 */
	uint32_t duty_window_ms; /* 占空比统计窗口 */
	uint32_t max_rate_bps;   /* 频段宽度决定的速率上限 */
} zlrs_region_t;

/* sub-G 硬件：LF 是 410–510 MHz，HF 是 850–930 MHz（两个 SKU） */
typedef enum {
	ZLRS_BAND_SUBG_LF = 0,
	ZLRS_BAND_SUBG_HF = 1,
} zlrs_band_t;

enum {
	ZLRS_REGION_FCC915 = 0,
	ZLRS_REGION_EU868,
	ZLRS_REGION_CN470,
	ZLRS_REGION_CN433,
	ZLRS_REGION_COUNT
};

const zlrs_region_t *zlrs_region_get(uint8_t id);

/*
 * 校验"地区 + 本机 sub-G 硬件 + 信道表 + 发射功率"是否自洽：
 * 地区要的 band 得跟板子一致、信道全在边界内、功率不超上限。
 * 返回 0 表示没问题，负值表示有问题（具体值只用于日志）。
 */
int zlrs_region_validate(const zlrs_region_t *region, uint8_t band, int8_t tx_power_dbm);

/* 地区带宽决定的速率上限：配置超过上限就压下来，返回实际可用的速率 */
uint32_t zlrs_region_clamp_rate(const zlrs_region_t *region, uint32_t rate_bps);

/* 调制档也要过地区门：FLRC 压速率，LoRa 把带宽压到地区宽度以内 */
zlrs_mod_cfg_t zlrs_region_clamp_mod(const zlrs_region_t *region, zlrs_mod_cfg_t mod);

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

/* 按帧长与速率估算空中时间（µs，向上取整）；用于占空比记账与帧率规划 */
uint32_t zlrs_airtime_us(uint16_t frame_len, uint32_t rate_bps);

/* 同一笔账按毫秒给（向上取整到 ms），占空比记账用 */
uint32_t zlrs_airtime_ms(uint16_t frame_len, uint32_t rate_bps);

/* 调制感知版本：FLRC 与上面两个等价，LoRa 走 lib/proto/mod.c 的模型 */
uint32_t zlrs_airtime_mod_us(const zlrs_mod_cfg_t *mod, uint16_t frame_len);
uint32_t zlrs_airtime_mod_ms(const zlrs_mod_cfg_t *mod, uint16_t frame_len);

/*
 * 这个地区在给定帧长/速率下，每秒最多允许发多少帧。
 * 不限制占空比时返回 UINT32_MAX。
 */
uint32_t zlrs_region_max_frames_per_s(const zlrs_region_t *region, uint32_t rate_bps,
				      uint16_t frame_len);

/* 同上，按调制档算（LoRa 的一帧比 FLRC 长得多，占空比压得更狠） */
uint32_t zlrs_region_max_frames_per_s_mod(const zlrs_region_t *region,
					  const zlrs_mod_cfg_t *mod, uint16_t frame_len);

/*
 * 控制链路的"时间计划"：帧率、帧周期、每个时隙的预算，以及遥测/数据窗口的
 * 稀疏系数。帧率受两件事约束：地区的占空比上限、以及一帧里至少装得下
 * "发一帧 RC + 收一帧遥测"的空中时间。
 */
typedef struct {
	uint32_t rate_hz;        /* 实际生效的控制帧率 */
	uint32_t frame_period_us;
	uint32_t slot_budget_us; /* 每个时隙的等待预算 */
	uint8_t tlm_every_n;     /* 遥测每 n 帧一次 */
	uint8_t data_every_n;    /* 数据窗口每 n 帧一次 */
	bool clamped_by_duty;    /* 帧率被地区占空比压过 */
	bool clamped_by_airtime; /* 帧率被空中时间压过 */
} zlrs_control_plan_t;

/*
 * 控制链路时间计划的输入。框成一族而不是一串位置参数，是因为"调制档 + 两个帧长
 * + 给另一条链路的预留"这四件事都会随硬件与地区变。
 */
typedef struct {
	const zlrs_region_t *region;
	const zlrs_mod_cfg_t *mod; /* 控制链路的调制档 */
	uint32_t want_hz;          /* 配置档位 */
	uint16_t frame_len;        /* RC 帧字节数（含头与标签） */
	uint16_t tlm_len;          /* 遥测帧字节数；0 = 就用 frame_len */
	uint32_t reserve_us;       /* 同一超帧里留给另一条链路的空中时间 */
} zlrs_plan_req_t;

void zlrs_region_control_plan(const zlrs_plan_req_t *req, zlrs_control_plan_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_REGION_REGION_H_ */

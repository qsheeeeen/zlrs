/*
 * 速率自适应框架：档位间的升降策略（连续窗口 + 保持时间 + 升档余量），
 * 档位表与阈值由实测曲线标定，见 doc/13-roadmap.md。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/mod.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	/*
	 * 档位 = 一整套调制参数：FLRC 的速率，或者 LoRa 的 SF/带宽/编码率。
	 * 见 doc/07-architecture.md 第 3 节——"速率档表从'一个速率'变成'调制 + 参数'"
	 * 就是这里。
	 */
	zlrs_mod_cfg_t mod;
	/* 低于这个 LQ 就守不住这一档，该降档（百分比） */
	uint8_t min_lq_to_stay;
} zlrs_rate_tier_t;

/*
 * 档位表**从快到慢**排列：下标 +1 是更慢的一档（降档），-1 是更快的一档（升档）。
 */

typedef struct {
	const zlrs_rate_tier_t *tiers;
	uint8_t count;
	uint8_t current;      /* 当前档位下标 */
	uint8_t hysteresis;   /* 升档时要比目标档的 min_lq_to_stay 高出的余量 */
	uint8_t windows;      /* 连续多少个窗口满足条件才换档 */
	uint32_t hold_ms;     /* 换档后至少保持这么久 */
	uint32_t last_ms;     /* 上次换档（或初始化）时刻 */
	uint8_t good;         /* 连续"够好"的窗口数 */
	uint8_t bad;          /* 连续"不够好"的窗口数 */
} zlrs_rate_ctl_t;

void zlrs_rate_init(zlrs_rate_ctl_t *ctl, const zlrs_rate_tier_t *tiers, uint8_t count,
		    uint8_t start, uint32_t now_ms);

/*
 * 一个 LQ 窗口结算时调一次。需要换档时返回 true 并把新档位下标写进 *out。
 * 内部按 windows / hold_ms / hysteresis 三个条件过滤。
 */
bool zlrs_rate_update(zlrs_rate_ctl_t *ctl, uint8_t lq_percent, uint32_t now_ms,
		      uint8_t *out);

uint32_t zlrs_rate_current_bps(const zlrs_rate_ctl_t *ctl);

/* 当前档位的完整调制描述（FLRC 与 LoRa 都适用） */
const zlrs_mod_cfg_t *zlrs_rate_current_mod(const zlrs_rate_ctl_t *ctl);

#ifdef __cplusplus
}
#endif


/*
 * 跳频序列，见 doc/09-protocol.md 第 5 节：两端由 fhss_seed 各自算出同一张表，
 * 每块第 0 个时隙固定落在同步信道，其余块内洗牌（等概率使用）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 地区频段：信道表与功率上限由地区配置层选取 */
typedef struct {
	const char *name;
	uint32_t start_hz;     /* 首个信道中心频率 */
	uint32_t step_hz;      /* 信道间隔 */
	uint8_t channel_count; /* 信道数 */
} zlrs_fhss_domain_t;

enum {
	ZLRS_DOMAIN_FCC915 = 0,
	ZLRS_DOMAIN_EU868,
	ZLRS_DOMAIN_CN470,
	ZLRS_DOMAIN_CN433,
	ZLRS_DOMAIN_ISM2400,
	ZLRS_DOMAIN_COUNT
};

extern const zlrs_fhss_domain_t zlrs_fhss_domains[ZLRS_DOMAIN_COUNT];

typedef struct {
	const zlrs_fhss_domain_t *domain;
	uint8_t sequence[ZLRS_FHSS_SEQUENCE_LEN];
	uint16_t sequence_count;
	uint16_t index;
	uint8_t sync_channel;

	/*
	 * 子带混合（doc/09-protocol.md 5.1）：主表之外再挂一张子带表（通常是
	 * sub-G），两张表各自维护索引；哪个时隙落哪个子带由链路层按混合比决定。
	 */
	const zlrs_fhss_domain_t *sub_domain;
	uint8_t sub_sequence[ZLRS_FHSS_SEQUENCE_LEN];
	uint16_t sub_count;
	uint16_t sub_index;
	uint8_t sub_sync_channel;
} zlrs_fhss_t;

void zlrs_fhss_init(zlrs_fhss_t *fhss, uint8_t domain_id, uint32_t seed);

/* 双表：主表 + 子带表，同一个 seed 派生，两端各算一张 */
void zlrs_fhss_init_dual(zlrs_fhss_t *fhss, uint8_t main_domain_id,
			 uint8_t sub_domain_id, uint32_t seed);

/* 子带混合比：每 4 个时隙里落在 sub-G 的个数（0 / 1 / 2 / 4，即 0/25/50/100 %） */
uint8_t zlrs_fhss_mix_slots(uint8_t mix);

/* 子带表是否可用（没配子带时为 false） */
bool zlrs_fhss_sub_ready(const zlrs_fhss_t *fhss);

uint32_t zlrs_fhss_sub_current_hz(const zlrs_fhss_t *fhss);
uint32_t zlrs_fhss_sub_next_hz(zlrs_fhss_t *fhss);
void zlrs_fhss_set_sub_index(zlrs_fhss_t *fhss, uint16_t index);

uint32_t zlrs_fhss_current_hz(const zlrs_fhss_t *fhss);
uint32_t zlrs_fhss_next_hz(zlrs_fhss_t *fhss);

void zlrs_fhss_set_index(zlrs_fhss_t *fhss, uint16_t index);

#ifdef __cplusplus
}
#endif

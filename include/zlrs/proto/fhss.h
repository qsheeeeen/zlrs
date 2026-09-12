/*
 * 跳频序列，见 docs/03-protocol.md 第 5 节。
 *
 *   - 种子来自 pair_key 的派生值 fhss_seed，两端各自算出同一张表
 *   - 每块的第 0 个时隙固定落在同步信道，其余位置块内洗牌，
 *     因此每个频点出现次数相同（法规的等概率使用）
 *   - 频点以 Hz 表示，直接喂给 LR2021 的 SetRfFrequency
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_PROTO_FHSS_H_
#define ZLRS_PROTO_FHSS_H_

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
} zlrs_fhss_t;

/* xorshift32，仅供频点洗牌使用；种子已经过密钥派生 */
void zlrs_fhss_rng_seed(uint32_t seed);
uint32_t zlrs_fhss_rng(void);
uint8_t zlrs_fhss_rng_n(uint8_t max);

/*
 * 按种子生成一张表，返回写入的条目数（频点数的整数倍）。
 * sequence 至少要有 ZLRS_FHSS_SEQUENCE_LEN 字节。
 */
uint16_t zlrs_fhss_build_sequence(uint32_t seed, uint8_t channel_count,
				  uint8_t sync_channel, uint8_t *sequence);

void zlrs_fhss_init(zlrs_fhss_t *fhss, uint8_t domain_id, uint32_t seed);

/* 信道号 -> 频率，越界回落到同步信道 */
uint32_t zlrs_fhss_freq_hz(const zlrs_fhss_t *fhss, uint8_t channel);
uint32_t zlrs_fhss_current_hz(const zlrs_fhss_t *fhss);
uint32_t zlrs_fhss_next_hz(zlrs_fhss_t *fhss);

uint16_t zlrs_fhss_index(const zlrs_fhss_t *fhss);
void zlrs_fhss_set_index(zlrs_fhss_t *fhss, uint16_t index);
bool zlrs_fhss_on_sync(const zlrs_fhss_t *fhss);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_PROTO_FHSS_H_ */

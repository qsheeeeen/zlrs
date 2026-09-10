/*
 * 跳频序列。
 *
 *   - 伪随机数发生器是固定的一阶 LCG：
 *       seed = (214013 * seed + 2531011) mod 2^31
 *   - 频点表按种子打乱
 *   - 每 freq_count 个时隙的第 0 个固定落在 sync 频道，同一块内不出现重复频点
 *
 * TODO(频段表)：下表沿用参考实现的寄存器单位与频点划分，仅作参考。
 * LR2021 的 SetRfFrequency 直接收 Hz，且覆盖 sub-GHz 与 2.4GHz，
 * 需要重做频段表与频率单位（建议 kHz，避免 freq_spread 的 32 位溢出）。
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

#define ZLRS_RADIO_SX127X 0U
#define ZLRS_RADIO_SX1280 1U
#define ZLRS_RADIO_LR2021 2U

typedef struct {
	const char *name;
	uint8_t radio;       /* ZLRS_RADIO_*，决定频率单位 */
	uint32_t freq_start; /* 频率单位 */
	uint32_t freq_stop;  /* 频率单位 */
	uint32_t freq_count; /* 跳频频道数 */
	uint32_t freq_center;
	uint32_t hz_start;   /* 仅用于日志打印 */
	uint32_t hz_stop;
} zlrs_fhss_domain_t;

enum {
	ZLRS_DOMAIN_FCC915 = 0,
	ZLRS_DOMAIN_AU915,
	ZLRS_DOMAIN_EU868,
	ZLRS_DOMAIN_ISM2400,
	ZLRS_DOMAIN_COUNT
};

extern const zlrs_fhss_domain_t zlrs_fhss_domains[ZLRS_DOMAIN_COUNT];

typedef struct {
	const zlrs_fhss_domain_t *domain;
	uint8_t sequence[ZLRS_FHSS_SEQUENCE_LEN];
	uint16_t sequence_count; /* 实际使用的表长，一定是 freq_count 的整数倍 */
	volatile uint8_t ptr;
	uint32_t freq_spread; /* 相邻频道间隔，*256 定点 */
	uint8_t sync_channel;
	int32_t freq_correction; /* 晶振/温漂校正，频率单位 */
} zlrs_fhss_t;

void zlrs_fhss_rng_seed(uint32_t seed);
uint16_t zlrs_fhss_rng(void);
uint8_t zlrs_fhss_rng_n(uint8_t max);

/*
 * 按种子生成一张跳频表，返回写入的条目数。
 * sequence 至少要有 ZLRS_FHSS_SEQUENCE_LEN 字节。
 */
uint16_t zlrs_fhss_build_sequence(uint32_t seed, uint32_t freq_count,
				  uint8_t sync_channel, uint8_t *sequence);

/* 用 UID 派生的种子初始化 */
void zlrs_fhss_init(zlrs_fhss_t *fhss, uint8_t domain_id, uint32_t seed);

uint32_t zlrs_fhss_initial_freq(const zlrs_fhss_t *fhss);
uint32_t zlrs_fhss_next_freq(zlrs_fhss_t *fhss);
uint8_t zlrs_fhss_curr_index(const zlrs_fhss_t *fhss);
void zlrs_fhss_set_index(zlrs_fhss_t *fhss, uint8_t index);
bool zlrs_fhss_on_sync_channel(const zlrs_fhss_t *fhss);

/* 频率单位 -> Hz，仅用于日志 */
uint32_t zlrs_fhss_unit_to_hz(const zlrs_fhss_domain_t *domain, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_PROTO_FHSS_H_ */

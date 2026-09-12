/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/proto/fhss.h"

#include <string.h>

/*
 * 信道划分按各地区的可用带宽与法规信道数取：
 *   - FCC 15.247 要求跳频系统至少 50 个信道
 *   - EU868 受占空比约束，信道数少而间隔宽
 *   - 2.4G ISM 用 1 MHz 间隔铺满整段
 */
const zlrs_fhss_domain_t zlrs_fhss_domains[ZLRS_DOMAIN_COUNT] = {
	[ZLRS_DOMAIN_FCC915] = {.name = "FCC915",
				.start_hz = 902500000U,
				.step_hz = 500000U,
				.channel_count = 50U},
	[ZLRS_DOMAIN_EU868] = {.name = "EU868",
			       .start_hz = 863250000U,
			       .step_hz = 500000U,
			       .channel_count = 13U},
	[ZLRS_DOMAIN_ISM2400] = {.name = "ISM2400",
				 .start_hz = 2402000000U,
				 .step_hz = 1000000U,
				 .channel_count = 79U},
};

static uint32_t rng_state = 0x12345678U;

void zlrs_fhss_rng_seed(uint32_t seed)
{
	/* xorshift32 的零点是不动点，避开它 */
	rng_state = (seed != 0U) ? seed : 0x12345678U;
}

uint32_t zlrs_fhss_rng(void)
{
	uint32_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;

	return x;
}

uint8_t zlrs_fhss_rng_n(uint8_t max)
{
	return (max == 0U) ? 0U : (uint8_t)(zlrs_fhss_rng() % max);
}

uint16_t zlrs_fhss_build_sequence(uint32_t seed, uint8_t channel_count,
				  uint8_t sync_channel, uint8_t *sequence)
{
	uint16_t count;

	if (channel_count == 0U) {
		return 0U;
	}
	if (sync_channel >= channel_count) {
		sync_channel = (uint8_t)(channel_count / 2U);
	}

	count = (uint16_t)((ZLRS_FHSS_SEQUENCE_LEN / channel_count) * channel_count);
	zlrs_fhss_rng_seed(seed);

	/*
	 * 每块的第 0 个位置放同步信道，同步信道原本该出现的位置放 0 号信道，
	 * 这样整张表里每个频点出现次数完全相同。
	 */
	for (uint16_t i = 0; i < count; i++) {
		const uint8_t channel = (uint8_t)(i % channel_count);

		if (channel == 0U) {
			sequence[i] = sync_channel;
		} else if (channel == sync_channel) {
			sequence[i] = 0U;
		} else {
			sequence[i] = channel;
		}
	}

	/* 块内洗牌，位置 0 保持不动 */
	for (uint16_t i = 0; i < count; i++) {
		uint16_t base;
		uint8_t pick;
		uint8_t tmp;

		if ((i % channel_count) == 0U) {
			continue;
		}

		base = (uint16_t)((i / channel_count) * channel_count);
		pick = (uint8_t)(1U + zlrs_fhss_rng_n((uint8_t)(channel_count - 1U)));
		tmp = sequence[i];
		sequence[i] = sequence[base + pick];
		sequence[base + pick] = tmp;
	}

	return count;
}

void zlrs_fhss_init(zlrs_fhss_t *fhss, uint8_t domain_id, uint32_t seed)
{
	if (domain_id >= ZLRS_DOMAIN_COUNT) {
		domain_id = ZLRS_DOMAIN_ISM2400;
	}

	memset(fhss, 0, sizeof(*fhss));
	fhss->domain = &zlrs_fhss_domains[domain_id];
	fhss->sync_channel = (uint8_t)(fhss->domain->channel_count / 2U);
	fhss->index = 0U;
	fhss->sequence_count =
		zlrs_fhss_build_sequence(seed, fhss->domain->channel_count,
					 fhss->sync_channel, fhss->sequence);
}

uint32_t zlrs_fhss_freq_hz(const zlrs_fhss_t *fhss, uint8_t channel)
{
	if (fhss->domain == NULL) {
		return 0U; /* 还没按密钥初始化 */
	}

	if (channel >= fhss->domain->channel_count) {
		channel = fhss->sync_channel;
	}

	return fhss->domain->start_hz + (uint32_t)channel * fhss->domain->step_hz;
}

uint32_t zlrs_fhss_current_hz(const zlrs_fhss_t *fhss)
{
	return zlrs_fhss_freq_hz(fhss, fhss->sequence[fhss->index]);
}

uint32_t zlrs_fhss_next_hz(zlrs_fhss_t *fhss)
{
	if (fhss->sequence_count == 0U) {
		return fhss->domain->start_hz;
	}

	fhss->index = (uint16_t)((fhss->index + 1U) % fhss->sequence_count);

	return zlrs_fhss_current_hz(fhss);
}

uint16_t zlrs_fhss_index(const zlrs_fhss_t *fhss)
{
	return fhss->index;
}

void zlrs_fhss_set_index(zlrs_fhss_t *fhss, uint16_t index)
{
	if (fhss->sequence_count > 0U) {
		fhss->index = (uint16_t)(index % fhss->sequence_count);
	}
}

bool zlrs_fhss_on_sync(const zlrs_fhss_t *fhss)
{
	return fhss->sequence_count > 0U &&
	       fhss->sequence[fhss->index] == fhss->sync_channel;
}

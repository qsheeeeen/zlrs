/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/proto/fhss.h"

#include <string.h>

#define FREQ_SPREAD_SCALE 256U

/*
 * 频段定义。freq_start/stop 沿用参考实现的芯片寄存器单位值，
 * 便于和已有实现对齐；LR2021 的频段表与频率单位待重做，见头文件 TODO。
 */
const zlrs_fhss_domain_t zlrs_fhss_domains[ZLRS_DOMAIN_COUNT] = {
	[ZLRS_DOMAIN_FCC915] = {.name = "FCC915",
				.radio = ZLRS_RADIO_SX127X,
				.freq_start = 14802944U,  /* 903.5 MHz */
				.freq_stop = 15186329U,   /* 926.9 MHz */
				.freq_count = 40U,
				.freq_center = 14991360U, /* 915 MHz */
				.hz_start = 903500000U,
				.hz_stop = 926900000U},
	[ZLRS_DOMAIN_AU915] = {.name = "AU915",
			       .radio = ZLRS_RADIO_SX127X,
			       .freq_start = 14999552U,  /* 915.5 MHz */
			       .freq_stop = 15186329U,   /* 926.9 MHz */
			       .freq_count = 20U,
			       .freq_center = 15089664U, /* 921 MHz */
			       .hz_start = 915500000U,
			       .hz_stop = 926900000U},
	[ZLRS_DOMAIN_EU868] = {.name = "EU868",
			       .radio = ZLRS_RADIO_SX127X,
			       .freq_start = 14143897U,  /* 863.275 MHz */
			       .freq_stop = 14247116U,   /* 869.575 MHz */
			       .freq_count = 13U,
			       .freq_center = 14221312U, /* 868 MHz */
			       .hz_start = 863275000U,
			       .hz_stop = 869575000U},
	[ZLRS_DOMAIN_ISM2400] = {.name = "ISM2G4",
				 .radio = ZLRS_RADIO_SX1280,
				 .freq_start = 12100970U,  /* 2400.4 MHz */
				 .freq_stop = 12499227U,   /* 2479.4 MHz */
				 .freq_count = 80U,
				 .freq_center = 12300462U, /* 2440 MHz */
				 .hz_start = 2400400000U,
				 .hz_stop = 2479400000U},
};

static uint32_t rng_state;

void zlrs_fhss_rng_seed(uint32_t seed)
{
	rng_state = seed;
}

uint16_t zlrs_fhss_rng(void)
{
	const uint32_t m = 2147483648U;
	const uint32_t a = 214013U;
	const uint32_t c = 2531011U;

	rng_state = (a * rng_state + c) % m;
	return (uint16_t)(rng_state >> 16);
}

uint8_t zlrs_fhss_rng_n(uint8_t max)
{
	return (uint8_t)(zlrs_fhss_rng() % max);
}

uint16_t zlrs_fhss_build_sequence(uint32_t seed, uint32_t freq_count,
				  uint8_t sync_channel, uint8_t *sequence)
{
	const uint16_t sequence_count =
		(uint16_t)((ZLRS_FHSS_SEQUENCE_LEN / freq_count) * freq_count);

	zlrs_fhss_rng_seed(seed);

	/* 每块的第 0 个位置放 sync 频道，sync 位置放 0，保持全频道等概率 */
	for (uint16_t i = 0; i < sequence_count; i++) {
		if ((i % freq_count) == 0U) {
			sequence[i] = sync_channel;
		} else if ((i % freq_count) == sync_channel) {
			sequence[i] = 0U;
		} else {
			sequence[i] = (uint8_t)(i % freq_count);
		}
	}

	/* 块内洗牌：把 sync 频道留在原位，其余两两交换 */
	for (uint16_t i = 0; i < sequence_count; i++) {
		if ((i % freq_count) != 0U) {
			uint16_t offset = (uint16_t)((i / freq_count) * freq_count);
			uint8_t idx = (uint8_t)(zlrs_fhss_rng_n((uint8_t)(freq_count - 1U)) + 1U);
			uint8_t tmp = sequence[i];

			sequence[i] = sequence[offset + idx];
			sequence[offset + idx] = tmp;
		}
	}

	return sequence_count;
}

static uint32_t zlrs_fhss_freq_at(const zlrs_fhss_t *fhss, uint8_t channel)
{
	uint32_t freq = fhss->domain->freq_start +
			(fhss->freq_spread * channel / FREQ_SPREAD_SCALE);

	if (fhss->freq_correction >= 0) {
		return freq - (uint32_t)fhss->freq_correction;
	}

	return freq + (uint32_t)(-fhss->freq_correction);
}

void zlrs_fhss_init(zlrs_fhss_t *fhss, uint8_t domain_id, uint32_t seed)
{
	uint16_t count;

	if (domain_id >= ZLRS_DOMAIN_COUNT) {
		domain_id = ZLRS_DOMAIN_ISM2400;
	}

	memset(fhss, 0, sizeof(*fhss));
	fhss->domain = &zlrs_fhss_domains[domain_id];

	fhss->ptr = 0;
	fhss->freq_correction = 0;
	fhss->sync_channel = (uint8_t)(fhss->domain->freq_count / 2U);
	fhss->freq_spread = (fhss->domain->freq_stop - fhss->domain->freq_start) *
			    FREQ_SPREAD_SCALE / (fhss->domain->freq_count - 1U);

	count = zlrs_fhss_build_sequence(seed, fhss->domain->freq_count,
					 fhss->sync_channel, fhss->sequence);
	fhss->sequence_count = count;
}

uint32_t zlrs_fhss_initial_freq(const zlrs_fhss_t *fhss)
{
	return zlrs_fhss_freq_at(fhss, fhss->sync_channel);
}

uint32_t zlrs_fhss_next_freq(zlrs_fhss_t *fhss)
{
	fhss->ptr = (uint8_t)((fhss->ptr + 1U) % fhss->sequence_count);

	return zlrs_fhss_freq_at(fhss, fhss->sequence[fhss->ptr]);
}

uint8_t zlrs_fhss_curr_index(const zlrs_fhss_t *fhss)
{
	return fhss->ptr;
}

void zlrs_fhss_set_index(zlrs_fhss_t *fhss, uint8_t index)
{
	fhss->ptr = (uint8_t)(index % fhss->sequence_count);
}

bool zlrs_fhss_on_sync_channel(const zlrs_fhss_t *fhss)
{
	return fhss->sequence[fhss->ptr] == fhss->sync_channel;
}

uint32_t zlrs_fhss_unit_to_hz(const zlrs_fhss_domain_t *domain, uint32_t value)
{
	switch (domain->radio) {
	case ZLRS_RADIO_SX1280:
		/* 步进 198.3642578125 Hz = 52 MHz / 2^18 */
		return (uint32_t)(((uint64_t)value * 1983642578ULL) / 10000000ULL);
	case ZLRS_RADIO_LR2021:
		/* SetRfFrequency 直接以 Hz 为单位 */
		return value;
	case ZLRS_RADIO_SX127X:
	default:
		/* 步进 61.03515625 Hz = 32 MHz / 2^19 */
		return (uint32_t)(((uint64_t)value * 6103515625ULL) / 100000000ULL);
	}
}

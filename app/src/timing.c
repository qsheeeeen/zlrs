/* SPDX-License-Identifier: Apache-2.0 */
#include "timing.h"

static const uint32_t bounds_us[ZLRS_TIMING_BUCKETS] = {
	0U,   2U,   4U,    8U,    16U,   32U,   64U,   128U,  256U,       384U,
	512U, 768U, 1024U, 1536U, 2048U, 3072U, 4096U, 8192U, UINT32_MAX,
};

void zlrs_timing_record(zlrs_timing_histogram_t *histogram, uint32_t us)
{
	if (histogram->samples == UINT32_MAX) {
		return;
	}
	uint8_t bucket = 0U;
	while (us > bounds_us[bucket]) {
		bucket++;
	}
	histogram->buckets[bucket]++;
	if (histogram->samples == 0U || us < histogram->min_us) {
		histogram->min_us = us;
	}
	if (us > histogram->max_us) {
		histogram->max_us = us;
	}
	histogram->total_us += us;
	histogram->samples++;
}

uint32_t zlrs_timing_percentile_bound(const zlrs_timing_histogram_t *histogram, uint8_t percent)
{
	if (histogram->samples == 0U || percent == 0U || percent > 100U) {
		return 0U;
	}
	const uint32_t target = ((uint64_t)histogram->samples * percent + 99U) / 100U;
	uint32_t count = 0U;
	for (uint8_t bucket = 0U; bucket < ZLRS_TIMING_BUCKETS; bucket++) {
		count += histogram->buckets[bucket];
		if (count >= target) {
			return bounds_us[bucket] < histogram->max_us ? bounds_us[bucket]
								     : histogram->max_us;
		}
	}
	return histogram->max_us;
}

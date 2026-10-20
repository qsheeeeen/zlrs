/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZLRS_TIMING_BUCKETS 19U

/* Fixed storage, no sample allocation. Counts saturate together at UINT32_MAX. */
typedef struct {
	uint32_t buckets[ZLRS_TIMING_BUCKETS];
	uint32_t samples;
	uint32_t min_us;
	uint32_t max_us;
	uint64_t total_us;
} zlrs_timing_histogram_t;

void zlrs_timing_record(zlrs_timing_histogram_t *histogram, uint32_t us);
/* Bucket upper bound, clamped to the observed maximum. Empty/invalid requests return 0.
 * Percent must be 1..100; these are bounds, not exact measured percentiles.
 */
uint32_t zlrs_timing_percentile_bound(const zlrs_timing_histogram_t *histogram, uint8_t percent);

#ifdef __cplusplus
}
#endif

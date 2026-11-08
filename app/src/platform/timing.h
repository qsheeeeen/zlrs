/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t last;
	uint32_t max;
} zlrs_timing_t;

static inline void zlrs_timing_record(zlrs_timing_t *timing, uint32_t sample)
{
	timing->last = sample;
	if (sample > timing->max) {
		timing->max = sample;
	}
}

/* A condition must remain true continuously; zero is a valid starting timestamp. */
typedef struct {
	uint32_t since_ms;
	bool active;
} zlrs_hold_t;

static inline bool zlrs_hold_reached(zlrs_hold_t *hold, bool condition, uint32_t now_ms,
				     uint32_t duration_ms)
{
	if (!condition) {
		hold->active = false;
	} else if (!hold->active) {
		hold->since_ms = now_ms;
		hold->active = true;
	}
	return condition && (uint32_t)(now_ms - hold->since_ms) >= duration_ms;
}

/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zlrs/link/slot_clock.h"

void zlrs_slot_clock_init(zlrs_slot_clock_t *clock, uint32_t origin_us, uint32_t period_us)
{
	*clock = (zlrs_slot_clock_t){
		.period_us = period_us < 4U ? 4U : period_us,
		.origin_us = origin_us,
		.next_us = origin_us,
	};
}

uint32_t zlrs_slot_clock_offset(uint32_t period_us, uint8_t slot)
{
	return (uint32_t)(((uint64_t)period_us * slot) / 4U);
}

bool zlrs_slot_clock_due(const zlrs_slot_clock_t *clock, uint32_t now_us)
{
	return (int32_t)(now_us - clock->next_us) >= 0;
}

bool zlrs_slot_clock_launch_ready(uint32_t now_us, uint32_t target_us, uint32_t lead_us)
{
	return lead_us > 0U && lead_us <= INT32_MAX &&
	       (int32_t)(target_us - now_us) >= (int32_t)lead_us;
}

uint32_t zlrs_slot_clock_advance(zlrs_slot_clock_t *clock, uint32_t now_us)
{
	uint32_t boundaries = 0U;

	while (zlrs_slot_clock_due(clock, now_us)) {
		if (clock->slot == 0U) {
			clock->published_origin_us = clock->origin_us;
			clock->frame++;
		}
		if (++clock->slot == 4U) {
			clock->slot = 0U;
			clock->origin_us += clock->period_us + (uint32_t)clock->shift_us;
			clock->shift_us = 0;
		}
		clock->next_us =
			clock->origin_us + zlrs_slot_clock_offset(clock->period_us, clock->slot);
		boundaries++;
	}
	return boundaries;
}

void zlrs_slot_clock_shift(zlrs_slot_clock_t *clock, int32_t shift_us)
{
	const int32_t limit = (int32_t)(clock->period_us / 16U);

	clock->shift_us = shift_us > limit ? limit : (shift_us < -limit ? -limit : shift_us);
}

uint32_t zlrs_slot_clock_next_origin(const zlrs_slot_clock_t *clock)
{
	/* Slot 3 already installs the next origin, including the queued phase correction. */
	return clock->slot == 0U ? clock->origin_us
				 : clock->origin_us + clock->period_us + (uint32_t)clock->shift_us;
}

uint32_t zlrs_slot_clock_scan_delay_ms(uint32_t cadence_us, uint8_t sync_every_n, bool acquired)
{
	const uint32_t minimum_ms = 300U;

	if (!acquired || sync_every_n == 0U) {
		return minimum_ms;
	}
	const uint64_t gap_us = (uint64_t)cadence_us * (2U * sync_every_n + 1U);
	const uint32_t gap_ms = (uint32_t)((gap_us + 999U) / 1000U);

	return gap_ms > minimum_ms ? gap_ms : minimum_ms;
}

uint32_t zlrs_slot_clock_timestamp_ms(uint64_t uptime_offset_us, uint64_t timer_us,
				      uint32_t deadline_us)
{
	const int32_t delta_us = (int32_t)(deadline_us - (uint32_t)timer_us);

	return (uint32_t)((uptime_offset_us + timer_us + delta_us) / 1000U);
}

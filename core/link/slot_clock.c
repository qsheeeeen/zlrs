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

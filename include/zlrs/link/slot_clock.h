/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A 1 MHz wrapping clock. Comparisons require gaps shorter than INT32_MAX us. */
typedef struct {
	uint32_t period_us;
	uint32_t origin_us;
	uint32_t next_us;
	uint32_t frame;
	uint32_t published_origin_us;
	uint8_t slot;
	int32_t shift_us;
} zlrs_slot_clock_t;

void zlrs_slot_clock_init(zlrs_slot_clock_t *clock, uint32_t origin_us, uint32_t period_us);
uint32_t zlrs_slot_clock_offset(uint32_t period_us, uint8_t slot);
bool zlrs_slot_clock_due(const zlrs_slot_clock_t *clock, uint32_t now_us);
uint32_t zlrs_slot_clock_advance(zlrs_slot_clock_t *clock, uint32_t now_us);
void zlrs_slot_clock_shift(zlrs_slot_clock_t *clock, int32_t shift_us);
uint32_t zlrs_slot_clock_next_origin(const zlrs_slot_clock_t *clock);
/* Map a nearby wrapping deadline using a fixed uptime epoch. The same deadline
 * must give the same millisecond at preparation and activation, including at wrap. */
uint32_t zlrs_slot_clock_timestamp_ms(uint64_t uptime_offset_us, uint64_t timer_us,
				      uint32_t deadline_us);
/* Acquisition scans promptly; an acquired clock tolerates two idle SYNC periods.
 * cadence_us is the interval between link A slots, not the four-window timer frame. */
uint32_t zlrs_slot_clock_scan_delay_ms(uint32_t cadence_us, uint8_t sync_every_n, bool acquired);

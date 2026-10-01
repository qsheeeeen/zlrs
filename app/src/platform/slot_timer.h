/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

struct zlrs_slot_timer_stats {
	uint32_t boundaries;
	uint32_t irq_late_us_max;
	uint32_t missed_boundaries;
};

int zlrs_slot_timer_init(void);
uint32_t zlrs_slot_timer_now_us(void);
void zlrs_slot_timer_start(uint32_t period_us, uint32_t prepare_us, void (*wake)(void));
void zlrs_slot_timer_stop(void);
void zlrs_slot_timer_shift(int32_t shift_us);
void zlrs_slot_timer_deadline(uint32_t deadline_us);
void zlrs_slot_timer_wait_frame(uint32_t *start_us, uint32_t *frame);
uint32_t zlrs_slot_timer_next_frame_us(void);
void zlrs_slot_timer_wait_until(uint32_t deadline_us);
void zlrs_slot_timer_stats(struct zlrs_slot_timer_stats *out);

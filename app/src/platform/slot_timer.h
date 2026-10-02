/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

struct zlrs_slot_timer_stats {
	uint32_t boundaries;
	uint32_t irq_late_us_max;
	uint32_t missed_boundaries;
};

struct zlrs_slot_timer_wait {
	uint32_t irq_us;
	uint32_t wake_us;
	bool armed;
};

int zlrs_slot_timer_init(void);
uint32_t zlrs_slot_timer_now_us(void);
uint32_t zlrs_slot_timer_uptime_ms(uint32_t deadline_us);
void zlrs_slot_timer_start(uint32_t period_us, uint32_t prepare_us, void (*wake)(void));
void zlrs_slot_timer_stop(void);
void zlrs_slot_timer_shift(int32_t shift_us);
void zlrs_slot_timer_deadline(uint32_t deadline_us);
void zlrs_slot_timer_wait_frame(uint32_t *start_us, uint32_t *frame);
uint32_t zlrs_slot_timer_next_frame_us(void);
void zlrs_slot_timer_wait_until(uint32_t deadline_us, struct zlrs_slot_timer_wait *timing);
/* The callback runs in a RAM-resident zero-latency ISR: no kernel or driver APIs.
 * A normal alarm-2 interrupt notifies the RF owner after the callback returns. */
void zlrs_slot_timer_launch_at(uint32_t target_us, void (*fire)(uint32_t irq_us));
void zlrs_slot_timer_cancel_launch(void);
void zlrs_slot_timer_stats(struct zlrs_slot_timer_stats *out);

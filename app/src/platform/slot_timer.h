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
uint32_t zlrs_slot_timer_now_ms(void);
uint32_t zlrs_slot_timer_uptime_ms(uint32_t deadline_us);
void zlrs_slot_timer_start(uint32_t period_us, uint32_t prepare_us, void (*wake)(void));
void zlrs_slot_timer_stop(void);
/* Preserve frame count and insert a shared guard before the next physical frame. */
void zlrs_slot_timer_reconfigure(uint32_t period_us, uint32_t prepare_us, uint32_t guard_us);
void zlrs_slot_timer_hold_next_frame(uint32_t guard_us);
void zlrs_slot_timer_shift(int32_t shift_us);
void zlrs_slot_timer_deadline(uint32_t deadline_us);
void zlrs_slot_timer_wait_frame(uint32_t *start_us, uint32_t *frame);
uint32_t zlrs_slot_timer_next_frame_us(void);
void zlrs_slot_timer_wait_until(uint32_t deadline_us, struct zlrs_slot_timer_wait *timing);
/* The callback runs in a RAM-resident zero-latency ISR: no kernel or driver APIs.
 * A normal alarm-2 interrupt notifies the RF owner after the callback returns. */
#define ZLRS_SLOT_TIMER_LAUNCH_LEAD_US     25U
/* The radio rejects later callbacks rather than launching outside the slot budget. */
#define ZLRS_SLOT_TIMER_LAUNCH_MAX_LATE_US 10U
/* False means the deadline cannot be armed in advance; no late software trigger. */
bool zlrs_slot_timer_launch_at(uint32_t target_us, void (*fire)(uint32_t irq_us));
bool zlrs_slot_timer_launch_ready(uint32_t now_us, uint32_t target_us, uint32_t lead_us);
uint32_t zlrs_slot_timer_scan_delay_ms(uint32_t cadence_us, uint16_t sync_every_n, bool acquired);

/* Move roughly one third of a frame between periodic SYNC opportunities. */
static inline uint32_t zlrs_slot_timer_scan_step_us(uint32_t cadence_us, uint16_t sync_every_n)
{
	const uint32_t frames = sync_every_n > 0U ? sync_every_n : 1U;
	const uint32_t step = cadence_us / (3U * frames);

	return step > 0U ? step : 1U;
}

void zlrs_slot_timer_cancel_launch(void);
void zlrs_slot_timer_stats(struct zlrs_slot_timer_stats *out);

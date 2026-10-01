/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * TIMER1 alarm 0 advances absolute slot boundaries independently of radio work.
 * The ISR only publishes timing and wakes the thread; SPI stays on the link thread.
 */
#include "platform/slot_timer.h"
#include "zlrs/link/slot_clock.h"

#include <hardware/structs/timer.h>
#include <hardware/ticks.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#define SLOT_TIMER_NODE DT_ALIAS(zlrs_slot_timer)
#define SLOT_TIMER_IRQ  DT_IRQ_BY_IDX(SLOT_TIMER_NODE, 0, irq)

static timer_hw_t *const timer = (timer_hw_t *)DT_REG_ADDR(SLOT_TIMER_NODE);
static const struct device *const clk = DEVICE_DT_GET(DT_CLOCKS_CTLR(SLOT_TIMER_NODE));
static const struct reset_dt_spec reset = RESET_DT_SPEC_GET(SLOT_TIMER_NODE);
static K_SEM_DEFINE(frame_sem, 0, 1);
static K_SEM_DEFINE(delay_sem, 0, 1);
static zlrs_slot_clock_t clock;
static bool running;
static void (*wake_thread)(void);
static struct zlrs_slot_timer_stats stats;

uint32_t zlrs_slot_timer_now_us(void)
{
	return timer->timerawl;
}

static void slot_timer_isr(const void *arg)
{
	ARG_UNUSED(arg);
	timer->intr = 1U;
	timer->intf &= ~1U;
	if (!running) {
		return;
	}

	const uint32_t now = zlrs_slot_timer_now_us();
	const uint32_t before = clock.frame;

	if (zlrs_slot_clock_due(&clock, now)) {
		stats.irq_late_us_max = MAX(stats.irq_late_us_max, now - clock.next_us);
		const uint32_t elapsed = zlrs_slot_clock_advance(&clock, now);

		stats.boundaries += elapsed;
		stats.missed_boundaries += elapsed - 1U;
		if (clock.frame != before) {
			k_sem_give(&frame_sem);
		}
	}

	timer->alarm[0] = clock.next_us;
	/* An interrupt between the comparison and the alarm write must not lose an alarm. */
	if ((int32_t)(zlrs_slot_timer_now_us() - clock.next_us) >= 0) {
		timer->intf |= 1U;
	}

	if (wake_thread != NULL) {
		wake_thread();
	}
}

static void delay_isr(const void *arg)
{
	ARG_UNUSED(arg);
	timer->intr = 2U;
	timer->intf &= ~2U;
	k_sem_give(&delay_sem);
}

static void deadline_isr(const void *arg)
{
	ARG_UNUSED(arg);
	timer->intr = 4U;
	timer->intf &= ~4U;
	if (running && wake_thread != NULL) {
		wake_thread();
	}
}

void zlrs_slot_timer_deadline(uint32_t deadline_us)
{
	const unsigned int key = irq_lock();

	timer->armed = 4U;
	timer->intr = 4U;
	timer->intf &= ~4U;
	timer->alarm[2] = deadline_us;
	timer->inte |= 4U;
	if ((int32_t)(zlrs_slot_timer_now_us() - deadline_us) >= 0) {
		timer->intf |= 4U;
	}
	irq_unlock(key);
}

void zlrs_slot_timer_shift(int32_t shift_us)
{
	const unsigned int key = irq_lock();

	zlrs_slot_clock_shift(&clock, shift_us);
	irq_unlock(key);
}

int zlrs_slot_timer_init(void)
{
	uint32_t rate;
	int rc;

	if (!device_is_ready(clk)) {
		return -ENODEV;
	}
	rc = clock_control_on(clk, (clock_control_subsys_t)DT_CLOCKS_CELL(SLOT_TIMER_NODE, clk_id));
	if (rc < 0) {
		return rc;
	}
	rc = clock_control_get_rate(
		clk, (clock_control_subsys_t)DT_CLOCKS_CELL(SLOT_TIMER_NODE, clk_id), &rate);
	if (rc < 0 || rate % 1000000U != 0U) {
		return -EINVAL;
	}
	rc = reset_line_toggle_dt(&reset);
	if (rc < 0) {
		return rc;
	}
	tick_start(TICK_TIMER1, rate / 1000000U);
	timer->source = 0U;
	timer->pause = 0U;
	IRQ_CONNECT(SLOT_TIMER_IRQ, DT_IRQ_BY_IDX(SLOT_TIMER_NODE, 0, priority), slot_timer_isr,
		    NULL, 0);
	IRQ_CONNECT(DT_IRQ_BY_IDX(SLOT_TIMER_NODE, 1, irq),
		    DT_IRQ_BY_IDX(SLOT_TIMER_NODE, 1, priority), delay_isr, NULL, 0);
	IRQ_CONNECT(DT_IRQ_BY_IDX(SLOT_TIMER_NODE, 2, irq),
		    DT_IRQ_BY_IDX(SLOT_TIMER_NODE, 2, priority), deadline_isr, NULL, 0);
	irq_enable(DT_IRQ_BY_IDX(SLOT_TIMER_NODE, 2, irq));
	irq_enable(SLOT_TIMER_IRQ);
	irq_enable(DT_IRQ_BY_IDX(SLOT_TIMER_NODE, 1, irq));
	return 0;
}

void zlrs_slot_timer_stop(void)
{
	const unsigned int key = irq_lock();

	running = false;
	timer->inte = 0U;
	timer->armed = 7U;
	timer->intr = 7U;
	timer->intf = 0U;
	irq_unlock(key);
	k_sem_reset(&frame_sem);
	k_sem_reset(&delay_sem);
}

void zlrs_slot_timer_start(uint32_t period_us, void (*wake)(void))
{
	zlrs_slot_timer_stop();
	const unsigned int key = irq_lock();

	zlrs_slot_clock_init(&clock, zlrs_slot_timer_now_us() + 1000U, period_us);
	wake_thread = wake;
	running = true;
	timer->alarm[0] = clock.next_us;
	timer->inte = 1U;
	irq_unlock(key);
}

void zlrs_slot_timer_wait_frame(uint32_t *start_us, uint32_t *frame)
{
	(void)k_sem_take(&frame_sem, K_FOREVER);
	const unsigned int key = irq_lock();

	*start_us = clock.published_origin_us;
	*frame = clock.frame;
	/* An ISR between taking the token and this snapshot may publish a newer frame. */
	k_sem_reset(&frame_sem);
	irq_unlock(key);
}

void zlrs_slot_timer_wait_until(uint32_t deadline_us)
{
	const unsigned int key = irq_lock();

	if ((int32_t)(deadline_us - zlrs_slot_timer_now_us()) <= 0) {
		irq_unlock(key);
		return;
	}
	k_sem_reset(&delay_sem);
	timer->armed = 2U;
	timer->intr = 2U;
	timer->alarm[1] = deadline_us;
	timer->inte |= 2U;
	if ((int32_t)(zlrs_slot_timer_now_us() - deadline_us) >= 0) {
		timer->intf |= 2U;
	}
	irq_unlock(key);
	(void)k_sem_take(&delay_sem, K_FOREVER);
}

void zlrs_slot_timer_stats(struct zlrs_slot_timer_stats *out)
{
	const unsigned int key = irq_lock();

	*out = stats;
	irq_unlock(key);
}

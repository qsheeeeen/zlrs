/*
 * Radio port layer: the concrete backends behind zlrs_radio_ops_t (contract: 07-architecture 6.2).
 * CONFIG_ZLRS_APP_RADIO picks one: radio_rac.c (RAC/LR2021) or radio_null.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "zlrs/link/link.h"

/* Operation tables handed to zlrs_link_init() */
const zlrs_radio_ops_t *zlrs_radio_control_ops(void);
const zlrs_radio_ops_t *zlrs_radio_data_ops(void);

/* Bring the backend up and wire completion into both links (negative on failure) */
int zlrs_radio_port_init(zlrs_link_t *control, zlrs_link_t *data);

/*
 * Wait for one transaction; negative on timeout.
 * Microseconds matter: at 500 Hz control a slot only budgets a few hundred microseconds.
 * With the radio off there is nothing to wait for, so it returns 0 at once.
 */
int zlrs_radio_port_wait_us(uint32_t timeout_us);

/* Hand finished transactions back to the link layer (called on the link thread) */
void zlrs_radio_port_drain(void);

/* Abort a pending transaction after a slot overrun, so a late completion cannot leak into the next */
void zlrs_radio_port_abort(void);

/* Same time base as the radio backend (k_uptime_get_32) */
uint32_t zlrs_radio_port_now_ms(void);

/*
 * Bench counters of the port itself: how many transactions ran, and how often the radio had an
 * IRQ latched when nothing was being waited for. A non-zero stale count means an event from an
 * earlier transaction survived into the next slot (frames get completed early or the planner
 * finds an interrupt with no task).
 */
typedef struct {
	uint32_t transactions;
	uint32_t stale_irq;      /* transactions that started with a radio IRQ still latched */
	uint32_t stale_irq_bits; /* the latched bits of the last such IRQ */
	uint32_t late_irq;       /* engine calls that found the planner's IRQ flag set with no wait */
	uint32_t overruns;       /* transactions aborted because the slot budget ran out */
	/*
	 * Receive windows that came back long before their timeout: a window that closes early either
	 * heard something (fine) or was cut short by an event that belonged to another transaction,
	 * which is how frames go missing without a single error counter moving.
	 */
	uint32_t rx_windows;
	uint32_t rx_early;      /* windows that ended in well under the programmed timeout */
	uint32_t rx_us_last;    /* duration of the last completed transaction, microseconds */
	uint32_t tx_us_last;
} zlrs_radio_diag_t;

void zlrs_radio_port_diag(zlrs_radio_diag_t *out);

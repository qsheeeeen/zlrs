/*
 * Radio port layer: the concrete backends behind zlrs_radio_ops_t (contract: 07-architecture 6.2).
 * The concrete backend is radio_rac.c (USP/RAC straight to the LR2021).
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
 * Does the port have an interrupt waiting to be handled? A caller that pumps the engine in its own
 * loop uses this to decide between "pump again right away" and "sleep and let the radio work":
 * the planner needs several passes per transaction and a sleep between them costs milliseconds.
 */
bool zlrs_radio_port_irq_pending(void);

/*
 * Bench switch for the data link's backend: on = direct FLRC driver, off = RAC planner. Kept in
 * RAM on purpose, so a fault in the new path costs a reset and the board comes back on the path
 * it booted with.
 */
bool zlrs_radio_direct_enabled(void);
void zlrs_radio_direct_set(bool enabled);
/* Apply a requested switch at a point where no transaction is in flight (once per frame) */
void zlrs_radio_direct_apply(void);

/* Bench: how often the switch was tried and how often it had to wait for the data link */
void zlrs_radio_direct_stats(uint32_t *calls, uint32_t *deferred);

/* Bench: how often polling the status register found the completion, and what it read */
void zlrs_radio_direct_irq_stats(uint32_t *poll_hits, uint32_t *last_status);

/* Bench: how often a transaction that reported nothing had to be recovered with a standby cycle */
uint32_t zlrs_radio_direct_resets(void);

/* True while the direct backend has a transaction in flight (it completes by polling, not by wait) */
bool zlrs_radio_port_direct_busy(void);

/* Per-transaction outcome of the direct backend: what the radio reported, if anything */
void zlrs_radio_direct_counts(uint32_t *tx_done, uint32_t *rx_pkt, uint32_t *rx_timeout,
			      uint32_t *no_status, uint32_t *last_nonzero);

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
	/*
	 * Where the time in a transaction goes. `submit` is measured from the moment the link hands
	 * the request over to the port to the moment the RAC accepted it: the drain (one SPI status
	 * read), whatever the driver does per request, and the RAC submit itself (radio wake-up plus
	 * programming over SPI). `wait` is the time from there to the completion callback, so a
	 * transaction is submit + wait and the two numbers say which half to attack.
	 */
	uint32_t submit_drain_us;
	/*
	 * The same status read again, right after the first one. The first read of a transaction is
	 * what wakes a sleeping radio; if this second one is far cheaper, that difference is the
	 * wake-up and the price of the planner sleeping the radio between transactions.
	 */
	uint32_t submit_drain2_us;
	uint32_t submit_rac_us;
	uint32_t submit_us_max;
	uint32_t wait_us_max;
} zlrs_radio_diag_t;

void zlrs_radio_port_diag(zlrs_radio_diag_t *out);

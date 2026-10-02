/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "zlrs/link/link.h"

int zlrs_radio_init(void);
int zlrs_radio_wait_us(uint32_t timeout_us);
bool zlrs_radio_irq_pending(void);
/* Process an IRQ; true only when the transaction is finished (or already idle). */
bool zlrs_radio_drain(void);
void zlrs_radio_abort(void);
/* Called by the timer ISR to end waits at a slot boundary. No SPI from the ISR. */
void zlrs_radio_wake(void);
void zlrs_radio_slot(uint32_t tx_us, uint32_t end_us);
/* Prepare modem and FIFO before the timer-controlled launch; no TX in prepare(). */
int zlrs_radio_prepare(zlrs_link_t *link, const zlrs_radio_req_t *req);
/* Fill the separate TX FIFO while RX continues. Bytes must stay immutable until prepare(). */
int zlrs_radio_preload_tx(const uint8_t *buf, uint16_t size);
int zlrs_radio_launch(void);
int zlrs_radio_launch_at(uint32_t target_us);
/* SET_TX is polled even when FIFO transfers use DMA. */
#define ZLRS_RADIO_STARTUP_US       500U
#define ZLRS_RADIO_PHASE_STARTUP_US 250U

uint32_t zlrs_radio_airtime_us(const zlrs_mod_cfg_t *mod, uint16_t len);
bool zlrs_radio_rx_timing(uint32_t *done_us, uint32_t *airtime_us);

struct zlrs_radio_diag {
	uint32_t transactions;
	uint32_t stale_irq;
	uint32_t stale_irq_bits;
	uint32_t overruns;
	uint32_t rx_windows;
	uint32_t rx_us_last;
	uint32_t tx_us_last;
	uint32_t submit_us_last;
	uint32_t submit_us_max;
	uint32_t tx_prepare_us_last[4]; /* standby/IRQ, modem, FIFO, FS/BUSY */
	uint32_t tx_preloaded;
	uint32_t tx_preload_used;
	uint32_t tx_preload_us_last;
	uint32_t tx_preload_us_max;
	uint32_t launch_cmd_us_last;
	uint32_t launch_cmd_us_max;
	uint32_t tx_launch_cmd_us_last;
	uint32_t tx_launch_cmd_us_max;
	uint32_t preload_lead_us_last;
	uint32_t launch_jitter_us_last;
	uint32_t launch_jitter_us_max;
	uint32_t tx_startup_us_last;
	uint32_t tx_startup_us_max;
	uint32_t rx_header_errors;
	uint32_t rx_crc_errors;
	uint32_t rx_length_errors;
	uint32_t rx_payload_errors;
	uint32_t rx_restarted;
	uint32_t rx_expired;
	uint32_t rx_incomplete;
	uint32_t tx_expired;
	uint32_t irq_service_us_last;
	uint32_t irq_service_us_max;
	uint32_t late_starts;
	uint32_t prepare_late_starts;
	uint32_t trigger_late_starts;
	uint32_t launch_irq_late_us_last;
	uint32_t launch_irq_late_us_max;
	uint32_t launch_wake_us_last;
	uint32_t launch_wake_us_max;
	uint32_t late_trigger_irq;
	uint32_t late_trigger_wake;
	uint32_t rx_finish_us_last;
	uint32_t rx_finish_us_max;
	uint32_t rx_protocol_us_last;
	uint32_t rx_protocol_us_max;
	uint32_t launch_late_us_last;
	uint32_t launch_late_us_max;
	uint32_t tx_done;
	uint32_t rx_packets;
	uint32_t empty_irq;
	uint32_t irq_last;
	uint16_t chip_errors;
	uint32_t chip_error_freq_hz;
};
typedef struct zlrs_radio_diag zlrs_radio_diag_t;
void zlrs_radio_diag(zlrs_radio_diag_t *out);

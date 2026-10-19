/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "zlrs/link/radio.h"

int zlrs_lr2021_init(void);

enum {
	ZLRS_LR2021_STATUS_TEMP = 1U << 0,
	ZLRS_LR2021_STATUS_XOSC_TEMP = 1U << 1,
	ZLRS_LR2021_STATUS_VBAT = 1U << 2,
	ZLRS_LR2021_STATUS_VERSION = 1U << 3,
	ZLRS_LR2021_STATUS_STATE = 1U << 4,
	ZLRS_LR2021_STATUS_ERRORS = 1U << 5,
};

typedef struct {
	int16_t temperature_centi_c;
	int16_t xosc_temperature_centi_c;
	uint16_t vbat_mv;
	uint8_t version_major;
	uint8_t version_minor;
	uint8_t chip_mode;
	uint8_t command_status;
	uint8_t reset_status;
	uint8_t valid;
	uint16_t errors;
	uint32_t irq;
	uint32_t sampled_ms;
} zlrs_lr2021_status_t;

/* The upstream getter returns a signed 13-bit Q8.5 value in a uint16_t. */
static inline int16_t zlrs_lr2021_temperature_centi_c(uint16_t q5)
{
	const int32_t signed_q5 = (q5 & 0x1000U) != 0U ? (int32_t)q5 - 8192 : q5;

	return (int16_t)(signed_q5 * 100 / 32);
}

/* RF owner only, after init and while no request/launch is active. Never clears
 * chip errors, IRQs or reset information. Failed fields have their valid bit clear. */
void zlrs_lr2021_read_status(zlrs_lr2021_status_t *out);
int zlrs_lr2021_wait_us(uint32_t timeout_us);
bool zlrs_lr2021_irq_pending(void);

/* One hardware completion. */
typedef struct {
	int status; /* 0 on success, negative errno on failure */
	uint16_t size;
	int16_t rssi_dbm;
	int8_t snr_db;
	uint32_t done_us; /* GPIO IRQ timestamp */
	uint32_t airtime_us;
	uint32_t completed_ms; /* time after FIFO read, before protocol processing */
} zlrs_lr2021_result_t;

/* Process pending hardware IRQs; true returns one completed transaction.
 * false means idle, no IRQ, or an unfinished transaction. */
bool zlrs_lr2021_process_irq(zlrs_lr2021_result_t *out);
void zlrs_lr2021_abort(void);
/* Called by the timer ISR to end waits at a slot boundary. No SPI from the ISR. */
void zlrs_lr2021_wake(void);
void zlrs_lr2021_slot(uint32_t tx_us, uint32_t end_us);
/* Prepare modem and FIFO before the timer-controlled launch; no TX in prepare(). */
int zlrs_lr2021_prepare(const zlrs_radio_req_t *req);
/* Fill the separate TX FIFO while RX continues. Bytes must stay immutable until prepare(). */
int zlrs_lr2021_preload_tx(const uint8_t *buf, uint16_t size);
int zlrs_lr2021_launch(void);
int zlrs_lr2021_launch_at(uint32_t target_us);
/* SET_TX is polled even when FIFO transfers use DMA. */
#define ZLRS_LR2021_STARTUP_US       500U
#define ZLRS_LR2021_PHASE_STARTUP_US 250U

uint32_t zlrs_lr2021_airtime_us(const zlrs_mod_cfg_t *mod, uint16_t len);

#define ZLRS_LR2021_TX_FIFO_PREFIX 128U

/* FLRC consumes this prefix while the owner appends the authenticated tail. */
static inline uint16_t zlrs_lr2021_tx_fifo_prefix(uint16_t size)
{
	return size > ZLRS_LR2021_TX_FIFO_PREFIX ? ZLRS_LR2021_TX_FIFO_PREFIX : size;
}

/* FIFO reads consume a prefix. Validate the total before subtracting it or
 * forming the next buffer pointer; leave remaining unchanged on invalid input. */
static inline bool zlrs_lr2021_fifo_remaining(uint16_t capacity, uint16_t copied, uint32_t total,
					      uint16_t *remaining)
{
	if (total > capacity || copied > total) {
		return false;
	}
	*remaining = (uint16_t)(total - copied);
	return true;
}

struct zlrs_lr2021_diag {
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
	uint32_t fs_reused;
	uint32_t tx_preloaded;
	uint32_t tx_preload_used;
	uint32_t tx_preload_us_last;
	uint32_t tx_preload_us_max;
	uint32_t tx_fifo_tails;
	uint32_t tx_fifo_bytes;
	uint32_t tx_fifo_errors;
	uint32_t tx_fifo_underflows;
	uint32_t tx_fifo_ready_us_last;
	uint32_t tx_fifo_ready_us_max;
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
	uint32_t rx_fifo_bytes;
	uint32_t rx_fifo_us_last;
	uint32_t rx_fifo_us_max;
	uint32_t launch_late_us_last;
	uint32_t launch_late_us_max;
	uint32_t tx_done;
	uint32_t rx_packets;
	uint32_t empty_irq;
	uint32_t irq_last;
	uint16_t chip_errors;
	uint32_t chip_error_freq_hz;
};
typedef struct zlrs_lr2021_diag zlrs_lr2021_diag_t;
void zlrs_lr2021_diag(zlrs_lr2021_diag_t *out);

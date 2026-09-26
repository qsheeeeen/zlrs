/*
 * Radio abstraction: the link layer only says what to send and how long to listen, the port
 * layer turns that into hardware (USP/RAC on target, a simulated radio in host tests).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/limits.h"
#include "zlrs/proto/mod.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t freq_hz; /* carrier frequency */
	/*
	 * Modulation: an FLRC rate or LoRa SF/bandwidth/coding rate/preamble, see
	 * doc/07-architecture.md 3. The port layer dispatches on modulation into RAC's flrc and lora
	 * parameter groups.
	 */
	zlrs_mod_cfg_t mod;
	uint8_t sync_slot; /* sync word slot: 1 control, 2 data, 3 pairing */
	uint8_t sync_word[ZLRS_SYNC_WORD_LEN];
	bool is_tx;
	int8_t tx_power_dbm;
	uint16_t size;        /* bytes to send */
	uint16_t max_rx_size; /* receive buffer cap */
	uint16_t timeout_ms;  /* receive window */
	uint8_t *buf;         /* shared TX/RX buffer, received data lands here */
} zlrs_radio_req_t;

typedef struct {
	/* Submit one transaction, 0 on success; the result comes back via zlrs_link_complete() */
	int (*submit)(void *ctx, const zlrs_radio_req_t *req);
	/* 32-bit random number for session and pairing nonces; the port layer uses the TRNG */
	uint32_t (*random)(void *ctx);
	void *ctx;
} zlrs_radio_ops_t;

#ifdef __cplusplus
}
#endif

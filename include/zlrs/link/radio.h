/*
 * Radio request produced by the protocol state machine. The caller executes it directly
 * through the LR2021 driver on target or the simulated radio in host tests.
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
	 * doc/06-architecture.md 3. The application configures the corresponding RAL parameters.
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

#ifdef __cplusplus
}
#endif

/*
 * Over-the-air telemetry payload: the RX reports the link quality it measures and the TX turns
 * it into a CRSF statistics frame (0x14). CRSF frames are not reused as payload: that format
 * belongs to the ground side (address and CRC included). The numbers are uplink (RX measuring
 * TX->RX); downlink is measured by the TX itself, see docs/design/07-architecture.md 3.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZLRS_TLM_LEN 8U

/* flag bits */
#define ZLRS_TLM_FLAG_PAIRED  0x0001U
#define ZLRS_TLM_FLAG_SESSION 0x0002U

typedef struct {
	uint8_t lq_percent;   /* uplink quality 0-100 */
	uint8_t rssi_neg_dbm; /* uplink RSSI as negated dBm (CRSF convention) */
	int8_t snr_db;        /* uplink SNR; the radio stack cannot provide it under FLRC, 0 */
	uint8_t rf_mode;      /* RX's current rate tier id (see rf_mode in crsf.h) */
	uint8_t tx_power;     /* RX's TX power tier id (see the power tiers in crsf.h) */
	uint8_t state;        /* RX link state (ZLRS_STATE_*), for diagnostics */
	uint16_t flags;       /* ZLRS_TLM_FLAG_* */
} zlrs_tlm_t;

void zlrs_tlm_encode(uint8_t out[ZLRS_TLM_LEN], const zlrs_tlm_t *tlm);

/* Returns false on a wrong payload length and leaves the outputs untouched */
bool zlrs_tlm_decode(const uint8_t *in, uint16_t len, zlrs_tlm_t *tlm);

#ifdef __cplusplus
}
#endif

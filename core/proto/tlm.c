/*
 * Telemetry payload codec, see include/zlrs/proto/tlm.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/proto/tlm.h"

#include <stddef.h>

void zlrs_tlm_encode(uint8_t out[ZLRS_TLM_LEN], const zlrs_tlm_t *tlm)
{
	out[0] = tlm->lq_percent;
	out[1] = tlm->rssi_neg_dbm;
	out[2] = (uint8_t)tlm->snr_db;
	out[3] = tlm->rf_mode;
	out[4] = tlm->tx_power;
	out[5] = tlm->state;
	out[6] = (uint8_t)(tlm->flags & 0xFFU);
	out[7] = (uint8_t)(tlm->flags >> 8);
}

bool zlrs_tlm_decode(const uint8_t *in, uint16_t len, zlrs_tlm_t *tlm)
{
	if (in == NULL || tlm == NULL || len < ZLRS_TLM_LEN) {
		return false;
	}

	tlm->lq_percent = in[0];
	tlm->rssi_neg_dbm = in[1];
	tlm->snr_db = (int8_t)in[2];
	tlm->rf_mode = in[3];
	tlm->tx_power = in[4];
	tlm->state = in[5];
	tlm->flags = (uint16_t)((uint16_t)in[6] | ((uint16_t)in[7] << 8));

	return true;
}

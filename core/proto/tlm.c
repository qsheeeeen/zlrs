/* SPDX-License-Identifier: Apache-2.0 */
#include "zlrs/proto/tlm.h"
#include <stddef.h>
void zlrs_tlm_encode(uint8_t out[ZLRS_TLM_LEN], const zlrs_tlm_t *tlm)
{
	out[0] = tlm->rssi_neg_dbm;
	out[1] = tlm->rssi_percent;
	out[2] = tlm->lq_percent;
	out[3] = (uint8_t)tlm->snr_db;
	out[4] = tlm->rf_power_dbm;
	out[5] = tlm->fps;
	out[6] = tlm->rssi_ant2_neg_dbm;
	out[7] = tlm->active_antenna;
}
bool zlrs_tlm_decode(const uint8_t *in, uint16_t len, zlrs_tlm_t *tlm)
{
	if (in == NULL || tlm == NULL || len != ZLRS_TLM_LEN) {
		return false;
	}
	tlm->rssi_neg_dbm = in[0];
	tlm->rssi_percent = in[1];
	tlm->lq_percent = in[2];
	tlm->snr_db = (int8_t)in[3];
	tlm->rf_power_dbm = in[4];
	tlm->fps = in[5];
	tlm->rssi_ant2_neg_dbm = in[6];
	tlm->active_antenna = in[7];
	return true;
}

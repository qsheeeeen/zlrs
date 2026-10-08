/* CRSF 0x1D Link Statistics TX payload, carried unchanged over the air.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZLRS_TLM_LEN 8U

typedef struct {
	uint8_t rssi_neg_dbm;
	uint8_t rssi_percent;
	uint8_t lq_percent;
	int8_t snr_db;
	uint8_t rf_power_dbm;      /* Aircraft receiver power, unsigned dBm. */
	uint8_t fps;               /* RF frames per second divided by 10. */
	uint8_t rssi_ant2_neg_dbm; /* Zero when antenna 2 is absent. */
	uint8_t active_antenna;
} zlrs_tlm_t;
void zlrs_tlm_encode(uint8_t out[ZLRS_TLM_LEN], const zlrs_tlm_t *tlm);
bool zlrs_tlm_decode(const uint8_t *in, uint16_t len, zlrs_tlm_t *tlm);

#ifdef __cplusplus
}
#endif

/* SPDX-License-Identifier: Apache-2.0 */
#include "power.h"

/* Product policy. RSSI/LQ thresholds need calibration against measured packet-error curves. */
enum {
	POWER_STEP_DB = 3,
	RSSI_LOW_DBM = -95,
	RSSI_HIGH_DBM = -70,
	LQ_LOW = 90,
	LQ_HIGH = 98,
	QUALITY_WINDOWS = 3,
	HOLD_MS = 2000,
	FEEDBACK_TIMEOUT_MS = 1000,
};

void zlrs_power_init(zlrs_power_t *power, int8_t ceiling_dbm, int8_t floor_dbm, uint32_t now_ms)
{
	*power = (zlrs_power_t){
		.floor_dbm = floor_dbm < ceiling_dbm ? floor_dbm : ceiling_dbm,
		.ceiling_dbm = ceiling_dbm,
		.current_dbm = ceiling_dbm,
		.last_change_ms = now_ms,
	};
}

int8_t zlrs_power_update(zlrs_power_t *power, int16_t rssi_dbm, uint8_t lq_percent,
			 uint32_t feedback_ms, uint32_t now_ms)
{
	if (feedback_ms == 0U || (uint32_t)(now_ms - feedback_ms) >= FEEDBACK_TIMEOUT_MS) {
		power->good = 0U;
		power->bad = 0U;
		if (power->current_dbm != power->ceiling_dbm) {
			power->current_dbm = power->ceiling_dbm;
			power->last_change_ms = now_ms;
			power->fallbacks++;
		}
		return power->current_dbm;
	}
	if (feedback_ms == power->last_feedback_ms) {
		return power->current_dbm;
	}
	power->last_feedback_ms = feedback_ms;
	if ((uint32_t)(now_ms - power->last_change_ms) < HOLD_MS) {
		power->good = 0U;
		power->bad = 0U;
		return power->current_dbm;
	}

	if (rssi_dbm < RSSI_LOW_DBM || lq_percent < LQ_LOW) {
		power->bad++;
		power->good = 0U;
	} else if (rssi_dbm > RSSI_HIGH_DBM && lq_percent >= LQ_HIGH) {
		power->good++;
		power->bad = 0U;
	} else {
		power->good = 0U;
		power->bad = 0U;
		return power->current_dbm;
	}

	int next = power->current_dbm;
	if (power->bad >= QUALITY_WINDOWS) {
		next += POWER_STEP_DB;
		if (next > power->ceiling_dbm) {
			next = power->ceiling_dbm;
		}
		power->raises += next != power->current_dbm;
	} else if (power->good >= QUALITY_WINDOWS) {
		next -= POWER_STEP_DB;
		if (next < power->floor_dbm) {
			next = power->floor_dbm;
		}
		power->lowers += next != power->current_dbm;
	} else {
		return power->current_dbm;
	}

	if (next != power->current_dbm) {
		power->last_change_ms = now_ms;
	}
	power->current_dbm = (int8_t)next;
	power->good = 0U;
	power->bad = 0U;
	return power->current_dbm;
}

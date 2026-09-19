/*
 * Dynamic TX power policy, see include/zlrs/link/power.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/link/power.h"

#include <stddef.h>

static int8_t clamp_dbm(const zlrs_power_ctl_t *ctl, int value)
{
	if (value < ctl->min_dbm) {
		return ctl->min_dbm;
	}
	if (value > ctl->max_dbm) {
		return ctl->max_dbm;
	}

	return (int8_t)value;
}

void zlrs_power_init(zlrs_power_ctl_t *ctl, int8_t configured_dbm, int8_t floor_dbm,
		     uint32_t now_ms)
{
	ctl->max_dbm = configured_dbm;
	ctl->min_dbm = (floor_dbm < configured_dbm) ? floor_dbm : configured_dbm;
	ctl->step_db = 3;
	/* Placeholder thresholds: calibrate them against measured RSSI/LQ vs. PER curves */
	ctl->rssi_low_dbm = -95;
	ctl->rssi_high_dbm = -70;
	ctl->lq_low = 90U;
	ctl->lq_high = 98U;
	ctl->windows = 3U;
	ctl->hold_ms = 2000U;
	ctl->tlm_timeout_ms = 1000U;

	/* Start at the ceiling: without telemetry there is no reason to lower power */
	ctl->current_dbm = configured_dbm;
	ctl->good = 0U;
	ctl->bad = 0U;
	ctl->last_change_ms = now_ms;
	ctl->last_tlm_ms = now_ms;
	ctl->tlm_seen = false;
	ctl->raises = 0U;
	ctl->lowers = 0U;
	ctl->fallbacks = 0U;
}

int8_t zlrs_power_current_dbm(const zlrs_power_ctl_t *ctl)
{
	return ctl->current_dbm;
}

bool zlrs_power_update(zlrs_power_ctl_t *ctl, bool valid, int16_t rssi_dbm,
		       uint8_t lq_percent, uint32_t now_ms, int8_t *out_dbm)
{
	if (valid) {
		/* Fresh telemetry: this call judges quality, not the timeout */
		ctl->tlm_seen = true;
		ctl->last_tlm_ms = now_ms;
	} else {
		/* Polling path: only here is the telemetry timeout checked */
		if (!ctl->tlm_seen ||
		    (uint32_t)(now_ms - ctl->last_tlm_ms) < ctl->tlm_timeout_ms) {
			return false;
		}

		ctl->good = 0U;
		ctl->bad = 0U;
		if (ctl->current_dbm == ctl->max_dbm) {
			return false;
		}

		/* Back to the ceiling first; saving power can wait for telemetry to return */
		ctl->current_dbm = ctl->max_dbm;
		ctl->fallbacks++;
		ctl->last_change_ms = now_ms;
		if (out_dbm != NULL) {
			*out_dbm = ctl->current_dbm;
		}

		return true;
	}

	/* No windows during the hold time: counting restarts when an adjustment is allowed */
	if ((uint32_t)(now_ms - ctl->last_change_ms) < ctl->hold_ms) {
		ctl->good = 0U;
		ctl->bad = 0U;
		return false;
	}

	if (rssi_dbm < ctl->rssi_low_dbm || lq_percent < ctl->lq_low) {
		ctl->bad++;
		ctl->good = 0U;
	} else if (rssi_dbm > ctl->rssi_high_dbm && lq_percent >= ctl->lq_high) {
		ctl->good++;
		ctl->bad = 0U;
	} else {
		ctl->good = 0U;
		ctl->bad = 0U;
		return false;
	}

	if (ctl->bad >= ctl->windows) {
		const int8_t next = clamp_dbm(ctl, (int)ctl->current_dbm + ctl->step_db);

		if (next == ctl->current_dbm) {
			ctl->bad = 0U;
			return false; /* already at the ceiling */
		}
		ctl->current_dbm = next;
		ctl->raises++;
	} else if (ctl->good >= ctl->windows) {
		const int8_t next = clamp_dbm(ctl, (int)ctl->current_dbm - ctl->step_db);

		if (next == ctl->current_dbm) {
			ctl->good = 0U;
			return false; /* already at the floor */
		}
		ctl->current_dbm = next;
		ctl->lowers++;
	} else {
		return false;
	}

	ctl->last_change_ms = now_ms;
	ctl->good = 0U;
	ctl->bad = 0U;

	if (out_dbm != NULL) {
		*out_dbm = ctl->current_dbm;
	}

	return true;
}

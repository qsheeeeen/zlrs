/* SPDX-License-Identifier: Apache-2.0 */
#include "power.h"

enum {
	POWER_STEP_DB = 3,
	RSSI_HYSTERESIS_DB = 6,
	RSSI_FILTER_MS = 500,
	RAISE_HOLD_MS = 500,
	LOWER_HOLD_MS = 2000,
	LQ_RECOVERY_MS = 5000,
};

void zlrs_power_init(zlrs_power_t *p, int8_t ceiling_dbm, int8_t floor_dbm, uint32_t now_ms)
{
	*p = (zlrs_power_t){
		.floor_dbm = floor_dbm < ceiling_dbm ? floor_dbm : ceiling_dbm,
		.ceiling_dbm = ceiling_dbm,
		.current_dbm = ceiling_dbm,
		.last_change_ms = now_ms,
	};
}

int8_t zlrs_power_recover(zlrs_power_t *p, uint32_t now_ms)
{
	p->good = p->bad = p->filtering = p->lq_hold = false;
	p->last_feedback_ms = 0U;
	p->lq_generation = 0U;
	if (p->current_dbm != p->ceiling_dbm) {
		p->current_dbm = p->ceiling_dbm;
		p->last_change_ms = now_ms;
		p->fallbacks++;
	}
	return p->current_dbm;
}

static int8_t change(zlrs_power_t *p, int next, uint32_t now_ms)
{
	if (next > p->ceiling_dbm) {
		next = p->ceiling_dbm;
	}
	if (next < p->floor_dbm) {
		next = p->floor_dbm;
	}
	if (next != p->current_dbm) {
		p->raises += next > p->current_dbm;
		p->lowers += next < p->current_dbm;
		p->last_change_ms = now_ms;
		p->current_dbm = (int8_t)next;
		p->good = p->bad = p->filtering = false;
	}
	return p->current_dbm;
}

int8_t zlrs_power_update(zlrs_power_t *p, const zlrs_power_observation_t *o)
{
	const uint32_t timeout = o->feedback_timeout_ms ? o->feedback_timeout_ms : 1000U;
	const uint32_t settle = o->settle_ms > 500U ? o->settle_ms : 500U;
	if (o->feedback_ms == 0U || (uint32_t)(o->now_ms - o->feedback_ms) >= timeout ||
	    o->rssi_dbm >= 0 || o->rssi_dbm < -255 ||
	    (o->lq_percent > 100U && o->lq_percent != 127U)) {
		return zlrs_power_recover(p, o->now_ms);
	}
	if (p->last_feedback_ms != 0U && (int32_t)(o->feedback_ms - p->last_feedback_ms) <= 0) {
		return p->current_dbm;
	}
	const uint32_t elapsed = o->feedback_ms - p->last_feedback_ms;
	if (p->last_feedback_ms != 0U && elapsed >= timeout) {
		zlrs_power_recover(p, o->now_ms);
	}
	if (p->last_feedback_ms == 0U) {
		p->last_change_ms = o->now_ms;
		/* A received cached window establishes a baseline after feedback loss. */
		p->lq_generation = o->lq_generation;
	}
	p->last_feedback_ms = o->feedback_ms;

	if (o->lq_generation != p->lq_generation) {
		const bool new_window = (uint32_t)(o->lq_generation - p->lq_generation) == 1U;
		p->lq_generation = o->lq_generation;
		if (new_window && o->lq_percent < 95U) {
			p->lq_hold = true;
			p->lq_hold_ms = o->now_ms;
			p->lq_hold_duration_ms =
				o->lq_percent < 50U ? LQ_RECOVERY_MS : LOWER_HOLD_MS;
			p->good = false;
			return change(p,
				      o->lq_percent < 50U ? p->ceiling_dbm
							  : p->current_dbm + POWER_STEP_DB,
				      o->now_ms);
		}
	}
	if (p->lq_hold && (uint32_t)(o->now_ms - p->lq_hold_ms) >= p->lq_hold_duration_ms) {
		p->lq_hold = false;
	}
	/* TX can have been encoded before the preceding RX. Discard those early samples. */
	if ((int32_t)(o->feedback_ms - p->last_change_ms) < (int32_t)settle) {
		p->good = p->bad = false;
		return p->current_dbm;
	}
	const int32_t sample = (int32_t)o->rssi_dbm * 256;
	if (!p->filtering) {
		p->rssi_q8 = sample;
		p->filtering = true;
	} else {
		p->rssi_q8 += (int32_t)(((int64_t)(sample - p->rssi_q8) * elapsed) /
					(RSSI_FILTER_MS + elapsed));
	}
	if (p->rssi_q8 < (int32_t)o->rssi_floor_dbm * 256) {
		p->good = false;
		if (!p->bad) {
			p->bad = true;
			p->bad_since_ms = o->now_ms;
		}
		if ((uint32_t)(o->now_ms - p->bad_since_ms) >= RAISE_HOLD_MS) {
			return change(p, p->current_dbm + POWER_STEP_DB, o->now_ms);
		}
	} else if (p->rssi_q8 > ((int32_t)o->rssi_floor_dbm + RSSI_HYSTERESIS_DB) * 256 &&
		   !p->lq_hold) {
		p->bad = false;
		if (!p->good) {
			p->good = true;
			p->good_since_ms = o->now_ms;
		}
		if ((uint32_t)(o->now_ms - p->good_since_ms) >= LOWER_HOLD_MS) {
			return change(p, p->current_dbm - POWER_STEP_DB, o->now_ms);
		}
	} else {
		p->good = p->bad = false;
	}
	return p->current_dbm;
}

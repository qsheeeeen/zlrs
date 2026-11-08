/* SPDX-License-Identifier: Apache-2.0 */
#include "power.h"
#include "band.h"
#include <zephyr/sys/util.h>

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
	p->good.active = p->bad.active = p->filtering = p->lq_hold = false;
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
	next = CLAMP(next, p->floor_dbm, p->ceiling_dbm);
	if (next != p->current_dbm) {
		p->raises += next > p->current_dbm;
		p->lowers += next < p->current_dbm;
		p->last_change_ms = now_ms;
		p->current_dbm = (int8_t)next;
		p->good.active = p->bad.active = p->filtering = false;
	}
	return p->current_dbm;
}

int8_t zlrs_power_update(zlrs_power_t *p, const zlrs_link_observation_t *o)
{
	const zlrs_link_status_t *link = o->link;
	const uint32_t timeout = MAX(1000U, o->settle_ms + 500U);
	const uint32_t settle = MAX(500U, o->settle_ms);
	const int16_t rssi_floor =
		zlrs_band_rssi_floor(link->cfg.region != ZLRS_REGION_ISM2400, link->cfg.phy);
	if (link->last_peer_feedback_ms == 0U ||
	    (uint32_t)(o->now_ms - link->last_peer_feedback_ms) >= timeout ||
	    link->peer_rssi_dbm >= 0 || link->peer_rssi_dbm < -255 ||
	    (link->peer_lq_percent > 100U && link->peer_lq_percent != 127U)) {
		return zlrs_power_recover(p, o->now_ms);
	}
	if (p->last_feedback_ms != 0U &&
	    (int32_t)(link->last_peer_feedback_ms - p->last_feedback_ms) <= 0) {
		return p->current_dbm;
	}
	const uint32_t elapsed = link->last_peer_feedback_ms - p->last_feedback_ms;
	if (p->last_feedback_ms != 0U && elapsed >= timeout) {
		zlrs_power_recover(p, o->now_ms);
	}
	if (p->last_feedback_ms == 0U) {
		p->last_change_ms = o->now_ms;
		/* A received cached window establishes a baseline after feedback loss. */
		p->lq_generation = link->peer_lq_generation;
	}
	p->last_feedback_ms = link->last_peer_feedback_ms;

	if (link->peer_lq_generation != p->lq_generation) {
		const bool new_window =
			(uint32_t)(link->peer_lq_generation - p->lq_generation) == 1U;
		p->lq_generation = link->peer_lq_generation;
		if (new_window && link->peer_lq_percent < 95U) {
			p->lq_hold = true;
			p->lq_hold_ms = o->now_ms;
			p->lq_hold_duration_ms =
				link->peer_lq_percent < 50U ? LQ_RECOVERY_MS : LOWER_HOLD_MS;
			p->good.active = false;
			return change(p,
				      link->peer_lq_percent < 50U ? p->ceiling_dbm
								  : p->current_dbm + POWER_STEP_DB,
				      o->now_ms);
		}
	}
	if (p->lq_hold && (uint32_t)(o->now_ms - p->lq_hold_ms) >= p->lq_hold_duration_ms) {
		p->lq_hold = false;
	}
	/* TX can have been encoded before the preceding RX. Discard those early samples. */
	if ((int32_t)(link->last_peer_feedback_ms - p->last_change_ms) < (int32_t)settle) {
		p->good.active = p->bad.active = false;
		return p->current_dbm;
	}
	const int32_t sample = (int32_t)link->peer_rssi_dbm * 256;
	if (!p->filtering) {
		p->rssi_q8 = sample;
		p->filtering = true;
	} else {
		p->rssi_q8 += (int32_t)(((int64_t)(sample - p->rssi_q8) * elapsed) /
					(RSSI_FILTER_MS + elapsed));
	}
	const bool weak = p->rssi_q8 < (int32_t)rssi_floor * 256;
	const bool strong =
		p->rssi_q8 > ((int32_t)rssi_floor + RSSI_HYSTERESIS_DB) * 256 && !p->lq_hold;
	if (zlrs_hold_reached(&p->bad, weak, o->now_ms, RAISE_HOLD_MS)) {
		return change(p, p->current_dbm + POWER_STEP_DB, o->now_ms);
	}
	if (zlrs_hold_reached(&p->good, strong, o->now_ms, LOWER_HOLD_MS)) {
		return change(p, p->current_dbm - POWER_STEP_DB, o->now_ms);
	}
	return p->current_dbm;
}

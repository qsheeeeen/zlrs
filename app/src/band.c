/* SPDX-License-Identifier: Apache-2.0 */
#include "band.h"

static uint32_t maximum(uint32_t a, uint32_t b) { return a > b ? a : b; }

void zlrs_band_policy_init(zlrs_band_policy_t *p, bool automatic, bool master, bool subg,
			   uint32_t duty_window_ms, uint32_t now_ms)
{
	*p = (zlrs_band_policy_t){
	    .automatic = automatic,
	    .master = master,
	    .subg = subg,
	    .loss_ms = maximum(1500U, duty_window_ms ? duty_window_ms + 2000U : 0U),
	    .dwell_ms =
		maximum(master ? 20000U : 4000U,
			duty_window_ms ? (master ? 2U : 1U) * (duty_window_ms + 2000U) : 0U),
	    .entered_ms = now_ms,
	    .contact_ms = now_ms};
}

void zlrs_band_policy_changed(zlrs_band_policy_t *p, bool subg, bool probe, uint32_t now_ms,
			      uint32_t last_rx_ms)
{
	p->subg = subg;
	p->probing = probe;
	zlrs_band_policy_resume(p, now_ms, last_rx_ms);
}

void zlrs_band_policy_resume(zlrs_band_policy_t *p, uint32_t now_ms, uint32_t last_rx_ms)
{
	p->entered_ms = p->contact_ms = now_ms;
	p->last_rx_ms = last_rx_ms;
	p->stable = p->bad = p->good = false;
}

zlrs_band_action_t zlrs_band_policy_update(zlrs_band_policy_t *p, const zlrs_band_observation_t *o)
{
	if (!p->automatic || !o->paired || o->pairing) {
		zlrs_band_policy_resume(p, o->now_ms, o->last_rx_ms);
		return ZLRS_BAND_KEEP;
	}
	if (o->last_rx_ms != p->last_rx_ms) {
		p->last_rx_ms = o->last_rx_ms;
		p->contact_ms = o->now_ms;
	}
	if (o->acquiring) {
		p->stable = p->bad = p->good = false;
		if ((uint32_t)(o->now_ms - p->entered_ms) >=
		    (p->probing ? p->loss_ms : p->dwell_ms)) {
			return ZLRS_BAND_SEARCH;
		}
		return ZLRS_BAND_KEEP;
	}
	if ((uint32_t)(o->now_ms - p->contact_ms) >= p->loss_ms) {
		return ZLRS_BAND_SEARCH;
	}
	if (!o->connected || o->planned) {
		p->bad = p->good = false;
		return ZLRS_BAND_KEEP;
	}
	const bool fresh = o->feedback_ms != 0U &&
			   (uint32_t)(o->now_ms - o->feedback_ms) < p->loss_ms &&
			   o->rssi_dbm != 0 && o->peer_rssi_dbm != 0;
	const bool poor = fresh && (o->lq_percent < 95U || o->peer_lq_percent < 95U ||
				    o->rssi_dbm < -95 || o->peer_rssi_dbm < -95);
	if (!fresh || poor) {
		p->good = p->stable = false;
	} else {
		if (!p->stable) {
			p->stable_ms = o->now_ms;
			p->stable = true;
		}
		if (!p->good) {
			p->good_ms = o->now_ms;
			p->good = true;
		}
	}
	if (poor) {
		if (!p->bad) {
			p->bad_ms = o->now_ms;
			p->bad = true;
		}
	} else {
		p->bad = false;
	}
	if (!p->master) {
		return ZLRS_BAND_KEEP;
	}
	if (!p->subg && p->bad && (uint32_t)(o->now_ms - p->bad_ms) >= 5000U) {
		return ZLRS_BAND_PLAN;
	}
	if (p->probing && p->good && (uint32_t)(o->now_ms - p->good_ms) >= 5000U) {
		p->probing = false;
	}
	if (p->subg && p->stable && (uint32_t)(o->now_ms - p->stable_ms) >= 60000U) {
		return ZLRS_BAND_PLAN;
	}
	return ZLRS_BAND_KEEP;
}

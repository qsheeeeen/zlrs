/* SPDX-License-Identifier: Apache-2.0 */
#include "band.h"

static uint32_t maximum(uint32_t a, uint32_t b)
{
	return a > b ? a : b;
}

void zlrs_band_policy_init(zlrs_band_policy_t *p, bool automatic, bool leader, bool subg,
			   uint32_t duty_window_ms, uint32_t now_ms)
{
	*p = (zlrs_band_policy_t){
		.automatic = automatic,
		.leader = leader,
		.subg = subg,
		.loss_ms = maximum(1500U, duty_window_ms ? duty_window_ms + 2000U : 0U),
		/* Leader holds one profile longer than a complete Follower sweep. */
		.dwell_ms = (leader ? ZLRS_PHY_AUTO_COUNT + 2U : 1U) *
			    maximum(4000U, duty_window_ms ? duty_window_ms + 2000U : 0U),
		.entered_ms = now_ms,
		.contact_ms = now_ms};
}

void zlrs_band_policy_changed(zlrs_band_policy_t *p, bool subg, zlrs_phy_t phy, bool probe,
			      uint32_t now_ms, uint32_t last_rx_ms)
{
	p->subg = subg;
	p->phy = subg ? ZLRS_PHY_FLRC_2600 : phy;
	p->probing = probe;
	zlrs_band_policy_resume(p, now_ms, last_rx_ms);
}

static zlrs_band_action_t next_profile(zlrs_band_policy_t *p, zlrs_band_action_t action)
{
	p->target_subg = false;
	if (p->subg) {
		/* Probe the most robust 2.4 GHz profile before recovering throughput. */
		p->target_phy = action == ZLRS_BAND_PLAN ? ZLRS_PHY_LORA_SF7 : ZLRS_PHY_FLRC_2600;
	} else if (p->phy + 1U < ZLRS_PHY_AUTO_COUNT) {
		p->target_phy = p->phy + 1U;
	} else {
		p->target_subg = true;
		p->target_phy = ZLRS_PHY_FLRC_2600;
	}
	return action;
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
			return next_profile(p, ZLRS_BAND_SEARCH);
		}
		return ZLRS_BAND_KEEP;
	}
	if ((uint32_t)(o->now_ms - p->contact_ms) >= p->loss_ms) {
		return next_profile(p, ZLRS_BAND_SEARCH);
	}
	if (!o->connected || o->planned) {
		p->bad = p->good = false;
		return ZLRS_BAND_KEEP;
	}
	const bool fresh = o->feedback_ms != 0U &&
			   (uint32_t)(o->now_ms - o->feedback_ms) < p->loss_ms &&
			   o->rssi_dbm != 0 && o->peer_rssi_dbm != 0;
	/* Initial 10 dB receive margins, to be calibrated with equal-length packets.
	 * Slow profiles must not inherit the fast profile's absolute RSSI cutoff. */
	static const int16_t floors[] = {-89, -94, -98, -103, -106};
	const int16_t floor = p->subg ? -95 : floors[p->phy < ZLRS_PHY_AUTO_COUNT ? p->phy : 0U];
	const bool poor = fresh && (o->lq_percent < 95U || o->peer_lq_percent < 95U ||
				    o->rssi_dbm < floor || o->peer_rssi_dbm < floor);
	const int16_t upgrade_floor = p->subg || p->phy == 0U ? floor : floors[p->phy - 1U] + 6;
	const bool upgrade = fresh && !poor && o->lq_percent >= 99U && o->peer_lq_percent >= 99U &&
			     o->rssi_dbm >= upgrade_floor && o->peer_rssi_dbm >= upgrade_floor;
	if (!fresh || poor) {
		p->good = p->stable = false;
	} else {
		if (!upgrade) {
			p->stable = false;
		} else if (!p->stable) {
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
	if (p->probing && p->good && (uint32_t)(o->now_ms - p->good_ms) >= 5000U) {
		p->probing = false;
	}
	if (!p->leader) {
		return ZLRS_BAND_KEEP;
	}
	if (!p->subg && p->bad && (uint32_t)(o->now_ms - p->bad_ms) >= 2000U) {
		return next_profile(p, ZLRS_BAND_PLAN);
	}
	if (p->stable && (uint32_t)(o->now_ms - p->stable_ms) >= 60000U) {
		if (p->subg) {
			return next_profile(p, ZLRS_BAND_PLAN);
		}
		if (p->phy > 0U) {
			p->target_subg = false;
			p->target_phy = p->phy - 1U;
			return ZLRS_BAND_PLAN;
		}
	}
	return ZLRS_BAND_KEEP;
}

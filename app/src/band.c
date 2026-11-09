/* SPDX-License-Identifier: Apache-2.0 */
#include "band.h"
#include <zephyr/sys/util.h>

void zlrs_band_policy_init(zlrs_band_policy_t *p, zlrs_role_t role, uint32_t duty_window_ms,
			   uint32_t now_ms)
{
	*p = (zlrs_band_policy_t){
		.loss_ms = MAX(1500U, duty_window_ms ? duty_window_ms + 2000U : 0U),
		/* Leader holds one profile longer than a complete Follower sweep. */
		.dwell_ms = (role == ZLRS_ROLE_LEADER ? ZLRS_PHY_AUTO_COUNT + 2U : 1U) *
			    MAX(4000U, duty_window_ms ? duty_window_ms + 2000U : 0U),
		.entered_ms = now_ms,
		.contact_ms = now_ms};
}

static zlrs_band_action_t next_profile(zlrs_band_policy_t *p, const zlrs_link_cfg_t *cfg,
				       zlrs_band_action_t action)
{
	p->target_subg = false;
	if (cfg->region != ZLRS_REGION_ISM2400) {
		/* Probe the most robust 2.4 GHz profile before recovering throughput. */
		p->target_phy = action == ZLRS_BAND_PLAN ? ZLRS_PHY_LORA_SF7 : ZLRS_PHY_FLRC_2600;
	} else if (cfg->phy + 1U < ZLRS_PHY_AUTO_COUNT) {
		p->target_phy = cfg->phy + 1U;
	} else {
		p->target_subg = true;
		p->target_phy = ZLRS_PHY_FLRC_2600;
	}
	return action;
}

void zlrs_band_policy_reset(zlrs_band_policy_t *p, bool probe, uint32_t now_ms, uint32_t last_rx_ms)
{
	p->probing = probe;
	p->entered_ms = p->contact_ms = now_ms;
	p->last_rx_ms = last_rx_ms;
	p->stable.active = p->bad.active = p->good.active = false;
	p->power_boosted = false;
	p->maximum = false;
	p->quality[0].windows = p->quality[1].windows = 0U;
}

zlrs_band_action_t zlrs_band_policy_update(zlrs_band_policy_t *p, const zlrs_link_observation_t *o,
					   const zlrs_power_t *power)
{
	const zlrs_link_status_t *link = o->link;
	const zlrs_link_cfg_t *cfg = &link->cfg;
	const bool subg = cfg->region != ZLRS_REGION_ISM2400;
	const uint32_t settle_ms = o->settle_ms;
	const bool power_at_max = link->cfg.tx_power_dbm >= power->ceiling_dbm;

	if (!cfg->automatic_band || !link->paired || (link->state == ZLRS_STATE_PAIRING)) {
		zlrs_band_policy_reset(p, p->probing, o->now_ms, link->last_rx_ms);
		return ZLRS_BAND_KEEP;
	}
	if (link->last_rx_ms != p->last_rx_ms) {
		p->last_rx_ms = link->last_rx_ms;
		p->contact_ms = o->now_ms;
	}
	if (link->radio_acquiring) {
		p->stable.active = p->bad.active = p->good.active = false;
		p->power_boosted = false;
		if ((uint32_t)(o->now_ms - p->entered_ms) >=
		    (p->probing ? p->loss_ms : p->dwell_ms)) {
			return next_profile(p, cfg, ZLRS_BAND_SEARCH);
		}
		return ZLRS_BAND_KEEP;
	}
	if ((uint32_t)(o->now_ms - p->contact_ms) >= p->loss_ms) {
		return next_profile(p, cfg, ZLRS_BAND_SEARCH);
	}
	if (!(link->session_valid && link->state == ZLRS_STATE_CONNECTED) ||
	    (link->band_plan.switch_frame != 0U)) {
		p->bad.active = p->good.active = false;
		return ZLRS_BAND_KEEP;
	}
	const bool fresh = link->last_peer_feedback_ms != 0U &&
			   (uint32_t)(o->now_ms - link->last_peer_feedback_ms) < p->loss_ms &&
			   link->stats.last_rssi != 0 && link->peer_rssi_dbm != 0;
	const int16_t floor = zlrs_band_rssi_floor(subg, cfg->phy);
	const bool weak = fresh && (link->stats.last_rssi < floor || link->peer_rssi_dbm < floor);
	/* Only a complete post-maximum window can prove that more power did not help.
	 * Discard the first boundary, whose window may contain lower-power attempts. */
	if (!fresh || !power_at_max) {
		p->maximum = false;
		p->quality[0].windows = p->quality[1].windows = 0U;
	} else {
		const bool reset = !p->maximum || p->power_changed_ms != power->last_change_ms;
		if (reset) {
			p->maximum = true;
			p->maximum_ms = o->now_ms;
			p->power_changed_ms = power->last_change_ms;
		}
		const uint32_t generations[] = {link->stats.lq_generation,
						link->peer_lq_generation};
		const bool settled = (uint32_t)(o->now_ms - p->maximum_ms) >= settle_ms;
		for (size_t i = 0U; i < ARRAY_SIZE(p->quality); i++) {
			const uint32_t delta = generations[i] - p->quality[i].generation;
			if (reset || delta > 1U) {
				p->quality[i].windows = 0U;
			} else if (delta == 1U && settled && p->quality[i].windows < 2U) {
				p->quality[i].windows++;
			}
			p->quality[i].generation = generations[i];
		}
	}
	const bool quality_poor = fresh && p->maximum &&
				  ((p->quality[0].windows >= 2U && link->stats.lq_percent < 95U) ||
				   (p->quality[1].windows >= 2U && link->peer_lq_percent < 95U));
	const bool poor = weak || quality_poor;
	const int16_t upgrade_floor =
		subg || cfg->phy == 0U ? floor : zlrs_band_rssi_floor(false, cfg->phy - 1U) + 6;
	const bool quality_healthy = link->stats.lq_percent >= 95U &&
				     link->stats.lq_percent <= 100U &&
				     link->peer_lq_percent >= 95U && link->peer_lq_percent <= 100U;
	const bool upgrade = fresh && !poor && quality_healthy &&
			     link->stats.last_rssi >= upgrade_floor &&
			     link->peer_rssi_dbm >= upgrade_floor;
	/* Both roles react to either weak direction before the Leader may downgrade.
	 * Reverse DATA already reports the weak reception to the other endpoint. */
	if (!subg && weak) {
		p->stable.active = p->good.active = false;
		if (!p->power_boosted || !power_at_max) {
			p->power_boosted = true;
			p->power_boost_ms = o->now_ms;
			p->bad.active = false;
			return ZLRS_BAND_BOOST;
		}
		/* Repeated pre-boost observations cannot qualify a downgrade. */
		if ((int32_t)(link->last_peer_feedback_ms - p->power_boost_ms) <=
			    (int32_t)settle_ms ||
		    (int32_t)(link->last_rx_ms - p->power_boost_ms) <= (int32_t)settle_ms) {
			p->bad.active = false;
			return ZLRS_BAND_KEEP;
		}
	} else if (fresh) {
		p->power_boosted = false;
	}
	const bool stable = zlrs_hold_reached(&p->stable, upgrade, o->now_ms, 60000U);
	const bool good =
		zlrs_hold_reached(&p->good, fresh && !poor && quality_healthy, o->now_ms, 5000U);
	const bool bad = zlrs_hold_reached(&p->bad, poor, o->now_ms, 2000U);
	if (p->probing && good) {
		p->probing = false;
	}
	if (cfg->role != ZLRS_ROLE_LEADER) {
		return ZLRS_BAND_KEEP;
	}
	if (!subg && (quality_poor || bad)) {
		return next_profile(p, cfg, ZLRS_BAND_PLAN);
	}
	if (stable) {
		if (cfg->region != ZLRS_REGION_ISM2400) {
			return next_profile(p, cfg, ZLRS_BAND_PLAN);
		}
		if (cfg->phy > 0U) {
			p->target_subg = false;
			p->target_phy = cfg->phy - 1U;
			return ZLRS_BAND_PLAN;
		}
	}
	return ZLRS_BAND_KEEP;
}

/* Initial 10 dB receive margins; calibrate against equal-length packet error curves. */
int16_t zlrs_band_rssi_floor(bool subg, zlrs_phy_t phy)
{
	return subg ? -95 : zlrs_phys[phy < ZLRS_PHY_AUTO_COUNT ? phy : 0U].rssi_floor_dbm;
}

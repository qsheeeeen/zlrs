/* Automatic frequency-band policy; clocks and radio operations belong to RF.
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "proto/mod.h"

typedef enum {
	ZLRS_BAND_KEEP,
	ZLRS_BAND_PLAN,
	ZLRS_BAND_SEARCH,
	ZLRS_BAND_BOOST
} zlrs_band_action_t;

typedef struct {
	bool automatic, leader, subg, probing;
	zlrs_phy_t phy, target_phy;
	bool target_subg;
	uint32_t loss_ms, dwell_ms, entered_ms, contact_ms, last_rx_ms;
	uint32_t stable_ms, bad_ms, good_ms;
	bool stable, bad, good;
	bool power_boosted;
	uint32_t power_boost_ms;
	bool maximum;
	uint32_t maximum_ms, power_changed_ms, local_generation, peer_generation;
	uint8_t local_windows, peer_windows;
} zlrs_band_policy_t;

typedef struct {
	uint32_t now_ms, last_rx_ms, feedback_ms;
	uint32_t lq_generation, peer_lq_generation;
	uint32_t power_changed_ms, settle_ms;
	bool paired, pairing, connected, acquiring, planned;
	bool power_at_max;
	int16_t rssi_dbm, peer_rssi_dbm;
	uint8_t lq_percent, peer_lq_percent;
} zlrs_band_observation_t;

void zlrs_band_policy_init(zlrs_band_policy_t *policy, bool automatic, bool leader, bool subg,
			   uint32_t duty_window_ms, uint32_t now_ms);
void zlrs_band_policy_changed(zlrs_band_policy_t *policy, bool subg, zlrs_phy_t phy, bool probe,
			      uint32_t now_ms, uint32_t last_rx_ms);
void zlrs_band_policy_resume(zlrs_band_policy_t *policy, uint32_t now_ms, uint32_t last_rx_ms);
zlrs_band_action_t zlrs_band_policy_update(zlrs_band_policy_t *policy,
					   const zlrs_band_observation_t *observation);

/* Initial per-PHY receive boundary, shared with transmit-power control. */
int16_t zlrs_band_rssi_floor(bool subg, zlrs_phy_t phy);

/* Automatic frequency-band policy; clocks and radio operations belong to RF.
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "power.h"
#include "proto/limits.h"
#include "proto/mod.h"

typedef enum {
	ZLRS_BAND_KEEP,
	ZLRS_BAND_PLAN,
	ZLRS_BAND_SEARCH,
	ZLRS_BAND_BOOST
} zlrs_band_action_t;

typedef struct {
	bool probing;
	zlrs_phy_t target_phy;
	bool target_subg;
	uint32_t loss_ms, dwell_ms, entered_ms, contact_ms, last_rx_ms;
	zlrs_hold_t stable, bad, good;
	bool power_boosted;
	uint32_t power_boost_ms;
	bool maximum;
	uint32_t maximum_ms, power_changed_ms;

	struct {
		uint32_t generation;
		uint8_t windows;
	} quality[2];
} zlrs_band_policy_t;

void zlrs_band_policy_init(zlrs_band_policy_t *policy, zlrs_role_t role, uint32_t duty_window_ms,
			   uint32_t now_ms);
void zlrs_band_policy_reset(zlrs_band_policy_t *policy, bool probe, uint32_t now_ms,
			    uint32_t last_rx_ms);
zlrs_band_action_t zlrs_band_policy_update(zlrs_band_policy_t *policy,
					   const zlrs_link_observation_t *observation,
					   const zlrs_power_t *power);

/* Initial per-PHY receive boundary, shared with transmit-power control. */
int16_t zlrs_band_rssi_floor(bool subg, zlrs_phy_t phy);

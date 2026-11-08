/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "link/link.h"
#include <stdbool.h>
#include <stdint.h>

#define ZLRS_PAIR_POWER_DBM (-10)

/* One policy per DATA direction; only the caller applies the returned setpoint. */
typedef struct {
	int8_t floor_dbm, ceiling_dbm, current_dbm;
	bool filtering, lq_hold;
	zlrs_hold_t good, bad;
	int32_t rssi_q8;
	uint32_t last_change_ms, last_feedback_ms, lq_generation, lq_hold_ms;
	uint32_t lq_hold_duration_ms;
	uint32_t raises, lowers, fallbacks;
} zlrs_power_t;

void zlrs_power_init(zlrs_power_t *power, int8_t ceiling_dbm, int8_t floor_dbm, uint32_t now_ms);
/* Recovery bypasses holds and clears observation/qualification history. */
int8_t zlrs_power_recover(zlrs_power_t *power, uint32_t now_ms);
int8_t zlrs_power_update(zlrs_power_t *power, const zlrs_link_observation_t *observation);

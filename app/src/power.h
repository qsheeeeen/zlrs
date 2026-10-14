/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ZLRS_PAIR_POWER_DBM (-10)

/* One policy state per DATA link. The caller owns it and applies the returned setpoint. */
typedef struct {
	int8_t floor_dbm;
	int8_t ceiling_dbm;
	int8_t current_dbm;
	uint8_t good;
	uint8_t bad;
	bool healthy;
	uint32_t healthy_since_ms;
	uint32_t last_change_ms;
	uint32_t last_feedback_ms;
	uint32_t raises;
	uint32_t lowers;
	uint32_t fallbacks;
} zlrs_power_t;

void zlrs_power_init(zlrs_power_t *power, int8_t ceiling_dbm, int8_t floor_dbm, uint32_t now_ms);

/* Recovery clears all qualification history and bypasses adjustment holds. */
int8_t zlrs_power_recover(zlrs_power_t *power, uint32_t now_ms);

/* feedback_ms is the peer-feedback reception time; zero means no feedback received yet.
 * Repeated observations count once. Millisecond arithmetic supports clock wraparound.
 */
int8_t zlrs_power_update(zlrs_power_t *power, int16_t rssi_dbm, uint8_t lq_percent,
			 uint32_t feedback_ms, uint32_t now_ms);

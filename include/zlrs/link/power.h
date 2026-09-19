/*
 * Dynamic TX power: follow the uplink RSSI/LQ the peer reports; losing telemetry goes straight
 * back to the ceiling (the peer is probably gone, and reach beats saving power). Same skeleton
 * as rate adaptation (windows, hold time, hysteresis); thresholds await calibration (09-protocol 4.5).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	/* policy parameters */
	int8_t min_dbm;      /* floor when the signal is good */
	int8_t max_dbm;      /* ceiling: the smaller of configured power and region cap */
	int8_t step_db;      /* step size per adjustment */
	int8_t rssi_low_dbm; /* below this, raise power */
	int8_t rssi_high_dbm;/* above this (and with good LQ), lower power */
	uint8_t lq_low;      /* below this LQ (%), raise power */
	uint8_t lq_high;     /* above this LQ (%), lower power */
	uint8_t windows;     /* consecutive samples needed before an adjustment */
	uint32_t hold_ms;    /* minimum gap between adjustments */
	uint32_t tlm_timeout_ms; /* telemetry silence before returning to the ceiling */

	/* state */
	int8_t current_dbm;
	uint8_t good;
	uint8_t bad;
	uint32_t last_change_ms;
	uint32_t last_tlm_ms;
	bool tlm_seen;
	uint32_t raises;   /* power increases */
	uint32_t lowers;   /* power decreases */
	uint32_t fallbacks;/* returns to the ceiling after a telemetry timeout */
} zlrs_power_ctl_t;

/*
 * configured_dbm: the ceiling config and region allow (dynamic power never exceeds it)
 * floor_dbm: the lowest power (a Kconfig knob)
 */
void zlrs_power_init(zlrs_power_ctl_t *ctl, int8_t configured_dbm, int8_t floor_dbm,
		     uint32_t now_ms);

/*
 * Call on every telemetry frame (valid = true, rssi is our signal as measured by the peer) or
 * on a periodic poll (valid = false, used for the telemetry timeout).
 * Returns true when a change is due and writes the new power into *out_dbm.
 */
bool zlrs_power_update(zlrs_power_ctl_t *ctl, bool valid, int16_t rssi_dbm,
		       uint8_t lq_percent, uint32_t now_ms, int8_t *out_dbm);

int8_t zlrs_power_current_dbm(const zlrs_power_ctl_t *ctl);

#ifdef __cplusplus
}
#endif


/*
 * Dynamic TX power: follow the quality the peer reports, so the link uses only what it needs.
 *
 * The policy is core/link's (`zlrs_power_update`, host-tested); this is its application — the
 * ceiling the region and the build set, the uplink quality from telemetry, and the once-a-second
 * verdict (doc/09-protocol.md 4.5). Losing telemetry means the other end is probably gone, which
 * the policy answers by going back to the ceiling.
 *
 * Naming: `<dir>/<file>.c` carries `zlrs_<dir>_<file>_*()`, so this is `zlrs_link_power_*()`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/link/link.h"

/* ceiling_dbm is min(configured power, region cap), floor_dbm the build's floor */
void zlrs_link_power_init(int8_t ceiling_dbm, int8_t floor_dbm);

/* Remember the uplink quality the peer reported; the verdict runs at 1 Hz (telemetry callback) */
void zlrs_link_power_telemetry(uint8_t rssi_neg_dbm, uint8_t lq_percent);

/* Once-a-second verdict: move the control link's TX power when the policy says so */
void zlrs_link_power_tick(zlrs_link_t *control, uint32_t now_ms);

/* Where the control loop is now, for the bench line in doc/12-debug.md 3 */
void zlrs_link_power_stats(int8_t *current_dbm, uint32_t *raises, uint32_t *lowers,
			   uint32_t *fallbacks);

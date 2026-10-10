/*
 * Dynamic TX power: follow the quality the peer reports, so the link uses only what it needs.
 *
 * The application module owns the policy, the ceiling the region and the build set, the peer
 * quality from DATA feedback, and the once-a-second verdict (doc/08-protocol.md). Losing feedback
 * means the other end is probably gone, which the policy answers by going back to the ceiling.
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

/* Remember the quality the peer reported; the verdict runs at 1 Hz */
void zlrs_link_power_feedback(int16_t rssi_dbm, uint8_t lq_percent);

/* Once-a-second verdict: move the DATA link's TX power when the policy says so */
void zlrs_link_power_tick(zlrs_link_t *data_link, uint32_t now_ms);

/* Where the DATA power loop is now, for the bench line in doc/11-debug.md */
void zlrs_link_power_stats(int8_t *current_dbm, uint32_t *raises, uint32_t *lowers,
			   uint32_t *fallbacks);

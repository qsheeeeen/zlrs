/*
 * Rate adaptation framework: how tiers move up and down (windows, hold time, up-margin).
 * The tier table and thresholds come from measured curves, see doc/13-roadmap.md.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/mod.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	/*
	 * A tier is a full set of modulation parameters: an FLRC rate or LoRa SF/bandwidth/coding rate.
	 * See doc/07-architecture.md 3: this is where the tier table stops being "one rate" and becomes
	 * "a modulation plus its parameters".
	 */
	zlrs_mod_cfg_t mod;
	/* Below this LQ the tier cannot be held and the link should drop one (percent) */
	uint8_t min_lq_to_stay;
} zlrs_rate_tier_t;

/*
 * The tier table runs fastest to slowest: +1 is slower (a drop), -1 is faster (a raise).
 */

typedef struct {
	const zlrs_rate_tier_t *tiers;
	uint8_t count;
	uint8_t current;      /* index of the current tier */
	uint8_t hysteresis;   /* margin above the target tier's min_lq_to_stay when moving up */
	uint8_t windows;      /* consecutive windows that have to agree before a change */
	uint32_t hold_ms;     /* minimum time a new tier stays before the next change */
	uint32_t last_ms;     /* when the tier last changed (or was initialised) */
	uint8_t good;         /* consecutive good windows */
	uint8_t bad;          /* consecutive bad windows */
} zlrs_rate_ctl_t;

void zlrs_rate_init(zlrs_rate_ctl_t *ctl, const zlrs_rate_tier_t *tiers, uint8_t count,
		    uint8_t start, uint32_t now_ms);

/*
 * Called once per LQ window. Returns true when the tier should change and writes the new index.
 * Internally filtered by windows, hold_ms and hysteresis.
 */
bool zlrs_rate_update(zlrs_rate_ctl_t *ctl, uint8_t lq_percent, uint32_t now_ms,
		      uint8_t *out);

uint32_t zlrs_rate_current_bps(const zlrs_rate_ctl_t *ctl);

/* Full modulation description of the current tier (FLRC and LoRa alike) */
const zlrs_mod_cfg_t *zlrs_rate_current_mod(const zlrs_rate_ctl_t *ctl);

#ifdef __cplusplus
}
#endif


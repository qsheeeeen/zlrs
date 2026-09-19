/*
 * Rate adaptation policy, see include/zlrs/link/rate.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/link/rate.h"

#include <stddef.h>

void zlrs_rate_init(zlrs_rate_ctl_t *ctl, const zlrs_rate_tier_t *tiers, uint8_t count,
		    uint8_t start, uint32_t now_ms)
{
	ctl->tiers = tiers;
	ctl->count = count;
	ctl->current = (start < count) ? start : 0U;
	ctl->hysteresis = 10U; /* 10 percentage points by default, tunable when calibrating */
	ctl->windows = 3U;
	ctl->hold_ms = 2000U;
	ctl->last_ms = now_ms;
	ctl->good = 0U;
	ctl->bad = 0U;
}

uint32_t zlrs_rate_current_bps(const zlrs_rate_ctl_t *ctl)
{
	if (ctl->tiers == NULL || ctl->count == 0U) {
		return 0U;
	}

	return ctl->tiers[ctl->current].mod.rate_bps;
}

const zlrs_mod_cfg_t *zlrs_rate_current_mod(const zlrs_rate_ctl_t *ctl)
{
	if (ctl->tiers == NULL || ctl->count == 0U) {
		return NULL;
	}

	return &ctl->tiers[ctl->current].mod;
}

bool zlrs_rate_update(zlrs_rate_ctl_t *ctl, uint8_t lq_percent, uint32_t now_ms, uint8_t *out)
{
	const zlrs_rate_tier_t *tier;
	uint8_t target = ctl->current;

	if (ctl->tiers == NULL || ctl->count == 0U) {
		return false;
	}

	tier = &ctl->tiers[ctl->current];

	/* No windows during the hold time: counting restarts when a tier change is allowed */
	if ((uint32_t)(now_ms - ctl->last_ms) < ctl->hold_ms) {
		ctl->good = 0U;
		ctl->bad = 0U;
		return false;
	}

	/* Cannot hold this tier: several bad windows in a row drop to the next slower one (index +1) */
	if (lq_percent < tier->min_lq_to_stay) {
		ctl->bad++;
		ctl->good = 0U;
	} else if (ctl->current > 0U &&
		   lq_percent >= (uint8_t)(ctl->tiers[ctl->current - 1U].min_lq_to_stay +
					   ctl->hysteresis)) {
		/* Comfortable enough for a faster tier: several good windows in a row move up */
		ctl->good++;
		ctl->bad = 0U;
	} else {
		ctl->good = 0U;
		ctl->bad = 0U;
		return false;
	}

	if (ctl->bad >= ctl->windows && ctl->current + 1U < ctl->count) {
		target = (uint8_t)(ctl->current + 1U);
	} else if (ctl->good >= ctl->windows && ctl->current > 0U) {
		target = (uint8_t)(ctl->current - 1U);
	} else {
		return false;
	}

	ctl->current = target;
	ctl->last_ms = now_ms;
	ctl->good = 0U;
	ctl->bad = 0U;

	if (out != NULL) {
		*out = target;
	}

	return true;
}

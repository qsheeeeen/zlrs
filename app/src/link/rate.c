/*
 * Rate adaptation policy and application wiring.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "link/rate.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(zlrs_link_rate, CONFIG_APP_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZLRS_APP_RATE_ADAPT)
struct rate_tier {
	zlrs_mod_cfg_t mod;
	uint8_t min_lq_to_stay;
};

struct rate_ctl {
	const struct rate_tier *tiers;
	uint8_t count;
	uint8_t current;
	uint8_t hysteresis;
	uint8_t windows;
	uint32_t hold_ms;
	uint32_t last_ms;
	uint8_t good;
	uint8_t bad;
};

static void rate_init(struct rate_ctl *ctl, const struct rate_tier *tiers, uint8_t count,
		      uint8_t start, uint32_t now_ms)
{
	ctl->tiers = tiers;
	ctl->count = count;
	ctl->current = (start < count) ? start : 0U;
	ctl->hysteresis = 10U; /* percentage points above the faster tier's threshold */
	ctl->windows = 3U;
	ctl->hold_ms = 2000U;
	ctl->last_ms = now_ms;
	ctl->good = 0U;
	ctl->bad = 0U;
}

static bool rate_update(struct rate_ctl *ctl, uint8_t lq_percent, uint32_t now_ms, uint8_t *out)
{
	const struct rate_tier *tier;
	uint8_t target = ctl->current;

	if (ctl->tiers == NULL || ctl->count == 0U) {
		return false;
	}

	tier = &ctl->tiers[ctl->current];
	/* Do not count windows until the current tier has had time to settle. */
	if ((uint32_t)(now_ms - ctl->last_ms) < ctl->hold_ms) {
		ctl->good = 0U;
		ctl->bad = 0U;
		return false;
	}

	if (lq_percent < tier->min_lq_to_stay) {
		ctl->bad++;
		ctl->good = 0U;
	} else if (ctl->current > 0U &&
		   lq_percent >= (uint8_t)(ctl->tiers[ctl->current - 1U].min_lq_to_stay +
					   ctl->hysteresis)) {
		/* Faster tiers require a margin above their minimum LQ to avoid oscillation. */
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
/*
 * Tiers this build may use, fastest first. The thresholds are placeholders: they wait for measured
 * packet-error curves (doc/12-roadmap.md), the mechanism around them is what is being proven.
 */
static const struct rate_tier rate_tiers[] = {
	{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 2600000U}, .min_lq_to_stay = 85U},
	{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 1300000U}, .min_lq_to_stay = 60U},
	{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 650000U}, .min_lq_to_stay = 35U},
	{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 325000U}, .min_lq_to_stay = 0U},
};

static struct rate_ctl rate_ctl;
#endif

void zlrs_link_rate_init(const zlrs_mod_cfg_t *mod)
{
#if IS_ENABLED(CONFIG_ZLRS_APP_RATE_ADAPT)
	uint8_t start = 0U;

	for (uint8_t i = 0U; i < ARRAY_SIZE(rate_tiers); i++) {
		if (rate_tiers[i].mod.rate_bps == mod->rate_bps) {
			start = i;
		}
	}

	rate_init(&rate_ctl, rate_tiers, (uint8_t)ARRAY_SIZE(rate_tiers), start, 0U);
#else
	ARG_UNUSED(mod);
#endif
}

void zlrs_link_rate_tick(zlrs_link_t *control, uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_ZLRS_APP_RATE_ADAPT)
	uint8_t next = 0U;

	if (rate_update(&rate_ctl, control->stats.lq_percent, now_ms, &next)) {
		LOG_INF("rate tier -> %u bps (LQ %u%%)",
			(unsigned int)rate_tiers[next].mod.rate_bps, control->stats.lq_percent);
		zlrs_link_set_mod(control, &rate_tiers[next].mod);
	}
#else
	ARG_UNUSED(control);
	ARG_UNUSED(now_ms);
#endif
}

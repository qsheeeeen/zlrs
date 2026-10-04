/*
 * Rate adaptation, see link/rate.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "link/rate.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "zlrs/link/rate.h"

LOG_MODULE_REGISTER(zlrs_link_rate, CONFIG_APP_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZLRS_APP_RATE_ADAPT)
/*
 * Tiers this build may use, fastest first. The thresholds are placeholders: they wait for measured
 * packet-error curves (doc/12-roadmap.md), the mechanism around them is what is being proven.
 */
static const zlrs_rate_tier_t rate_tiers[] = {
	{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 2600000U}, .min_lq_to_stay = 85U},
	{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 1300000U}, .min_lq_to_stay = 60U},
	{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 650000U}, .min_lq_to_stay = 35U},
	{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 325000U}, .min_lq_to_stay = 0U},
};

static zlrs_rate_ctl_t rate_ctl;
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

	zlrs_rate_init(&rate_ctl, rate_tiers, (uint8_t)ARRAY_SIZE(rate_tiers), start, 0U);
#else
	ARG_UNUSED(mod);
#endif
}

void zlrs_link_rate_tick(zlrs_link_t *control, uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_ZLRS_APP_RATE_ADAPT)
	uint8_t next = 0U;

	if (zlrs_rate_update(&rate_ctl, control->stats.lq_percent, now_ms, &next)) {
		LOG_INF("rate tier -> %u bps (LQ %u%%)", (unsigned int)rate_tiers[next].mod.rate_bps,
			control->stats.lq_percent);
		zlrs_link_set_mod(control, &rate_tiers[next].mod);
	}
#else
	ARG_UNUSED(control);
	ARG_UNUSED(now_ms);
#endif
}

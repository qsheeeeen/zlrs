/*
 * Regions and duty accounting, see app/src/region/region.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "region/region.h"

#include <stddef.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#if DT_NODE_EXISTS(DT_NODELABEL(zlrs_rf))
#define REGION_BIT(node, prop, index) | BIT(DT_PROP_BY_IDX(node, prop, index) & 31U)
#define REGIONS_MASK                  (0U DT_FOREACH_PROP_ELEM(DT_NODELABEL(zlrs_rf), regions, REGION_BIT))
#define CHECK_REGION(node, prop, index)                                                            \
	BUILD_ASSERT((unsigned int)DT_PROP_BY_IDX(node, prop, index) < ZLRS_REGION_COUNT,          \
		     "unsupported predefined region ID");
DT_FOREACH_PROP_ELEM(DT_NODELABEL(zlrs_rf), regions, CHECK_REGION)
BUILD_ASSERT(DT_PROP_LEN(DT_NODELABEL(zlrs_rf), regions) > 0, "empty supported region set");
BUILD_ASSERT(__builtin_popcount(REGIONS_MASK) == DT_PROP_LEN(DT_NODELABEL(zlrs_rf), regions),
	     "duplicate supported region ID");
BUILD_ASSERT(DT_PROP(DT_NODELABEL(zlrs_rf), default_region) < ZLRS_REGION_COUNT,
	     "unknown default region");
BUILD_ASSERT((REGIONS_MASK & BIT(DT_PROP(DT_NODELABEL(zlrs_rf), default_region) & 31U)) != 0U,
	     "default region must belong to regions");
#else
/* Headless component fixtures have no radio and permit no region selection. */
#define REGIONS_MASK 0U
#endif

/*
 * Power and rate ceilings are conservative values for the usual sub-bands (check local rules):
 * FCC 15.247 uses the module ceiling of 22 dBm, ETSI EN 300 220 uses 14 dBm at 1%, CN470 17 dBm,
 * CN433 (low power) 10 dBm.
 */
const zlrs_region_t zlrs_regions[ZLRS_REGION_COUNT] = {
	[ZLRS_REGION_FCC915] = {.name = "FCC915",
				.start_hz = 902500000U,
				.step_hz = 500000U,
				.channel_count = 50U,
				.low_hz = 902000000U,
				.high_hz = 928000000U,
				.max_tx_power_dbm = 22,
				.duty_percent = 0U,
				.duty_window_ms = 0U,
				.max_rate_bps = 2600000U},
	[ZLRS_REGION_EU868] = {.name = "EU868",
			       .start_hz = 863250000U,
			       .step_hz = 500000U,
			       .channel_count = 13U,
			       .low_hz = 863000000U,
			       .high_hz = 870000000U,
			       .max_tx_power_dbm = 14,
			       .duty_percent = 1U,
			       .duty_window_ms = 60000U,
			       .max_rate_bps = 325000U},
	[ZLRS_REGION_CN470] = {.name = "CN470",
			       .start_hz = 470300000U,
			       .step_hz = 300000U,
			       .channel_count = 96U,
			       .low_hz = 470000000U,
			       .high_hz = 510000000U,
			       .max_tx_power_dbm = 17,
			       .duty_percent = 0U,
			       .duty_window_ms = 0U,
			       .max_rate_bps = 325000U},
	[ZLRS_REGION_CN433] = {.name = "CN433",
			       .start_hz = 433300000U,
			       .step_hz = 300000U,
			       .channel_count = 5U,
			       .low_hz = 433050000U,
			       .high_hz = 434790000U,
			       .max_tx_power_dbm = 10,
			       .duty_percent = 0U,
			       .duty_window_ms = 0U,
			       .max_rate_bps = 325000U},
	[ZLRS_REGION_ISM2400] = {.name = "ISM2400",
				 .start_hz = 2402000000U,
				 .step_hz = 1000000U,
				 .channel_count = 79U,
				 .low_hz = 2400000000U,
				 .high_hz = 2483500000U,
				 .max_tx_power_dbm = 22,
				 .max_rate_bps = 2600000U},
};

uint32_t zlrs_region_clamp_rate(const zlrs_region_t *region, uint32_t rate_bps)
{
	if (region == NULL || region->max_rate_bps == 0U) {
		return rate_bps;
	}

	return (rate_bps > region->max_rate_bps) ? region->max_rate_bps : rate_bps;
}

uint32_t zlrs_region_supported_mask(void)
{
	return REGIONS_MASK;
}

const zlrs_region_t *zlrs_region_get(zlrs_region_id_t id)
{
	if ((unsigned int)id >= ZLRS_REGION_COUNT || (REGIONS_MASK & BIT(id)) == 0U) {
		return NULL;
	}

	return &zlrs_regions[id];
}

void zlrs_duty_init(zlrs_duty_t *duty, const zlrs_region_t *region, uint32_t now_ms)
{
	duty->window_ms =
		region != NULL && region->duty_percent != 0U ? region->duty_window_ms : 0U;
	duty->budget_ms = region != NULL ? (duty->window_ms / 100U) * region->duty_percent : 0U;

	duty->window_start_ms = now_ms;
	duty->used_ms = 0U;
	duty->denied = 0U;
}

bool zlrs_duty_reserve(zlrs_duty_t *duty, uint32_t airtime_ms, uint32_t now_ms)
{
	if (duty->window_ms == 0U) {
		return true;
	}

	if ((uint32_t)(now_ms - duty->window_start_ms) >= duty->window_ms) {
		duty->window_start_ms = now_ms;
		duty->used_ms = 0U;
	}

	if (airtime_ms > duty->budget_ms - duty->used_ms) {
		duty->denied++;
		return false;
	}

	duty->used_ms += airtime_ms;

	return true;
}

/*
 * Regions and duty accounting, see app/src/region/region.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "region/region.h"

#include <stddef.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#define RADIO_NODE DT_NODELABEL(zlrs_rf)
#if DT_NODE_EXISTS(RADIO_NODE)
#define REGION_BIT(node, prop, index) | BIT(DT_PROP_BY_IDX(node, prop, index) & 31U)
#define REGIONS_MASK                  (0U DT_FOREACH_PROP_ELEM(RADIO_NODE, regions, REGION_BIT))
#define CHECK_REGION(node, prop, index)                                                            \
	BUILD_ASSERT((unsigned int)DT_PROP_BY_IDX(node, prop, index) < ZLRS_REGION_COUNT,          \
		     "unsupported predefined region ID");
DT_FOREACH_PROP_ELEM(RADIO_NODE, regions, CHECK_REGION)
BUILD_ASSERT(DT_PROP_LEN(RADIO_NODE, regions) > 0, "empty supported region set");
BUILD_ASSERT(__builtin_popcount(REGIONS_MASK) == DT_PROP_LEN(RADIO_NODE, regions),
	     "duplicate supported region ID");
BUILD_ASSERT(DT_PROP(RADIO_NODE, default_region) < ZLRS_REGION_COUNT, "unknown default region");
BUILD_ASSERT((REGIONS_MASK & BIT(DT_PROP(RADIO_NODE, default_region) & 31U)) != 0U,
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
static const zlrs_region_t regions[ZLRS_REGION_COUNT] = {
	[ZLRS_REGION_FCC915] = {.name = "FCC915",
				.domain = ZLRS_DOMAIN_FCC915,
				.low_hz = 902000000U,
				.high_hz = 928000000U,
				.max_tx_power_dbm = 22,
				.duty_percent = 0U,
				.duty_window_ms = 0U,
				.max_rate_bps = 2600000U},
	[ZLRS_REGION_EU868] = {.name = "EU868",
			       .domain = ZLRS_DOMAIN_EU868,
			       .low_hz = 863000000U,
			       .high_hz = 870000000U,
			       .max_tx_power_dbm = 14,
			       .duty_percent = 1U,
			       .duty_window_ms = 60000U,
			       .max_rate_bps = 325000U},
	[ZLRS_REGION_CN470] = {.name = "CN470",
			       .domain = ZLRS_DOMAIN_CN470,
			       .low_hz = 470000000U,
			       .high_hz = 510000000U,
			       .max_tx_power_dbm = 17,
			       .duty_percent = 0U,
			       .duty_window_ms = 0U,
			       .max_rate_bps = 325000U},
	[ZLRS_REGION_CN433] = {.name = "CN433",
			       .domain = ZLRS_DOMAIN_CN433,
			       .low_hz = 433050000U,
			       .high_hz = 434790000U,
			       .max_tx_power_dbm = 10,
			       .duty_percent = 0U,
			       .duty_window_ms = 0U,
			       .max_rate_bps = 325000U},
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

	return &regions[id];
}

uint32_t zlrs_airtime_us(uint16_t frame_len, uint32_t rate_bps)
{
	uint64_t bits;

	if (rate_bps == 0U) {
		return 0U;
	}

	/* Net payload plus overhead; preamble and sync word are approximated at 32 bits */
	bits = ((uint64_t)frame_len + 4U) * 8U;

	return (uint32_t)((bits * 1000000U + rate_bps - 1U) / rate_bps);
}

uint32_t zlrs_airtime_ms(uint16_t frame_len, uint32_t rate_bps)
{
	const uint32_t us = zlrs_airtime_us(frame_len, rate_bps);

	return (us + 999U) / 1000U;
}

/* Frames per second: the budget comes from the duty cap, each frame costs its airtime */
static uint32_t max_frames_for_airtime(const zlrs_region_t *region, uint32_t airtime_us)
{
	uint64_t budget_us;

	if (region == NULL || region->duty_percent == 0U || airtime_us == 0U) {
		return UINT32_MAX;
	}

	/* 1% = 10000 us of transmit budget per second */
	budget_us = (uint64_t)region->duty_percent * 10000U;

	return (uint32_t)(budget_us / airtime_us);
}

uint32_t zlrs_region_max_frames_per_s(const zlrs_region_t *region, uint32_t rate_bps,
				      uint16_t frame_len)
{
	return max_frames_for_airtime(region, zlrs_airtime_us(frame_len, rate_bps));
}

void zlrs_duty_init(zlrs_duty_t *duty, const zlrs_region_t *region, uint32_t now_ms)
{
	duty->window_ms = (region != NULL) ? region->duty_window_ms : 0U;
	duty->limit_percent = (region != NULL) ? region->duty_percent : 0U;
	duty->limited =
		(region != NULL) && (region->duty_percent > 0U) && (region->duty_window_ms > 0U);
	duty->window_start_ms = now_ms;
	duty->used_ms = 0U;
	duty->denied = 0U;
}

static void duty_roll(zlrs_duty_t *duty, uint32_t now_ms)
{
	if (!duty->limited) {
		return;
	}

	if ((uint32_t)(now_ms - duty->window_start_ms) >= duty->window_ms) {
		duty->window_start_ms = now_ms;
		duty->used_ms = 0U;
	}
}

uint32_t zlrs_duty_remaining_ms(zlrs_duty_t *duty, uint32_t now_ms)
{
	uint32_t budget;

	if (!duty->limited) {
		return UINT32_MAX;
	}

	duty_roll(duty, now_ms);

	budget = (duty->window_ms / 100U) * duty->limit_percent;
	if (duty->used_ms >= budget) {
		return 0U;
	}

	return budget - duty->used_ms;
}

bool zlrs_duty_reserve(zlrs_duty_t *duty, uint32_t airtime_ms, uint32_t now_ms)
{
	if (!duty->limited) {
		return true;
	}

	duty_roll(duty, now_ms);

	if (airtime_ms > zlrs_duty_remaining_ms(duty, now_ms)) {
		duty->denied++;
		return false;
	}

	duty->used_ms += airtime_ms;

	return true;
}

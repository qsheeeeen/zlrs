/*
 * Regions and duty accounting, see include/zlrs/region/region.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/region/region.h"

#include <stddef.h>

/*
 * Power and rate ceilings are conservative values for the usual sub-bands (check local rules):
 * FCC 15.247 uses the module ceiling of 22 dBm, ETSI EN 300 220 uses 14 dBm at 1%, CN470 17 dBm,
 * CN433 (low power) 10 dBm. The band is a hardware property: FCC915/EU868 need HF, the rest LF.
 */
static const zlrs_region_t regions[ZLRS_REGION_COUNT] = {
	[ZLRS_REGION_FCC915] = {.name = "FCC915",
				.domain = ZLRS_DOMAIN_FCC915,
				.band = ZLRS_BAND_SUBG_HF,
				.low_hz = 902000000U,
				.high_hz = 928000000U,
				.max_tx_power_dbm = 22,
				.duty_percent = 0U,
				.duty_window_ms = 0U,
				.max_rate_bps = 2600000U},
	[ZLRS_REGION_EU868] = {.name = "EU868",
			       .domain = ZLRS_DOMAIN_EU868,
			       .band = ZLRS_BAND_SUBG_HF,
			       .low_hz = 863000000U,
			       .high_hz = 870000000U,
			       .max_tx_power_dbm = 14,
			       .duty_percent = 1U,
			       .duty_window_ms = 60000U,
			       .max_rate_bps = 325000U},
	[ZLRS_REGION_CN470] = {.name = "CN470",
			       .domain = ZLRS_DOMAIN_CN470,
			       .band = ZLRS_BAND_SUBG_LF,
			       .low_hz = 470000000U,
			       .high_hz = 510000000U,
			       .max_tx_power_dbm = 17,
			       .duty_percent = 0U,
			       .duty_window_ms = 0U,
			       .max_rate_bps = 325000U},
	[ZLRS_REGION_CN433] = {.name = "CN433",
			       .domain = ZLRS_DOMAIN_CN433,
			       .band = ZLRS_BAND_SUBG_LF,
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

zlrs_mod_cfg_t zlrs_region_clamp_mod(const zlrs_region_t *region, zlrs_mod_cfg_t mod)
{
	/* The step in narrow-band regions follows the FLRC bandwidth (see core/proto/fhss.c), and a
	      LoRa bandwidth has to fit inside the band edges as well */
	static const uint32_t bws[] = {500000U, 250000U, 125000U};

	if (region == NULL) {
		return mod;
	}

	if (mod.modulation == (uint8_t)ZLRS_MOD_FLRC) {
		mod.rate_bps = zlrs_region_clamp_rate(region, mod.rate_bps);
		return mod;
	}

	if (mod.modulation != (uint8_t)ZLRS_MOD_LORA) {
		return mod;
	}

	/* Widest to narrowest: pick a bandwidth that fits both the request and the band edges */
	for (uint8_t i = 0U; i < sizeof(bws) / sizeof(bws[0]); i++) {
		if (bws[i] <= mod.bw_hz && bws[i] <= (region->high_hz - region->low_hz)) {
			mod.bw_hz = bws[i];
			return mod;
		}
	}

	/* Nothing fits: return it as-is and let the boot check complain */
	return mod;
}

const zlrs_region_t *zlrs_region_get(uint8_t id)
{
	if (id >= ZLRS_REGION_COUNT) {
		return NULL;
	}

	return &regions[id];
}

int zlrs_region_validate(const zlrs_region_t *region, const zlrs_rf_design_t *rf,
			 int8_t tx_power_dbm)
{
	const zlrs_fhss_domain_t *domain;
	uint32_t first;
	uint32_t last;

	if (region == NULL || rf == NULL || region->domain >= ZLRS_DOMAIN_COUNT) {
		return -1;
	}
	/* The radio design has to be the band the region needs: LF cannot do 915, HF cannot do 470/433 */
	if (region->band != rf->band) {
		return -5;
	}
	/* And it has to actually cover the region's band edges, not merely claim the right SKU */
	if (region->low_hz < rf->low_hz || region->high_hz > rf->high_hz) {
		return -6;
	}
	if (tx_power_dbm > region->max_tx_power_dbm) {
		return -2;
	}

	domain = &zlrs_fhss_domains[region->domain];
	if (domain->channel_count == 0U) {
		return -3;
	}

	first = domain->start_hz;
	last = domain->start_hz + (uint32_t)(domain->channel_count - 1U) * domain->step_hz;
	if (first < region->low_hz || last > region->high_hz) {
		return -4;
	}

	return 0;
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

uint32_t zlrs_airtime_mod_us(const zlrs_mod_cfg_t *mod, uint16_t frame_len)
{
	return zlrs_mod_airtime_us(mod, frame_len);
}

uint32_t zlrs_airtime_mod_ms(const zlrs_mod_cfg_t *mod, uint16_t frame_len)
{
	return zlrs_mod_airtime_ms(mod, frame_len);
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

uint32_t zlrs_region_max_frames_per_s_mod(const zlrs_region_t *region, const zlrs_mod_cfg_t *mod,
					  uint16_t frame_len)
{
	return max_frames_for_airtime(region, zlrs_mod_airtime_us(mod, frame_len));
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

uint8_t zlrs_region_mix_level(const zlrs_region_t *region, uint32_t window_ms,
			      uint32_t subg_slot_airtime_us)
{
	static const uint8_t slots[4] = {0U, 1U, 2U, 4U};
	uint64_t allowed_us;
	uint8_t level = 3U;

	if (region == NULL || region->duty_percent == 0U || window_ms == 0U ||
	    subg_slot_airtime_us == 0U) {
		return 3U; /* no duty limit: 100% */
	}

	/* Allowed airtime per window = window length x duty cycle */
	allowed_us = ((uint64_t)window_ms * 1000U * region->duty_percent) / 100U;

	/* Highest to lowest: first level that fits (the four levels are 0/1/2/4 slots) */
	for (int i = 3; i >= 0; i--) {
		if ((uint64_t)slots[i] * subg_slot_airtime_us <= allowed_us) {
			level = (uint8_t)i;
			break;
		}
		level = 0U;
	}

	return level;
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

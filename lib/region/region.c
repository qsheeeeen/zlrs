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
	/* The step in narrow-band regions follows the FLRC bandwidth (see lib/proto/fhss.c), and a
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

int zlrs_region_validate(const zlrs_region_t *region, uint8_t band, int8_t tx_power_dbm)
{
	const zlrs_fhss_domain_t *domain;
	uint32_t first;
	uint32_t last;

	if (region == NULL || region->domain >= ZLRS_DOMAIN_COUNT) {
		return -1;
	}
	/* Region and local sub-G hardware must match: an LF board cannot do 915, an HF one cannot do 470/433 */
	if (region->band != band) {
		return -5;
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

/* A slot needs headroom beyond airtime for RAC scheduling, mode flips and SPI */
#define SLOT_OVERHEAD_US   200U
/* Slot budget ceiling: waiting longer only waits for a transaction that never returns */
#define SLOT_BUDGET_MAX_US 2000U
/* Rate floor: below this the link is unflyable (used when duty clamps too hard, capped by airtime) */
#define MIN_CONTROL_HZ     10U
/* Target rates for the telemetry and data windows (times per second) */
#define TLM_TARGET_HZ      150U
#define DATA_TARGET_HZ     100U

void zlrs_region_control_plan(const zlrs_plan_req_t *req, zlrs_control_plan_t *out)
{
	const zlrs_region_t *region = (req != NULL) ? req->region : NULL;
	const uint16_t tlm_len = (req != NULL && req->tlm_len > 0U)
					 ? req->tlm_len
					 : ((req != NULL) ? req->frame_len : 0U);
	const uint32_t airtime_us = zlrs_mod_airtime_us((req != NULL) ? req->mod : NULL,
							(req != NULL) ? req->frame_len : 0U);
	const uint32_t tlm_airtime_us =
		zlrs_mod_airtime_us((req != NULL) ? req->mod : NULL, tlm_len);
	/* A frame fits at least two control slots (RC out, telemetry in) plus the other link's share */
	const uint32_t per_slot_us =
		((airtime_us > tlm_airtime_us) ? airtime_us : tlm_airtime_us) + SLOT_OVERHEAD_US;
	const uint32_t max_by_duty = zlrs_region_max_frames_per_s_mod(
		region, (req != NULL) ? req->mod : NULL, (req != NULL) ? req->frame_len : 0U);
	uint32_t hz = (req != NULL) ? req->want_hz : 0U;
	uint32_t reserve_us = 0U;

	out->clamped_by_duty = false;
	out->clamped_by_airtime = false;

	/*
	 * The other link's share depends on the data window running every n frames, and n depends on
	 * the frame rate, so iterate a few times. Even the worst case only needs an extra round or two.
	 */
	for (uint8_t iter = 0U; iter < 4U; iter++) {
		const uint32_t per_frame_us =
			airtime_us + tlm_airtime_us + 2U * SLOT_OVERHEAD_US + reserve_us;
		const uint32_t max_by_airtime =
			(per_frame_us > 0U) ? (1000000U / per_frame_us) : UINT32_MAX;
		uint32_t next_reserve = 0U;

		hz = (req != NULL) ? req->want_hz : 0U;

		if (max_by_duty < hz) {
			hz = max_by_duty;
			out->clamped_by_duty = true;
		} else {
			out->clamped_by_duty = false;
		}
		if (max_by_airtime < hz) {
			hz = max_by_airtime;
			out->clamped_by_airtime = true;
		} else {
			out->clamped_by_airtime = false;
		}

		/*
		 * Fallback: never let the rate reach 0. When duty or airtime only allow a few hertz, clamp to
		 * MIN_CONTROL_HZ but never above what airtime allows, or every frame would overrun its
		 * budget. That is what the LoRa tier does on EU868 (1% duty cannot feed tens of ms per
		 * frame).
		 */
		if (hz < MIN_CONTROL_HZ) {
			hz = MIN_CONTROL_HZ;
			if (max_by_airtime < hz) {
				hz = max_by_airtime;
			}
			if (max_by_duty < hz) {
				hz = max_by_duty;
			}
		}
		if (hz == 0U) {
			hz = 1U;
		}

		if (req == NULL || req->other_mod == NULL || req->other_frame_len == 0U) {
			break;
		}

		/* The other link's two windows, amortised per frame through data_every_n */
		{
			const uint32_t other_us =
				zlrs_mod_airtime_us(req->other_mod, req->other_frame_len);
			const uint8_t every_n =
				(uint8_t)((hz / DATA_TARGET_HZ) > 0U ? (hz / DATA_TARGET_HZ) : 1U);

			next_reserve = (2U * (other_us + SLOT_OVERHEAD_US)) / every_n;
		}

		if (next_reserve == reserve_us) {
			break;
		}
		reserve_us = next_reserve;
	}

	out->rate_hz = hz;
	out->frame_period_us = 1000000U / hz;
	out->slot_budget_us = out->frame_period_us / 4U;
	/* At narrow-band rates airtime alone exceeds a quarter frame, so budget what it needs */
	if (out->slot_budget_us < per_slot_us) {
		out->slot_budget_us = per_slot_us;
	}
	/*
	 * The ceiling follows the modulation: a sub-millisecond FLRC frame stuck past 2 ms is wedged,
	 * while a LoRa frame legitimately takes tens of ms, so a 2 ms cap would time out every single
	 * one. Take the larger of the real need and the FLRC rule of thumb.
	 */
	{
		const uint32_t cap =
			(per_slot_us > SLOT_BUDGET_MAX_US) ? per_slot_us : SLOT_BUDGET_MAX_US;

		if (out->slot_budget_us > cap) {
			out->slot_budget_us = cap;
		}
	}

	/* Telemetry and data windows: convert times per second into every n frames */
	out->tlm_every_n = (uint8_t)((hz / TLM_TARGET_HZ) > 0U ? (hz / TLM_TARGET_HZ) : 1U);
	out->data_every_n = (uint8_t)((hz / DATA_TARGET_HZ) > 0U ? (hz / DATA_TARGET_HZ) : 1U);
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

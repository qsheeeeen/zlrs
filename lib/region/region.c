/*
 * 地区配置与占空比记账，见 include/zlrs/region/region.h。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/region/region.h"

#include <stddef.h>

/*
 * 功率上限取该地区常见子频段的保守值：
 *   - FCC 15.247：902–928 MHz 允许 1 W 传导功率，取模块能力上限 22 dBm
 *   - ETSI EN 300 220：868 MHz 多数子频段 25 mW（14 dBm）+ 1% 占空比
 *   - CN 470–510 MHz：取 50 mW（17 dBm），不另设占空比
 *   - 2.4 GHz ISM：无占空比要求，取 20 dBm
 */
static const zlrs_region_t regions[ZLRS_REGION_COUNT] = {
	[ZLRS_REGION_FCC915] = { .name = "FCC915",
				 .domain = ZLRS_DOMAIN_FCC915,
				 .low_hz = 902000000U,
				 .high_hz = 928000000U,
				 .max_tx_power_dbm = 22,
				 .duty_percent = 0U,
				 .duty_window_ms = 0U },
	[ZLRS_REGION_EU868] = { .name = "EU868",
				.domain = ZLRS_DOMAIN_EU868,
				.low_hz = 863000000U,
				.high_hz = 870000000U,
				.max_tx_power_dbm = 14,
				.duty_percent = 1U,
				.duty_window_ms = 60000U },
	[ZLRS_REGION_CN470] = { .name = "CN470",
				.domain = ZLRS_DOMAIN_CN470,
				.low_hz = 470000000U,
				.high_hz = 510000000U,
				.max_tx_power_dbm = 17,
				.duty_percent = 0U,
				.duty_window_ms = 0U },
};

const zlrs_region_t *zlrs_region_get(uint8_t id)
{
	if (id >= ZLRS_REGION_COUNT) {
		return NULL;
	}

	return &regions[id];
}

int zlrs_region_validate(const zlrs_region_t *region, int8_t tx_power_dbm)
{
	const zlrs_fhss_domain_t *domain;
	uint32_t first;
	uint32_t last;

	if (region == NULL || region->domain >= ZLRS_DOMAIN_COUNT) {
		return -1;
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

uint32_t zlrs_airtime_ms(uint16_t frame_len, uint32_t rate_bps)
{
	uint64_t bits;

	if (rate_bps == 0U) {
		return 0U;
	}

	/* 只算载荷与开销的净时间，前导码/同步字按 32 bit 粗算 */
	bits = ((uint64_t)frame_len + 4U) * 8U;

	return (uint32_t)((bits * 1000U + rate_bps - 1U) / rate_bps);
}

void zlrs_duty_init(zlrs_duty_t *duty, const zlrs_region_t *region, uint32_t now_ms)
{
	duty->window_ms = (region != NULL) ? region->duty_window_ms : 0U;
	duty->limit_percent = (region != NULL) ? region->duty_percent : 0U;
	duty->limited = (region != NULL) && (region->duty_percent > 0U) &&
			(region->duty_window_ms > 0U);
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

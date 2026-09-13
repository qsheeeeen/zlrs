/*
 * 地区配置与占空比记账，见 include/zlrs/region/region.h。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/region/region.h"

#include <stddef.h>

/*
 * 功率与速率上限取该地区常见子频段的保守值（上板前还要按当地法规核对，
 * 见 doc/05-roadmap.md 的 M6）：
 *   - FCC 15.247：902–928 MHz 允许 1 W 传导功率，取模块能力上限 22 dBm；
 *     频带宽，速率不受限
 *   - ETSI EN 300 220：868 MHz 多数子频段 25 mW（14 dBm）+ 1% 占空比；
 *     信道 500 kHz 间隔，只放得下 300 kHz 带宽的窄带档
 *   - CN 470–510 MHz：取 50 mW（17 dBm），不设占空比；300 kHz 间隔 → 窄带档
 *   - CN 433.05–434.79 MHz：微功率短距离设备，取 10 mW（10 dBm）；1.74 MHz
 *     宽、5 个信道，只能窄带档
 *
 * band 是硬件属性：FCC915/EU868 要 HF SKU，CN470/CN433 要 LF SKU。
 */
static const zlrs_region_t regions[ZLRS_REGION_COUNT] = {
	[ZLRS_REGION_FCC915] = { .name = "FCC915",
				 .domain = ZLRS_DOMAIN_FCC915,
				 .band = ZLRS_BAND_SUBG_HF,
				 .low_hz = 902000000U,
				 .high_hz = 928000000U,
				 .max_tx_power_dbm = 22,
				 .duty_percent = 0U,
				 .duty_window_ms = 0U,
				 .max_rate_bps = 2600000U },
	[ZLRS_REGION_EU868] = { .name = "EU868",
				.domain = ZLRS_DOMAIN_EU868,
				.band = ZLRS_BAND_SUBG_HF,
				.low_hz = 863000000U,
				.high_hz = 870000000U,
				.max_tx_power_dbm = 14,
				.duty_percent = 1U,
				.duty_window_ms = 60000U,
				.max_rate_bps = 325000U },
	[ZLRS_REGION_CN470] = { .name = "CN470",
				.domain = ZLRS_DOMAIN_CN470,
				.band = ZLRS_BAND_SUBG_LF,
				.low_hz = 470000000U,
				.high_hz = 510000000U,
				.max_tx_power_dbm = 17,
				.duty_percent = 0U,
				.duty_window_ms = 0U,
				.max_rate_bps = 325000U },
	[ZLRS_REGION_CN433] = { .name = "CN433",
				.domain = ZLRS_DOMAIN_CN433,
				.band = ZLRS_BAND_SUBG_LF,
				.low_hz = 433050000U,
				.high_hz = 434790000U,
				.max_tx_power_dbm = 10,
				.duty_percent = 0U,
				.duty_window_ms = 0U,
				.max_rate_bps = 325000U },
};

uint32_t zlrs_region_clamp_rate(const zlrs_region_t *region, uint32_t rate_bps)
{
	if (region == NULL || region->max_rate_bps == 0U) {
		return rate_bps;
	}

	return (rate_bps > region->max_rate_bps) ? region->max_rate_bps : rate_bps;
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
	/* 地区与本机 sub-G 硬件必须匹配：LF 板跑不了 915，HF 板跑不了 470/433 */
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

	/* 只算载荷与开销的净时间，前导码/同步字按 32 bit 粗算 */
	bits = ((uint64_t)frame_len + 4U) * 8U;

	return (uint32_t)((bits * 1000000U + rate_bps - 1U) / rate_bps);
}

uint32_t zlrs_airtime_ms(uint16_t frame_len, uint32_t rate_bps)
{
	const uint32_t us = zlrs_airtime_us(frame_len, rate_bps);

	return (us + 999U) / 1000U;
}

uint32_t zlrs_region_max_frames_per_s(const zlrs_region_t *region, uint32_t rate_bps,
				      uint16_t frame_len)
{
	const uint32_t airtime_us = zlrs_airtime_us(frame_len, rate_bps);
	uint64_t budget_us;

	if (region == NULL || region->duty_percent == 0U || airtime_us == 0U) {
		return UINT32_MAX;
	}

	/* 1% = 每秒钟 10000 µs 的发射额度 */
	budget_us = (uint64_t)region->duty_percent * 10000U;

	return (uint32_t)(budget_us / airtime_us);
}

/* 每个时隙除了空中时间，还要留出 RAC 调度、模式翻转与 SPI 的余量 */
#define SLOT_OVERHEAD_US 200U
/* 时隙预算封顶：再长也只是等一个回不来的事务 */
#define SLOT_BUDGET_MAX_US 2000U
/* 遥测与数据窗口的目标速率（每秒多少次） */
#define TLM_TARGET_HZ 150U
#define DATA_TARGET_HZ 100U

void zlrs_region_control_plan(const zlrs_region_t *region, uint32_t want_hz,
			      uint32_t rate_bps, uint16_t frame_len,
			      zlrs_control_plan_t *out)
{
	const uint32_t airtime_us = zlrs_airtime_us(frame_len, rate_bps);
	/* 一帧至少要装下两个控制时隙：发 RC + 收遥测 */
	const uint32_t per_slot_us = airtime_us + SLOT_OVERHEAD_US;
	const uint32_t max_by_airtime = (per_slot_us > 0U) ? (1000000U / (2U * per_slot_us))
							   : UINT32_MAX;
	const uint32_t max_by_duty = zlrs_region_max_frames_per_s(region, rate_bps, frame_len);
	uint32_t hz = want_hz;

	out->clamped_by_duty = false;
	out->clamped_by_airtime = false;

	if (max_by_duty < hz) {
		hz = max_by_duty;
		out->clamped_by_duty = true;
	}
	if (max_by_airtime < hz) {
		hz = max_by_airtime;
		out->clamped_by_airtime = true;
	}
	if (hz < 10U) {
		hz = 10U; /* 再低就没法飞了：留给日志去报 */
	}

	out->rate_hz = hz;
	out->frame_period_us = 1000000U / hz;
	out->slot_budget_us = out->frame_period_us / 4U;
	/* 窄带速率下空中时间本身就超过四分之一帧，按实际需要给预算 */
	if (out->slot_budget_us < per_slot_us) {
		out->slot_budget_us = per_slot_us;
	}
	if (out->slot_budget_us > SLOT_BUDGET_MAX_US) {
		out->slot_budget_us = SLOT_BUDGET_MAX_US;
	}

	/* 遥测/数据窗口按"每秒大约多少次"折算成"每 n 帧一次" */
	out->tlm_every_n = (uint8_t)((hz / TLM_TARGET_HZ) > 0U ? (hz / TLM_TARGET_HZ) : 1U);
	out->data_every_n = (uint8_t)((hz / DATA_TARGET_HZ) > 0U ? (hz / DATA_TARGET_HZ) : 1U);
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

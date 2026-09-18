/*
 * 地区配置与占空比记账，见 include/zlrs/region/region.h。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/region/region.h"

#include <stddef.h>

/*
 * 功率与速率上限取各地区常见子频段的保守值（还要按当地法规核对，见 roadmap M6）：
 * FCC 15.247 取模块上限 22 dBm；ETSI EN 300 220 取 14 dBm + 1%；CN470 取 17 dBm；
 * CN433 微功率取 10 dBm。band 是硬件属性：FCC915/EU868 要 HF SKU，其余要 LF SKU。
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

zlrs_mod_cfg_t zlrs_region_clamp_mod(const zlrs_region_t *region, zlrs_mod_cfg_t mod)
{
	/* 窄频段地区的步进是按 FLRC 的带宽定的（见 lib/proto/fhss.c），
	   LoRa 的带宽也得塞进地区边界里 */
	static const uint32_t bws[] = { 500000U, 250000U, 125000U };

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

	/* 从宽到窄找一个既不超过申请值、又塞得进地区边界的带宽 */
	for (uint8_t i = 0U; i < sizeof(bws) / sizeof(bws[0]); i++) {
		if (bws[i] <= mod.bw_hz && bws[i] <= (region->high_hz - region->low_hz)) {
			mod.bw_hz = bws[i];
			return mod;
		}
	}

	/* 一个都放不下：原样返回，让启动校验去喊 */
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

uint32_t zlrs_airtime_mod_us(const zlrs_mod_cfg_t *mod, uint16_t frame_len)
{
	return zlrs_mod_airtime_us(mod, frame_len);
}

uint32_t zlrs_airtime_mod_ms(const zlrs_mod_cfg_t *mod, uint16_t frame_len)
{
	return zlrs_mod_airtime_ms(mod, frame_len);
}

/* 每秒能发多少帧：额度按占空比给，一帧的代价是它的空中时间 */
static uint32_t max_frames_for_airtime(const zlrs_region_t *region, uint32_t airtime_us)
{
	uint64_t budget_us;

	if (region == NULL || region->duty_percent == 0U || airtime_us == 0U) {
		return UINT32_MAX;
	}

	/* 1% = 每秒钟 10000 µs 的发射额度 */
	budget_us = (uint64_t)region->duty_percent * 10000U;

	return (uint32_t)(budget_us / airtime_us);
}

uint32_t zlrs_region_max_frames_per_s(const zlrs_region_t *region, uint32_t rate_bps,
				      uint16_t frame_len)
{
	return max_frames_for_airtime(region, zlrs_airtime_us(frame_len, rate_bps));
}

uint32_t zlrs_region_max_frames_per_s_mod(const zlrs_region_t *region,
					  const zlrs_mod_cfg_t *mod, uint16_t frame_len)
{
	return max_frames_for_airtime(region, zlrs_mod_airtime_us(mod, frame_len));
}

/* 每个时隙除了空中时间，还要留出 RAC 调度、模式翻转与 SPI 的余量 */
#define SLOT_OVERHEAD_US 200U
/* 时隙预算封顶：再长也只是等一个回不来的事务 */
#define SLOT_BUDGET_MAX_US 2000U
/* 帧率下限：再低就没法飞了（占空比压过头时按它兜底，但不会超过空中时间允许的上限） */
#define MIN_CONTROL_HZ 10U
/* 遥测与数据窗口的目标速率（每秒多少次） */
#define TLM_TARGET_HZ 150U
#define DATA_TARGET_HZ 100U

void zlrs_region_control_plan(const zlrs_plan_req_t *req, zlrs_control_plan_t *out)
{
	const zlrs_region_t *region = (req != NULL) ? req->region : NULL;
	const uint16_t tlm_len = (req != NULL && req->tlm_len > 0U) ? req->tlm_len
								   : ((req != NULL) ? req->frame_len : 0U);
	const uint32_t airtime_us = zlrs_mod_airtime_us((req != NULL) ? req->mod : NULL,
							(req != NULL) ? req->frame_len : 0U);
	const uint32_t tlm_airtime_us = zlrs_mod_airtime_us((req != NULL) ? req->mod : NULL,
							    tlm_len);
	/* 一帧至少要装下两个控制时隙：发 RC + 收遥测，外加另一条链路的预留 */
	const uint32_t per_slot_us =
		((airtime_us > tlm_airtime_us) ? airtime_us : tlm_airtime_us) +
		SLOT_OVERHEAD_US;
	const uint32_t max_by_duty = zlrs_region_max_frames_per_s_mod(
		region, (req != NULL) ? req->mod : NULL, (req != NULL) ? req->frame_len : 0U);
	uint32_t hz = (req != NULL) ? req->want_hz : 0U;
	uint32_t reserve_us = 0U;

	out->clamped_by_duty = false;
	out->clamped_by_airtime = false;

	/*
	 * 另一条链路的预留取决于"数据窗口每 n 帧跑一次"，而 n 又取决于帧率——
	 * 迭代几轮让它收敛。不收敛的极端情况（帧率掉档）也只会多做一两轮。
	 */
	for (uint8_t iter = 0U; iter < 4U; iter++) {
		const uint32_t per_frame_us = airtime_us + tlm_airtime_us +
					      2U * SLOT_OVERHEAD_US + reserve_us;
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
		 * 兜底：不让帧率掉到 0。占空比或空中时间只够几赫兹时，压到
		 * MIN_CONTROL_HZ，但不会超过空中时间允许的上限——否则每一帧都会
		 * 超预算。LoRa 远距档在 EU868 上就是这个下场（1% 占空比撑不住
		 * 几十毫秒的帧）。
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

		/* 另一条链路的两个窗口，按 data_every_n 摊到每帧 */
		{
			const uint32_t other_us =
				zlrs_mod_airtime_us(req->other_mod, req->other_frame_len);
			const uint8_t every_n = (uint8_t)((hz / DATA_TARGET_HZ) > 0U
								  ? (hz / DATA_TARGET_HZ)
								  : 1U);

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
	/* 窄带速率下空中时间本身就超过四分之一帧，按实际需要给预算 */
	if (out->slot_budget_us < per_slot_us) {
		out->slot_budget_us = per_slot_us;
	}
	/*
	 * 上限跟着调制档走：FLRC 的亚毫秒帧等超过 2 ms 就是卡住了，LoRa 的一帧
	 * 本来就要几十毫秒，压到 2 ms 等于每帧都超时——上限取"实际需要的时间"与
	 * "FLRC 的经验上限"里的较大者。
	 */
	{
		const uint32_t cap = (per_slot_us > SLOT_BUDGET_MAX_US) ? per_slot_us
								       : SLOT_BUDGET_MAX_US;

		if (out->slot_budget_us > cap) {
			out->slot_budget_us = cap;
		}
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

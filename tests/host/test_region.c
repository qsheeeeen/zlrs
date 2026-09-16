/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 地区配置层测试：三张表的信道表都要落在频段边界内、功率不超上限；
 * 占空比按滑动窗口记账，超了就拦下来并计数。
 */

#include "test.h"

#include "zlrs/region/region.h"

static void test_tables(void)
{
	for (uint8_t i = 0U; i < ZLRS_REGION_COUNT; i++) {
		const zlrs_region_t *region = zlrs_region_get(i);

		CHECK(region != NULL);
		CHECK(region->name != NULL);
		/* 本机 band 一致、功率在上限内：必须通过 */
		CHECK(zlrs_region_validate(region, region->band,
					   region->max_tx_power_dbm) == 0);
		/* 功率超一点就该被拦 */
		if (region->max_tx_power_dbm < 22) {
			CHECK(zlrs_region_validate(region, region->band,
						   (int8_t)(region->max_tx_power_dbm + 1)) !=
			      0);
		}
		/* band 不匹配就该被拦：LF 板跑不了 915，HF 板跑不了 470/433 */
		CHECK(zlrs_region_validate(region,
					   (region->band == ZLRS_BAND_SUBG_HF)
						   ? ZLRS_BAND_SUBG_LF
						   : ZLRS_BAND_SUBG_HF,
					   region->max_tx_power_dbm) != 0);
	}

	CHECK(zlrs_region_get(ZLRS_REGION_COUNT) == NULL);
	CHECK(zlrs_region_validate(NULL, ZLRS_BAND_SUBG_HF, 0) != 0);

	/* 具体到原型：HF 板只认 FCC915/EU868，LF 板只认 CN470/CN433 */
	CHECK(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_FCC915), ZLRS_BAND_SUBG_HF,
				   10) == 0);
	CHECK(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_FCC915), ZLRS_BAND_SUBG_LF,
				   10) == -5);
	CHECK(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_CN470), ZLRS_BAND_SUBG_LF,
				   10) == 0);
	CHECK(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_CN433), ZLRS_BAND_SUBG_LF,
				   10) == 0);
	CHECK(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_CN433), ZLRS_BAND_SUBG_HF,
				   10) == -5);
}

/* 速率上限：频段窄的地区要把配置的速率压下来 */
static void test_rate_limit(void)
{
	printf("region: rate limit\n");

	CHECK(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_FCC915), 2600000U) ==
	      2600000U);
	CHECK(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_FCC915), 1300000U) ==
	      1300000U);
	/* EU868 / CN470 / CN433 放不下宽带档 */
	CHECK(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_EU868), 1300000U) ==
	      325000U);
	CHECK(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_CN470), 2600000U) ==
	      325000U);
	CHECK(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_CN433), 650000U) ==
	      325000U);
	/* 本来就低于上限的不动 */
	CHECK(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_CN433), 325000U) ==
	      325000U);
	CHECK(zlrs_region_clamp_rate(NULL, 1300000U) == 1300000U);
}

static void test_airtime(void)
{
	printf("region: airtime\n");

	/* 128 字节 @ 1.3 Mbps：(128+4)*8 bit = 812.3 µs，向上取整 813 µs */
	CHECK(zlrs_airtime_us(128U, 1300000U) == 813U);
	/* 同样的帧在 0.325 Mbps 上约 3.25 ms */
	CHECK(zlrs_airtime_us(128U, 325000U) == 3250U);
	/* 128 字节 @ 1.3 Mbps：(128+4)*8 bit ≈ 812 µs，向上取整到 1 ms */
	CHECK(zlrs_airtime_ms(128U, 1300000U) == 1U);
	/* 同样的帧在 0.325 Mbps 上约 3.25 ms */
	CHECK(zlrs_airtime_ms(128U, 325000U) == 4U);
	/* 速率未知时不记账 */
	CHECK(zlrs_airtime_ms(128U, 0U) == 0U);
	CHECK(zlrs_airtime_us(128U, 0U) == 0U);
}

/* 地区占空比 → 控制帧率上限：EU868 的 1% 撑不起 150 Hz，FCC/CN 不限 */
static void test_max_frames(void)
{
	/* 31 字节控制帧 @1.3 Mbps：(31+4)*8/1.3e6 = 215.4 µs，向上取整 216 µs */
	printf("region: max control rate\n");

	CHECK(zlrs_airtime_us(31U, 1300000U) == 216U);

	/* 1% → 10000 µs/s → 10000/216 = 46 帧/秒 */
	CHECK(zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_EU868), 1300000U,
					   31U) == 46U);
	/* 不限占空比的地区：想跑多少跑多少 */
	CHECK(zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_FCC915), 1300000U,
					   31U) == UINT32_MAX);
	CHECK(zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_CN470), 1300000U,
					   31U) == UINT32_MAX);
	/* 速率未知时不设限（交给上层） */
	CHECK(zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_EU868), 0U, 31U) ==
	      UINT32_MAX);
}

static void test_duty(void)
{
	const zlrs_region_t *eu = zlrs_region_get(ZLRS_REGION_EU868);
	const zlrs_region_t *fcc = zlrs_region_get(ZLRS_REGION_FCC915);
	zlrs_duty_t duty;
	uint32_t now = 1000U;

	printf("region: duty cycle\n");

	/* EU868：1% / 60 秒 = 600 ms 预算 */
	zlrs_duty_init(&duty, eu, now);
	CHECK(duty.limited);
	CHECK(zlrs_duty_remaining_ms(&duty, now) == 600U);

	/* 一次 4 ms 的发射后还剩 596 ms */
	CHECK(zlrs_duty_reserve(&duty, 4U, now));
	CHECK(zlrs_duty_remaining_ms(&duty, now) == 596U);

	/* 把剩下的额度用光（每次 100 ms 的长时间发射），再发就被拦下并计数 */
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(zlrs_duty_reserve(&duty, 100U, now));
	}
	CHECK(zlrs_duty_remaining_ms(&duty, now) == 96U);
	CHECK(!zlrs_duty_reserve(&duty, 100U, now));
	CHECK(zlrs_duty_reserve(&duty, 96U, now));
	CHECK(zlrs_duty_remaining_ms(&duty, now) == 0U);
	CHECK(!zlrs_duty_reserve(&duty, 1U, now));
	CHECK(duty.denied == 2U); /* 刚才那次 100 ms 也被拦过 */

	/* 窗口翻篇：额度回来 */
	now += 60001U;
	CHECK(zlrs_duty_remaining_ms(&duty, now) == 600U);
	CHECK(zlrs_duty_reserve(&duty, 4U, now));

	/* FCC915 不限制占空比：随便发，计数器也不动 */
	zlrs_duty_init(&duty, fcc, now);
	CHECK(!duty.limited);
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(zlrs_duty_reserve(&duty, 4U, now));
	}
	CHECK(zlrs_duty_remaining_ms(&duty, now) == UINT32_MAX);
	CHECK(duty.denied == 0U);
}

/* 控制计划：帧率受占空比与空中时间双重约束，节奏系数跟着帧率折算 */
static void test_control_plan(void)
{
	zlrs_control_plan_t plan;
	const zlrs_mod_cfg_t fast = zlrs_mod_flrc(1300000U);
	const zlrs_mod_cfg_t narrow = zlrs_mod_flrc(325000U);
	zlrs_plan_req_t req = {
		.mod = &fast,
		.frame_len = 31U,
	};

	printf("region: control plan\n");

	/* 默认档：FCC915 + 1.3 Mbps，150 Hz 原样跑 */
	req.region = zlrs_region_get(ZLRS_REGION_FCC915);
	req.want_hz = 150U;
	zlrs_region_control_plan(&req, &plan);
	CHECK(plan.rate_hz == 150U);
	CHECK(plan.frame_period_us == 6666U);
	CHECK(plan.slot_budget_us == 1666U);
	CHECK(plan.tlm_every_n == 1U);
	CHECK(plan.data_every_n == 1U);
	CHECK(!plan.clamped_by_duty && !plan.clamped_by_airtime);

	/* 500 Hz 档：FCC915 不限占空比，1.3 Mbps 的空中时间也撑得住 */
	req.want_hz = 500U;
	zlrs_region_control_plan(&req, &plan);
	CHECK(plan.rate_hz == 500U);
	CHECK(plan.frame_period_us == 2000U);
	CHECK(plan.slot_budget_us == 500U);
	CHECK(plan.tlm_every_n == 3U);  /* ≈167 次/秒 */
	CHECK(plan.data_every_n == 5U); /* =100 次/秒 */
	CHECK(!plan.clamped_by_duty && !plan.clamped_by_airtime);

	/* EU868 的 1% 占空比把 500 Hz 压到 46 Hz */
	req.region = zlrs_region_get(ZLRS_REGION_EU868);
	zlrs_region_control_plan(&req, &plan);
	CHECK(plan.rate_hz == 46U);
	CHECK(plan.clamped_by_duty);
	CHECK(!plan.clamped_by_airtime);
	CHECK(plan.tlm_every_n == 1U); /* 帧率已经低于目标速率：每帧都发 */

	/* 窄带速率（CN470 被压到 0.325 Mbps）下，帧率受空中时间限制 */
	req.region = zlrs_region_get(ZLRS_REGION_CN470);
	req.mod = &narrow;
	zlrs_region_control_plan(&req, &plan);
	CHECK(plan.clamped_by_airtime);
	/* 空中时间 862 µs + 200 µs 余量 → 一帧两个控制时隙 ≈ 470 Hz */
	CHECK(plan.rate_hz == 470U);
	/* 四分之一帧（531 µs）装不下 1062 µs 的时隙，预算按实际需要给 */
	CHECK(plan.slot_budget_us == 1062U);
}

/*
 * LoRa 远距档（doc/03-protocol.md 4.1.1）：一帧就是几十毫秒，帧率由空中时间
 * 定，不再是配置里写多少就是多少。
 */
static void test_lora_plan(void)
{
	const zlrs_mod_cfg_t lora = zlrs_mod_lora(7U, 125000U, 1U, 8U);
	const zlrs_mod_cfg_t sub_g_slow = zlrs_mod_flrc(325000U);
	zlrs_control_plan_t plan;
	zlrs_plan_req_t req = {
		.region = NULL, /* FCC915：不限占空比 */
		.mod = &lora,
		.want_hz = 10U,
		.frame_len = 31U,
		.tlm_len = 7U,
	};

	printf("region: lora control plan\n");

	req.region = zlrs_region_get(ZLRS_REGION_FCC915);
	zlrs_region_control_plan(&req, &plan);

	/* 10 Hz 装不下（72 + 36 + 余量 = 108 ms），压到 9 Hz */
	CHECK(plan.clamped_by_airtime);
	CHECK(!plan.clamped_by_duty);
	CHECK(plan.rate_hz == 9U);
	CHECK(plan.frame_period_us == 111111U);
	/* 时隙预算要容得下 72 ms 的 RC 帧——FLRC 的 2 ms 上限在这里不适用 */
	CHECK(plan.slot_budget_us >= 71936U + 200U);

	/* 数据链路在 sub-G 跑窄带：两个 135 B 的窗口摊进来，帧率再掉一点 */
	req.other_mod = &sub_g_slow;
	req.other_frame_len = 135U;
	zlrs_region_control_plan(&req, &plan);
	CHECK(plan.rate_hz == 8U);
	CHECK(plan.frame_period_us >= 108432U + 2U * 3622U);

	/* EU868 的 1% 占空比撑不住 LoRa：告警归告警，帧率兜底到 1 Hz */
	req.region = zlrs_region_get(ZLRS_REGION_EU868);
	req.other_mod = NULL;
	req.other_frame_len = 0U;
	zlrs_region_control_plan(&req, &plan);
	CHECK(plan.clamped_by_duty);
	CHECK(plan.rate_hz == 1U);
}

/*
 * 竞速模式（doc/01-system.md 3.2）：控制链路独占 2.4G——它没有地区表，占空比
 * 门不作用在它身上；让到 sub-G 的那条链路（数据）才受法规约束，它的窗口按
 * data_every_n 摊进超帧，峰值帧率被拖下来一点。
 */
static void test_racing_plan(void)
{
	const zlrs_mod_cfg_t ctl = zlrs_mod_flrc(2600000U);
	const zlrs_mod_cfg_t sub_g_slow = zlrs_mod_flrc(325000U);
	zlrs_control_plan_t plan;
	zlrs_plan_req_t req = {
		.region = NULL, /* 2.4G：没有地区表 */
		.mod = &ctl,
		.want_hz = 500U,
		.frame_len = 31U,
		.tlm_len = 15U,
		.other_mod = &sub_g_slow,
		.other_frame_len = 135U,
	};

	printf("region: racing plan (control on 2.4g, data on sub-g)\n");

	zlrs_region_control_plan(&req, &plan);

	/* 不受占空比限制，但 sub-G 数据窗口的预留把峰值压到 420 Hz */
	CHECK(!plan.clamped_by_duty);
	CHECK(plan.clamped_by_airtime);
	CHECK(plan.rate_hz == 420U);
	/* 数据窗口每 4 帧一次：7204 µs 的预留摊到每帧 1811 µs，和帧率自洽 */
	CHECK(plan.data_every_n == 4U);
	CHECK(plan.frame_period_us == 2380U);
}

void test_region(void)
{
	printf("region: tables\n");
	test_tables();
	test_rate_limit();
	test_airtime();
	test_max_frames();
	test_control_plan();
	test_lora_plan();
	test_racing_plan();
	test_duty();
}

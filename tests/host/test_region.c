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

	printf("region: control plan\n");

	/* 默认档：FCC915 + 1.3 Mbps，150 Hz 原样跑 */
	zlrs_region_control_plan(zlrs_region_get(ZLRS_REGION_FCC915), 150U, 1300000U, 31U,
				 &plan);
	CHECK(plan.rate_hz == 150U);
	CHECK(plan.frame_period_us == 6666U);
	CHECK(plan.slot_budget_us == 1666U);
	CHECK(plan.tlm_every_n == 1U);
	CHECK(plan.data_every_n == 1U);
	CHECK(!plan.clamped_by_duty && !plan.clamped_by_airtime);

	/* 500 Hz 档：FCC915 不限占空比，1.3 Mbps 的空中时间也撑得住 */
	zlrs_region_control_plan(zlrs_region_get(ZLRS_REGION_FCC915), 500U, 1300000U, 31U,
				 &plan);
	CHECK(plan.rate_hz == 500U);
	CHECK(plan.frame_period_us == 2000U);
	CHECK(plan.slot_budget_us == 500U);
	CHECK(plan.tlm_every_n == 3U);  /* ≈167 次/秒 */
	CHECK(plan.data_every_n == 5U); /* =100 次/秒 */
	CHECK(!plan.clamped_by_duty && !plan.clamped_by_airtime);

	/* EU868 的 1% 占空比把 500 Hz 压到 46 Hz */
	zlrs_region_control_plan(zlrs_region_get(ZLRS_REGION_EU868), 500U, 1300000U, 31U,
				 &plan);
	CHECK(plan.rate_hz == 46U);
	CHECK(plan.clamped_by_duty);
	CHECK(!plan.clamped_by_airtime);
	CHECK(plan.tlm_every_n == 1U); /* 帧率已经低于目标速率：每帧都发 */

	/* 窄带速率（CN470 被压到 0.325 Mbps）下，帧率受空中时间限制 */
	zlrs_region_control_plan(zlrs_region_get(ZLRS_REGION_CN470), 500U, 325000U, 31U,
				 &plan);
	CHECK(plan.clamped_by_airtime);
	/* 空中时间 862 µs + 200 µs 余量 → 一帧两个控制时隙 ≈ 470 Hz */
	CHECK(plan.rate_hz == 470U);
	/* 四分之一帧（531 µs）装不下 1062 µs 的时隙，预算按实际需要给 */
	CHECK(plan.slot_budget_us == 1062U);
}

void test_region(void)
{
	printf("region: tables\n");
	test_tables();
	test_rate_limit();
	test_airtime();
	test_max_frames();
	test_control_plan();
	test_duty();
}

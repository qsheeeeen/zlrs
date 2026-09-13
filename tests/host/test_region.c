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

	/* 128 字节 @ 1.3 Mbps：(128+4)*8 bit ≈ 812 µs，向上取整到 1 ms */
	CHECK(zlrs_airtime_ms(128U, 1300000U) == 1U);
	/* 同样的帧在 0.325 Mbps 上约 3.25 ms */
	CHECK(zlrs_airtime_ms(128U, 325000U) == 4U);
	/* 速率未知时不记账 */
	CHECK(zlrs_airtime_ms(128U, 0U) == 0U);
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

void test_region(void)
{
	printf("region: tables\n");
	test_tables();
	test_rate_limit();
	test_airtime();
	test_duty();
}

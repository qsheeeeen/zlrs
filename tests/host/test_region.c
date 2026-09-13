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
		/* 功率上限之内的配置必须通过校验 */
		CHECK(zlrs_region_validate(region, region->max_tx_power_dbm) == 0);
		/* 超一点就该被拦 */
		if (region->max_tx_power_dbm < 22) {
			CHECK(zlrs_region_validate(region, (int8_t)(region->max_tx_power_dbm + 1)) !=
			      0);
		}
	}

	CHECK(zlrs_region_get(ZLRS_REGION_COUNT) == NULL);
	CHECK(zlrs_region_validate(NULL, 0) != 0);
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
	test_airtime();
	test_duty();
}

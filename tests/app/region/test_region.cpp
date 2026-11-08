/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Region layer tests: every channel plan stays inside its band edges, power within the ceiling,
 * duty is accounted over a sliding window and overruns are refused and counted.
 */

#include <zephyr/ztest.h>
#include <initializer_list>
#include <cstdint>
#include <cstdio>

#include "region/region.h"
#include <zephyr/sys/util.h>

ZTEST(Region, test_PredefinedPlansAreBounded)
{
	for (unsigned int id = 0; id < ZLRS_REGION_COUNT; id++) {
		const auto *region = zlrs_region_get(id);
		zassert_not_null(region);
		zassert_true(region->domain < ZLRS_DOMAIN_COUNT);
		const auto &domain = zlrs_fhss_domains[region->domain];
		zexpect_true(domain.channel_count > 0);
		zexpect_true(domain.start_hz >= region->low_hz);
		zexpect_true(domain.start_hz + (domain.channel_count - 1) * domain.step_hz <=
			     region->high_hz);
	}
	zexpect_is_null(zlrs_region_get(ZLRS_REGION_COUNT));
	zexpect_is_null(zlrs_region_get(255));
}

ZTEST(Region, test_AllPredefinedRegionsCanBelongToOneBoard)
{
	zexpect_equal(zlrs_region_supported_mask(), BIT_MASK(ZLRS_REGION_COUNT));
	for (unsigned int id = 0; id < ZLRS_REGION_COUNT; id++) {
		zexpect_not_null(zlrs_region_get(id));
	}
}

/* Rate ceiling: narrow bands pull the configured rate down */
ZTEST(Region, test_RateCeilings)
{
	printf("region: rate limit\n");

	zexpect_equal(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_FCC915), 2600000U),
		      2600000U);
	zexpect_equal(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_FCC915), 1300000U),
		      1300000U);
	/* EU868, CN470 and CN433 cannot fit the wideband tiers */
	zexpect_equal(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_EU868), 1300000U),
		      325000U);
	zexpect_equal(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_CN470), 2600000U),
		      325000U);
	zexpect_equal(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_CN433), 650000U), 325000U);
	/* Anything already below the ceiling stays put */
	zexpect_equal(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_CN433), 325000U), 325000U);
	zexpect_equal(zlrs_region_clamp_rate(nullptr, 1300000U), 1300000U);
}

ZTEST(Region, test_AirtimeRounding)
{
	printf("region: airtime\n");

	/* 128 bytes at 1.3 Mbps: (128+4)*8 bit = 812.3 us, rounded up to 813 us */
	zexpect_equal(zlrs_airtime_us(128U, 1300000U), 813U);
	/* The same frame at 0.325 Mbps is about 3.25 ms */
	zexpect_equal(zlrs_airtime_us(128U, 325000U), 3250U);
	/* 128 bytes at 1.3 Mbps: (128+4)*8 bit ~ 812 us, rounded up to 1 ms */
	zexpect_equal(zlrs_airtime_ms(128U, 1300000U), 1U);
	/* The same frame at 0.325 Mbps is about 3.25 ms */
	zexpect_equal(zlrs_airtime_ms(128U, 325000U), 4U);
	/* No accounting when the rate is unknown */
	zexpect_equal(zlrs_airtime_ms(128U, 0U), 0U);
	zexpect_equal(zlrs_airtime_us(128U, 0U), 0U);
}

/* Region duty limits maximum DATA frame cadence in constrained bands */
ZTEST(Region, test_MaximumCadence)
{
	/* 31-byte data frame at 1.3 Mbps: (31+4)*8/1.3e6 = 215.4 us, rounded up to 216 us */
	printf("region: max frame rate\n");

	zexpect_equal(zlrs_airtime_us(31U, 1300000U), 216U);

	/* 1% -> 10000 us/s -> 10000/216 = 46 frames per second */
	zexpect_equal(
		zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_EU868), 1300000U, 31U),
		46U);
	/* Unlimited-duty regions: as fast as you like */
	zexpect_equal(
		zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_FCC915), 1300000U, 31U),
		UINT32_MAX);
	zexpect_equal(
		zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_CN470), 1300000U, 31U),
		UINT32_MAX);
	/* No ceiling when the rate is unknown (the layer above decides) */
	zexpect_equal(zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_EU868), 0U, 31U),
		      UINT32_MAX);
}

ZTEST(Region, test_DutyBudgetAndWindowReset)
{
	const zlrs_region_t *eu = zlrs_region_get(ZLRS_REGION_EU868);
	const zlrs_region_t *fcc = zlrs_region_get(ZLRS_REGION_FCC915);
	zlrs_duty_t duty;
	uint32_t now = 1000U;

	printf("region: duty cycle\n");

	/* EU868: 1% of 60 s = 600 ms of budget */
	zlrs_duty_init(&duty, eu, now);
	zexpect_true(duty.window_ms != 0U);
	zexpect_equal(zlrs_duty_remaining_ms(&duty, now), 600U);

	/* One 4 ms transmission leaves 596 ms */
	zexpect_true(zlrs_duty_reserve(&duty, 4U, now));
	zexpect_equal(zlrs_duty_remaining_ms(&duty, now), 596U);

	/* Spend the rest in 100 ms chunks; the next attempt is refused and counted */
	for (uint8_t i = 0U; i < 5U; i++) {
		zexpect_true(zlrs_duty_reserve(&duty, 100U, now));
	}
	zexpect_equal(zlrs_duty_remaining_ms(&duty, now), 96U);
	zexpect_false(zlrs_duty_reserve(&duty, 100U, now));
	zexpect_true(zlrs_duty_reserve(&duty, 96U, now));
	zexpect_equal(zlrs_duty_remaining_ms(&duty, now), 0U);
	zexpect_false(zlrs_duty_reserve(&duty, 1U, now));
	zexpect_equal(duty.denied, 2U); /* that 100 ms attempt was refused too */

	/* New window: the budget is back */
	now += 60001U;
	zexpect_equal(zlrs_duty_remaining_ms(&duty, now), 600U);
	zexpect_true(zlrs_duty_reserve(&duty, 4U, now));

	/* FCC915 has no duty limit: transmit freely, no counter moves */
	zlrs_duty_init(&duty, fcc, now);
	zexpect_false(duty.window_ms != 0U);
	for (uint8_t i = 0U; i < 5U; i++) {
		zexpect_true(zlrs_duty_reserve(&duty, 4U, now));
	}
	zexpect_equal(zlrs_duty_remaining_ms(&duty, now), UINT32_MAX);
	zexpect_equal(duty.denied, 0U);
}

ZTEST_SUITE(Region, NULL, NULL, NULL, NULL, NULL);

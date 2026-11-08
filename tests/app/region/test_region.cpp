/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Region layer tests: every channel plan stays inside its band edges, power within the ceiling,
 * duty is accounted over a sliding window and overruns are refused and counted.
 */

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <zephyr/ztest.h>

#include "region/region.h"
#include <zephyr/sys/util.h>

ZTEST(Region, test_PredefinedPlansAreBounded)
{
	for (unsigned int id = 0; id < ZLRS_REGION_COUNT; id++) {
		const auto *region = &zlrs_regions[id];
		zassert_not_null(region);
		zexpect_true(region->channel_count > 0);
		zexpect_true(region->start_hz >= region->low_hz);
		zexpect_true(region->start_hz + (region->channel_count - 1) * region->step_hz <=
			     region->high_hz);
	}
	zexpect_is_null(zlrs_region_get(ZLRS_REGION_COUNT));
	zexpect_is_null(zlrs_region_get(255));
}

ZTEST(Region, test_AllConfiguredRegionsAreAvailable)
{
	zexpect_equal(zlrs_region_supported_mask(), BIT_MASK(ZLRS_REGION_ISM2400));
	for (unsigned int id = 0; id < ZLRS_REGION_ISM2400; id++) {
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
	zexpect_equal(duty.budget_ms - duty.used_ms, 600U);

	/* One 4 ms transmission leaves 596 ms */
	zexpect_true(zlrs_duty_reserve(&duty, 4U, now));
	zexpect_equal(duty.budget_ms - duty.used_ms, 596U);

	/* Spend the rest in 100 ms chunks; the next attempt is refused and counted */
	for (uint8_t i = 0U; i < 5U; i++) {
		zexpect_true(zlrs_duty_reserve(&duty, 100U, now));
	}
	zexpect_equal(duty.budget_ms - duty.used_ms, 96U);
	zexpect_false(zlrs_duty_reserve(&duty, 100U, now));
	zexpect_true(zlrs_duty_reserve(&duty, 96U, now));
	zexpect_equal(duty.budget_ms - duty.used_ms, 0U);
	zexpect_false(zlrs_duty_reserve(&duty, 1U, now));
	zexpect_equal(duty.denied, 2U); /* that 100 ms attempt was refused too */

	/* New window: the budget is back */
	now += 60001U;
	zexpect_true(zlrs_duty_reserve(&duty, 4U, now));
	zexpect_equal(duty.budget_ms - duty.used_ms, 596U);

	/* FCC915 has no duty limit: transmit freely, no counter moves */
	zlrs_duty_init(&duty, fcc, now);
	zexpect_false(duty.window_ms != 0U);
	for (uint8_t i = 0U; i < 5U; i++) {
		zexpect_true(zlrs_duty_reserve(&duty, 4U, now));
	}
	zexpect_equal(duty.used_ms, 0U);
	zexpect_equal(duty.denied, 0U);
}

ZTEST_SUITE(Region, NULL, NULL, NULL, NULL, NULL);

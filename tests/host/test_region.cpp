/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Region layer tests: every channel plan stays inside its band edges, power within the ceiling,
 * duty is accounted over a sliding window and overruns are refused and counted.
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstdio>

#include "zlrs/region/region.h"

/* The two radio designs the prototype ships with (see the boards directory) */
static const zlrs_rf_design_t rf_lf = {
	.band = ZLRS_BAND_SUBG_LF, .low_hz = 410000000U, .high_hz = 510000000U};
static const zlrs_rf_design_t rf_hf = {
	.band = ZLRS_BAND_SUBG_HF, .low_hz = 850000000U, .high_hz = 930000000U};

TEST(Region, BoardAndRegionValidation)
{
	for (uint8_t i = 0U; i < ZLRS_REGION_COUNT; i++) {
		SCOPED_TRACE(i);
		const zlrs_region_t *region = zlrs_region_get(static_cast<zlrs_region_id_t>(i));
		ASSERT_NE(region, nullptr);
		ASSERT_NE(region->name, nullptr);
		const zlrs_rf_design_t *rf = (region->band == ZLRS_BAND_SUBG_HF) ? &rf_hf : &rf_lf;
		/* Matching band, power within the ceiling: must pass */
		EXPECT_EQ(zlrs_region_validate(region, rf, region->max_tx_power_dbm), 0);
		/* A little over the ceiling has to be refused */
		if (region->max_tx_power_dbm < 22) {
			EXPECT_NE(zlrs_region_validate(region, rf,
						       (int8_t)(region->max_tx_power_dbm + 1)),
				  0);
		}
		/* A band mismatch has to be refused: an LF board cannot do 915, an HF one cannot do 470/433 */
		EXPECT_EQ(zlrs_region_validate(
				  region, (region->band == ZLRS_BAND_SUBG_HF) ? &rf_lf : &rf_hf,
				  region->max_tx_power_dbm),
			  -5);
		/* Right band but a design that does not cover the region's edges: also refused */
		{
			const zlrs_rf_design_t narrow = {.band = rf->band,
							 .low_hz = region->high_hz,
							 .high_hz = region->high_hz};

			EXPECT_EQ(zlrs_region_validate(region, &narrow, region->max_tx_power_dbm),
				  -6);
		}
	}

	EXPECT_EQ(zlrs_region_get(ZLRS_REGION_COUNT), nullptr);
	EXPECT_NE(zlrs_region_validate(nullptr, &rf_hf, 0), 0);
	EXPECT_NE(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_FCC915), nullptr, 0), 0);

	/* On the prototype: the HF board accepts FCC915/EU868, the LF board CN470/CN433 */
	EXPECT_EQ(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_FCC915), &rf_hf, 10), 0);
	EXPECT_EQ(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_FCC915), &rf_lf, 10), -5);
	EXPECT_EQ(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_CN470), &rf_lf, 10), 0);
	EXPECT_EQ(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_CN433), &rf_lf, 10), 0);
	EXPECT_EQ(zlrs_region_validate(zlrs_region_get(ZLRS_REGION_CN433), &rf_hf, 10), -5);
}

/* Rate ceiling: narrow bands pull the configured rate down */
TEST(Region, RateCeilings)
{
	printf("region: rate limit\n");

	EXPECT_EQ(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_FCC915), 2600000U), 2600000U);
	EXPECT_EQ(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_FCC915), 1300000U), 1300000U);
	/* EU868, CN470 and CN433 cannot fit the wideband tiers */
	EXPECT_EQ(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_EU868), 1300000U), 325000U);
	EXPECT_EQ(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_CN470), 2600000U), 325000U);
	EXPECT_EQ(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_CN433), 650000U), 325000U);
	/* Anything already below the ceiling stays put */
	EXPECT_EQ(zlrs_region_clamp_rate(zlrs_region_get(ZLRS_REGION_CN433), 325000U), 325000U);
	EXPECT_EQ(zlrs_region_clamp_rate(nullptr, 1300000U), 1300000U);
}

TEST(Region, AirtimeRounding)
{
	printf("region: airtime\n");

	/* 128 bytes at 1.3 Mbps: (128+4)*8 bit = 812.3 us, rounded up to 813 us */
	EXPECT_EQ(zlrs_airtime_us(128U, 1300000U), 813U);
	/* The same frame at 0.325 Mbps is about 3.25 ms */
	EXPECT_EQ(zlrs_airtime_us(128U, 325000U), 3250U);
	/* 128 bytes at 1.3 Mbps: (128+4)*8 bit ~ 812 us, rounded up to 1 ms */
	EXPECT_EQ(zlrs_airtime_ms(128U, 1300000U), 1U);
	/* The same frame at 0.325 Mbps is about 3.25 ms */
	EXPECT_EQ(zlrs_airtime_ms(128U, 325000U), 4U);
	/* No accounting when the rate is unknown */
	EXPECT_EQ(zlrs_airtime_ms(128U, 0U), 0U);
	EXPECT_EQ(zlrs_airtime_us(128U, 0U), 0U);
}

/* Region duty limits maximum DATA frame cadence in constrained bands */
TEST(Region, MaximumCadence)
{
	/* 31-byte data frame at 1.3 Mbps: (31+4)*8/1.3e6 = 215.4 us, rounded up to 216 us */
	printf("region: max frame rate\n");

	EXPECT_EQ(zlrs_airtime_us(31U, 1300000U), 216U);

	/* 1% -> 10000 us/s -> 10000/216 = 46 frames per second */
	EXPECT_EQ(zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_EU868), 1300000U, 31U),
		  46U);
	/* Unlimited-duty regions: as fast as you like */
	EXPECT_EQ(zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_FCC915), 1300000U, 31U),
		  UINT32_MAX);
	EXPECT_EQ(zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_CN470), 1300000U, 31U),
		  UINT32_MAX);
	/* No ceiling when the rate is unknown (the layer above decides) */
	EXPECT_EQ(zlrs_region_max_frames_per_s(zlrs_region_get(ZLRS_REGION_EU868), 0U, 31U),
		  UINT32_MAX);
}

TEST(Region, DutyBudgetAndWindowReset)
{
	const zlrs_region_t *eu = zlrs_region_get(ZLRS_REGION_EU868);
	const zlrs_region_t *fcc = zlrs_region_get(ZLRS_REGION_FCC915);
	zlrs_duty_t duty;
	uint32_t now = 1000U;

	printf("region: duty cycle\n");

	/* EU868: 1% of 60 s = 600 ms of budget */
	zlrs_duty_init(&duty, eu, now);
	EXPECT_TRUE(duty.limited);
	EXPECT_EQ(zlrs_duty_remaining_ms(&duty, now), 600U);

	/* One 4 ms transmission leaves 596 ms */
	EXPECT_TRUE(zlrs_duty_reserve(&duty, 4U, now));
	EXPECT_EQ(zlrs_duty_remaining_ms(&duty, now), 596U);

	/* Spend the rest in 100 ms chunks; the next attempt is refused and counted */
	for (uint8_t i = 0U; i < 5U; i++) {
		EXPECT_TRUE(zlrs_duty_reserve(&duty, 100U, now));
	}
	EXPECT_EQ(zlrs_duty_remaining_ms(&duty, now), 96U);
	EXPECT_FALSE(zlrs_duty_reserve(&duty, 100U, now));
	EXPECT_TRUE(zlrs_duty_reserve(&duty, 96U, now));
	EXPECT_EQ(zlrs_duty_remaining_ms(&duty, now), 0U);
	EXPECT_FALSE(zlrs_duty_reserve(&duty, 1U, now));
	EXPECT_EQ(duty.denied, 2U); /* that 100 ms attempt was refused too */

	/* New window: the budget is back */
	now += 60001U;
	EXPECT_EQ(zlrs_duty_remaining_ms(&duty, now), 600U);
	EXPECT_TRUE(zlrs_duty_reserve(&duty, 4U, now));

	/* FCC915 has no duty limit: transmit freely, no counter moves */
	zlrs_duty_init(&duty, fcc, now);
	EXPECT_FALSE(duty.limited);
	for (uint8_t i = 0U; i < 5U; i++) {
		EXPECT_TRUE(zlrs_duty_reserve(&duty, 4U, now));
	}
	EXPECT_EQ(zlrs_duty_remaining_ms(&duty, now), UINT32_MAX);
	EXPECT_EQ(duty.denied, 0U);
}

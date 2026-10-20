/* SPDX-License-Identifier: Apache-2.0 */
#include <gtest/gtest.h>

extern "C" {
#include "power.h"
}

namespace
{
int8_t observe(zlrs_power_t *power, int16_t rssi, uint8_t lq, uint32_t now)
{
	return zlrs_power_update(power, rssi, lq, now, now);
}

uint32_t healthy(zlrs_power_t *power, uint32_t start, uint32_t duration, uint8_t quality = 100U)
{
	for (uint32_t elapsed = 0U; elapsed <= duration; elapsed += 250U) {
		observe(power, -60, quality, start + elapsed);
	}
	return start + duration;
}

class PowerPolicy: public testing::Test
{
      protected:
	zlrs_power_t power{};

	void SetUp() override
	{
		zlrs_power_init(&power, 12, -17, 0U);
	}
};

TEST_F(PowerPolicy, RequiresContinuousHealthBeforeLowering)
{
	EXPECT_EQ(power.current_dbm, 12);
	EXPECT_EQ(zlrs_power_update(&power, -60, 100U, 0U, 1U), 12);
	healthy(&power, 100001U, 39750U);
	EXPECT_EQ(power.current_dbm, 12);
	EXPECT_EQ(observe(&power, -60, 100U, 140001U), 9);
	EXPECT_EQ(power.lowers, 1U);
}

TEST_F(PowerPolicy, OneLostPacketInAQualityWindowStillAllowsSaving)
{
	healthy(&power, 1U, 39750U, 99U);
	EXPECT_EQ(power.current_dbm, 12);
	EXPECT_EQ(observe(&power, -60, 99U, 40001U), 9);
	EXPECT_EQ(power.lowers, 1U);
	EXPECT_EQ(zlrs_power_update(&power, -60, 99U, 40001U, 41001U), 12);
}

TEST_F(PowerPolicy, TwoPercentLossRestartsSavingQualification)
{
	healthy(&power, 1U, 39750U, 99U);
	EXPECT_EQ(observe(&power, -60, 98U, 40001U), 12);
	healthy(&power, 40251U, 39750U, 99U);
	EXPECT_EQ(power.current_dbm, 12);
	EXPECT_EQ(observe(&power, -60, 99U, 80251U), 9);
}

TEST_F(PowerPolicy, CountsDistinctPoorFeedback)
{
	healthy(&power, 100001U, 40000U);
	ASSERT_EQ(power.current_dbm, 9);
	healthy(&power, 140251U, 1750U);
	EXPECT_EQ(observe(&power, -60, 98U, 142002U), 9);
	for (uint32_t now = 142003U; now < 142050U; now++) {
		EXPECT_EQ(zlrs_power_update(&power, -60, 98U, 142002U, now), 9);
	}
	EXPECT_EQ(observe(&power, -60, 98U, 142050U), 9);
	EXPECT_EQ(observe(&power, -96, 100U, 142051U), 12);
	EXPECT_EQ(power.raises, 1U);
}

TEST_F(PowerPolicy, MarginalReceptionRestartsHealthyQualification)
{
	healthy(&power, 142301U, 39750U);
	EXPECT_EQ(observe(&power, -70, 100U, 182301U), 12);
	healthy(&power, 182551U, 39750U);
	EXPECT_EQ(observe(&power, -95, 99U, 222551U), 12);
	healthy(&power, 222801U, 39750U);
	EXPECT_EQ(power.current_dbm, 12);
	EXPECT_EQ(observe(&power, -69, 100U, 262801U), 9);
}

TEST_F(PowerPolicy, MissingFeedbackRestoresMaximumAtTimeout)
{
	const uint32_t finish = healthy(&power, 1U, 40000U);
	ASSERT_EQ(power.current_dbm, 9);
	EXPECT_EQ(zlrs_power_update(&power, -60, 100U, finish, finish + 999U), 9);
	EXPECT_EQ(zlrs_power_update(&power, -60, 100U, finish, finish + 1000U), 12);
	EXPECT_EQ(power.fallbacks, 1U);
	EXPECT_EQ(zlrs_power_recover(&power, finish + 1001U), 12);
	EXPECT_EQ(power.fallbacks, 1U);
}

TEST_F(PowerPolicy, NewFeedbackAfterUnobservedGapStillRestoresMaximum)
{
	healthy(&power, 300001U, 40000U);
	ASSERT_EQ(power.current_dbm, 9);
	EXPECT_EQ(observe(&power, -60, 100U, 341001U), 12);
	EXPECT_EQ(power.fallbacks, 1U);
}

struct InvalidFeedback {
	int16_t rssi;
	uint8_t quality;
	const char *name;
};

class InvalidPowerFeedback: public PowerPolicy, public testing::WithParamInterface<InvalidFeedback>
{
};

TEST_P(InvalidPowerFeedback, RestoresMaximum)
{
	healthy(&power, 1U, 40000U);
	ASSERT_EQ(power.current_dbm, 9);
	EXPECT_EQ(observe(&power, GetParam().rssi, GetParam().quality, 40251U), 12);
}

INSTANTIATE_TEST_SUITE_P(InvalidInputs, InvalidPowerFeedback,
			 testing::Values(InvalidFeedback{0, 100, "ZeroRssi"},
					 InvalidFeedback{-256, 100, "OutOfRangeRssi"},
					 InvalidFeedback{-60, 101, "ImpossibleQuality"}),
			 [](const testing::TestParamInfo<InvalidFeedback> &info) {
				 return info.param.name;
			 });

TEST_F(PowerPolicy, ExplicitRecoveryBypassesAdjustmentHold)
{
	healthy(&power, 1U, 40000U);
	ASSERT_EQ(power.current_dbm, 9);
	EXPECT_EQ(observe(&power, -255, 100U, 40002U), 9);
	EXPECT_EQ(zlrs_power_recover(&power, 40003U), 12);
}

struct PowerRange {
	int8_t ceiling;
	int8_t floor;
	const char *name;
};

class PowerBounds: public testing::TestWithParam<PowerRange>
{
};

TEST_P(PowerBounds, LowersToFloorAndRecoversWithinLimits)
{
	zlrs_power_t power{};
	const auto range = GetParam();
	zlrs_power_init(&power, range.ceiling, range.floor, 0U);
	uint32_t next = 1U;
	for (unsigned int step = 0; step < 12U; step++) {
		SCOPED_TRACE(step);
		next = healthy(&power, next, 40000U) + 250U;
		EXPECT_GE(power.current_dbm, range.floor);
		EXPECT_LE(power.current_dbm, range.ceiling);
	}
	EXPECT_EQ(power.current_dbm, range.floor);
	EXPECT_EQ(observe(&power, -60, 98U, next), range.floor);
	EXPECT_EQ(observe(&power, -60, 98U, next + 250U), range.floor);
	EXPECT_EQ(observe(&power, -60, 98U, next + 500U), range.floor + 3);
	EXPECT_EQ(zlrs_power_recover(&power, next + 501U), range.ceiling);
}

INSTANTIATE_TEST_SUITE_P(
	HardwareAndRegionalLimits, PowerBounds,
	testing::Values(PowerRange{12, -17, "Ism2400"}, PowerRange{22, -10, "SubgMaximum"},
			PowerRange{14, -10, "Subg14"}, PowerRange{17, -10, "Subg17"},
			PowerRange{10, -10, "Subg10"}),
	[](const testing::TestParamInfo<PowerRange> &info) { return info.param.name; });

TEST_F(PowerPolicy, InvertedRangeCannotLowerPower)
{
	zlrs_power_init(&power, -10, 0, 0U);
	healthy(&power, 1U, 40000U);
	EXPECT_EQ(power.current_dbm, -10);
	EXPECT_EQ(power.lowers, 0U);
}

TEST_F(PowerPolicy, ReductionClampsToFloor)
{
	zlrs_power_init(&power, 1, 0, 0U);
	healthy(&power, 1U, 40000U);
	EXPECT_EQ(power.current_dbm, 0);
}

TEST_F(PowerPolicy, FeedbackTimeoutCrossesClockWrap)
{
	const uint32_t start = UINT32_MAX - 1000U;
	zlrs_power_init(&power, 12, -17, start);
	const uint32_t finish = healthy(&power, start, 40000U);
	ASSERT_EQ(power.current_dbm, 9);
	EXPECT_EQ(zlrs_power_update(&power, -60, 100U, finish, finish + 999U), 9);
	EXPECT_EQ(zlrs_power_update(&power, -60, 100U, finish, finish + 1000U), 12);
}
} // namespace

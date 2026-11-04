/* SPDX-License-Identifier: Apache-2.0 */
#include <gtest/gtest.h>
extern "C" {
#include "power.h"
}

namespace
{
class PowerPolicy: public testing::Test
{
      protected:
	zlrs_power_t p{};
	zlrs_power_observation_t o{};

	void SetUp() override
	{
		zlrs_power_init(&p, 12, -17, 0);
		o.rssi_dbm = -60;
		o.rssi_floor_dbm = -89;
		o.lq_percent = 127;
	}

	int sample(uint32_t now)
	{
		o.now_ms = o.feedback_ms = now;
		return zlrs_power_update(&p, &o);
	}

	void run(uint32_t start, uint32_t duration, uint32_t interval = 250)
	{
		for (uint32_t dt = 0; dt <= duration; dt += interval) {
			sample(start + dt);
		}
	}
};

TEST_F(PowerPolicy, UnknownQualityAllowsSavingAfterSettlingAndTwoSeconds)
{
	run(1, 2250);
	EXPECT_EQ(p.current_dbm, 12);
	EXPECT_EQ(sample(2501), 9);
	EXPECT_EQ(p.lowers, 1U);
}

TEST_F(PowerPolicy, CachedLowQualityDoesNotRepeatedlyRaiseOrResetSaving)
{
	run(1, 2500);
	o.lq_percent = 90;
	o.lq_generation = 1;
	EXPECT_EQ(sample(2751), 12);
	EXPECT_EQ(p.raises, 1U);
	run(3001, 30000);
	EXPECT_EQ(p.current_dbm, -17);
	EXPECT_EQ(p.raises, 1U);
}

TEST_F(PowerPolicy, AmbiguousToggleBaselineIsNotAPoorWindowEvent)
{
	run(1, 10000);
	o.lq_percent = 49;
	o.lq_generation = 2;
	const int before = p.current_dbm;
	EXPECT_EQ(sample(10251), before);
	EXPECT_EQ(p.raises, 0U);
	o.lq_generation++;
	EXPECT_EQ(sample(10501), 12);
}

TEST_F(PowerPolicy, EachNewLowWindowActsOnceEvenWhenValueIsUnchanged)
{
	run(1, 10000);
	int before = p.current_dbm;
	o.lq_percent = 90;
	o.lq_generation = 1;
	EXPECT_EQ(sample(10251), before + 3);
	EXPECT_EQ(sample(10501), before + 3);
	o.lq_generation++;
	EXPECT_EQ(sample(10751), before + 6);
	EXPECT_EQ(p.raises, 2U);
}

TEST_F(PowerPolicy, NinetyEightPercentNeverBlocksRssiSaving)
{
	o.lq_percent = 98;
	for (uint32_t now = 1; now <= 30001; now += 250) {
		o.lq_generation = now / 1250;
		sample(now);
	}
	EXPECT_EQ(p.current_dbm, -17);
	EXPECT_EQ(p.raises, 0U);
}

TEST_F(PowerPolicy, SevereNewQualityRestoresMaximumAndHoldsForFiveSeconds)
{
	run(1, 10000);
	o.lq_percent = 49;
	o.lq_generation = 1;
	EXPECT_EQ(sample(10251), 12);
	run(10501, 4500);
	EXPECT_EQ(p.current_dbm, 12);
	run(15251, 2000);
	EXPECT_EQ(p.current_dbm, 9);
	EXPECT_EQ(p.raises, 1U);
}

TEST_F(PowerPolicy, DuplicateFeedbackCannotAdvanceQualification)
{
	sample(1);
	sample(501);
	for (uint32_t now = 502; now < 1501; now++) {
		o.now_ms = now;
		EXPECT_EQ(zlrs_power_update(&p, &o), 12);
	}
	EXPECT_EQ(p.lowers, 0U);
}

TEST_F(PowerPolicy, FeedbackTimeoutAndUnobservedGapRestoreMaximum)
{
	run(1, 2500);
	ASSERT_EQ(p.current_dbm, 9);
	o.now_ms = o.feedback_ms + 999;
	EXPECT_EQ(zlrs_power_update(&p, &o), 9);
	o.now_ms++;
	EXPECT_EQ(zlrs_power_update(&p, &o), 12);
	EXPECT_EQ(p.fallbacks, 1U);
	run(5001, 2500);
	ASSERT_EQ(p.current_dbm, 9);
	EXPECT_EQ(sample(8501), 12);
	EXPECT_EQ(p.fallbacks, 2U);
}

TEST_F(PowerPolicy, CachedQualityAfterFeedbackGapOnlyEstablishesBaseline)
{
	run(1, 2500);
	o.lq_generation = 1;
	o.lq_percent = 49;
	EXPECT_EQ(sample(3501), 12);
	run(3751, 10000);
	EXPECT_EQ(p.raises, 0U);
	EXPECT_LT(p.current_dbm, 12);
}

TEST_F(PowerPolicy, HysteresisPreventsAdjustmentInsideReceiveMargin)
{
	o.rssi_dbm = -86;
	run(1, 30000);
	EXPECT_EQ(p.current_dbm, 12);
	EXPECT_EQ(p.lowers, 0U);
}

TEST_F(PowerPolicy, WeakRssiRaisesWithoutWaitingForLqWindow)
{
	run(1, 10000);
	ASSERT_LT(p.current_dbm, 12);
	uint32_t raises = p.raises;
	o.rssi_dbm = -100;
	run(10251, 2000);
	EXPECT_GT(p.raises, raises);
}

TEST_F(PowerPolicy, SingleRssiSpikeDoesNotRaise)
{
	run(1, 2500);
	o.rssi_dbm = -110;
	sample(2751);
	o.rssi_dbm = -60;
	run(3001, 2500);
	EXPECT_EQ(p.raises, 0U);
}

TEST_F(PowerPolicy, SlowFrameSettlingDiscardsPreloadedObservations)
{
	o.settle_ms = 1500;
	run(1, 3250);
	EXPECT_EQ(p.current_dbm, 12);
	EXPECT_EQ(sample(3501), 9);
}

TEST_F(PowerPolicy, FiltersByElapsedTimeRatherThanPacketCount)
{
	zlrs_power_t other = p;
	o.rssi_dbm = -85;
	run(1, 1000);
	for (uint32_t now = 1; now <= 1001; now += 250) {
		o.now_ms = o.feedback_ms = now;
		zlrs_power_update(&other, &o);
	}
	o.rssi_dbm = -95;
	for (uint32_t now = 1002; now <= 1251; now++) {
		sample(now);
	}
	o.now_ms = o.feedback_ms = 1251;
	zlrs_power_update(&other, &o);
	EXPECT_NEAR(p.rssi_q8, other.rssi_q8, 256);
	EXPECT_EQ(p.current_dbm, other.current_dbm);
}

TEST_F(PowerPolicy, WraparoundPreservesTimeoutAndAdjustmentIntervals)
{
	const uint32_t start = UINT32_MAX - 1000;
	zlrs_power_init(&p, 12, -17, start);
	run(start, 2500);
	EXPECT_EQ(p.current_dbm, 9);
	o.now_ms = o.feedback_ms + 1000;
	EXPECT_EQ(zlrs_power_update(&p, &o), 12);
}

TEST_F(PowerPolicy, InvalidInputRecoversButUnavailableQualityIsAllowed)
{
	for (int16_t invalid : {0, -256}) {
		SetUp();
		run(1, 2500);
		o.rssi_dbm = invalid;
		EXPECT_EQ(sample(2751), 12);
	}
	SetUp();
	run(1, 2500);
	o.lq_percent = 101;
	EXPECT_EQ(sample(2751), 12);
}

TEST_F(PowerPolicy, EveryHardwareRangeAndFinalPartialStepStayBounded)
{
	for (const auto [ceiling, floor] :
	     {std::pair{12, -17}, {22, -10}, {14, -10}, {17, -10}, {10, -10}, {1, 0}, {-10, 0}}) {
		SetUp();
		zlrs_power_init(&p, ceiling, floor, 0);
		run(1, 35000);
		EXPECT_EQ(p.current_dbm, std::min(ceiling, floor));
		EXPECT_EQ(zlrs_power_recover(&p, 35002), ceiling);
	}
}
} // namespace

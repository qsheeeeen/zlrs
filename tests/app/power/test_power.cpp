/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <initializer_list>
#include <algorithm>
#include <utility>
extern "C" {
#include "power.h"
}

namespace
{

zlrs_power_t p{};
zlrs_power_observation_t o{};

void reset_PowerPolicy()
{
	p = {};
	o = {};
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

ZTEST(PowerPolicy, test_UnknownQualityAllowsSavingAfterSettlingAndTwoSeconds)
{
	reset_PowerPolicy();
	run(1, 2250);
	zexpect_equal(p.current_dbm, 12);
	zexpect_equal(sample(2501), 9);
	zexpect_equal(p.lowers, 1U);
}

ZTEST(PowerPolicy, test_CachedLowQualityDoesNotRepeatedlyRaiseOrResetSaving)
{
	reset_PowerPolicy();
	run(1, 2500);
	o.lq_percent = 90;
	o.lq_generation = 1;
	zexpect_equal(sample(2751), 12);
	zexpect_equal(p.raises, 1U);
	run(3001, 30000);
	zexpect_equal(p.current_dbm, -17);
	zexpect_equal(p.raises, 1U);
}

ZTEST(PowerPolicy, test_AmbiguousToggleBaselineIsNotAPoorWindowEvent)
{
	reset_PowerPolicy();
	run(1, 10000);
	o.lq_percent = 49;
	o.lq_generation = 2;
	const int before = p.current_dbm;
	zexpect_equal(sample(10251), before);
	zexpect_equal(p.raises, 0U);
	o.lq_generation++;
	zexpect_equal(sample(10501), 12);
}

ZTEST(PowerPolicy, test_EachNewLowWindowActsOnceEvenWhenValueIsUnchanged)
{
	reset_PowerPolicy();
	run(1, 10000);
	int before = p.current_dbm;
	o.lq_percent = 90;
	o.lq_generation = 1;
	zexpect_equal(sample(10251), before + 3);
	zexpect_equal(sample(10501), before + 3);
	o.lq_generation++;
	zexpect_equal(sample(10751), before + 6);
	zexpect_equal(p.raises, 2U);
}

ZTEST(PowerPolicy, test_NinetyEightPercentNeverBlocksRssiSaving)
{
	reset_PowerPolicy();
	o.lq_percent = 98;
	for (uint32_t now = 1; now <= 30001; now += 250) {
		o.lq_generation = now / 1250;
		sample(now);
	}
	zexpect_equal(p.current_dbm, -17);
	zexpect_equal(p.raises, 0U);
}

ZTEST(PowerPolicy, test_SevereNewQualityRestoresMaximumAndHoldsForFiveSeconds)
{
	reset_PowerPolicy();
	run(1, 10000);
	o.lq_percent = 49;
	o.lq_generation = 1;
	zexpect_equal(sample(10251), 12);
	run(10501, 4500);
	zexpect_equal(p.current_dbm, 12);
	run(15251, 2000);
	zexpect_equal(p.current_dbm, 9);
	zexpect_equal(p.raises, 1U);
}

ZTEST(PowerPolicy, test_DuplicateFeedbackCannotAdvanceQualification)
{
	reset_PowerPolicy();
	sample(1);
	sample(501);
	for (uint32_t now = 502; now < 1501; now++) {
		o.now_ms = now;
		zexpect_equal(zlrs_power_update(&p, &o), 12);
	}
	zexpect_equal(p.lowers, 0U);
}

ZTEST(PowerPolicy, test_FeedbackTimeoutAndUnobservedGapRestoreMaximum)
{
	reset_PowerPolicy();
	run(1, 2500);
	zassert_equal(p.current_dbm, 9);
	o.now_ms = o.feedback_ms + 999;
	zexpect_equal(zlrs_power_update(&p, &o), 9);
	o.now_ms++;
	zexpect_equal(zlrs_power_update(&p, &o), 12);
	zexpect_equal(p.fallbacks, 1U);
	run(5001, 2500);
	zassert_equal(p.current_dbm, 9);
	zexpect_equal(sample(8501), 12);
	zexpect_equal(p.fallbacks, 2U);
}

ZTEST(PowerPolicy, test_CachedQualityAfterFeedbackGapOnlyEstablishesBaseline)
{
	reset_PowerPolicy();
	run(1, 2500);
	o.lq_generation = 1;
	o.lq_percent = 49;
	zexpect_equal(sample(3501), 12);
	run(3751, 10000);
	zexpect_equal(p.raises, 0U);
	zexpect_true((p.current_dbm) < (12));
}

ZTEST(PowerPolicy, test_HysteresisPreventsAdjustmentInsideReceiveMargin)
{
	reset_PowerPolicy();
	o.rssi_dbm = -86;
	run(1, 30000);
	zexpect_equal(p.current_dbm, 12);
	zexpect_equal(p.lowers, 0U);
}

ZTEST(PowerPolicy, test_WeakRssiRaisesWithoutWaitingForLqWindow)
{
	reset_PowerPolicy();
	run(1, 10000);
	zassert_true((p.current_dbm) < (12));
	uint32_t raises = p.raises;
	o.rssi_dbm = -100;
	run(10251, 2000);
	zexpect_true((p.raises) > (raises));
}

ZTEST(PowerPolicy, test_SingleRssiSpikeDoesNotRaise)
{
	reset_PowerPolicy();
	run(1, 2500);
	o.rssi_dbm = -110;
	sample(2751);
	o.rssi_dbm = -60;
	run(3001, 2500);
	zexpect_equal(p.raises, 0U);
}

ZTEST(PowerPolicy, test_SlowFrameSettlingDiscardsPreloadedObservations)
{
	reset_PowerPolicy();
	o.settle_ms = 1500;
	run(1, 3250);
	zexpect_equal(p.current_dbm, 12);
	zexpect_equal(sample(3501), 9);
}

ZTEST(PowerPolicy, test_FiltersByElapsedTimeRatherThanPacketCount)
{
	reset_PowerPolicy();
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
	zexpect_within(p.rssi_q8, other.rssi_q8, 256);
	zexpect_equal(p.current_dbm, other.current_dbm);
}

ZTEST(PowerPolicy, test_WraparoundPreservesTimeoutAndAdjustmentIntervals)
{
	reset_PowerPolicy();
	const uint32_t start = UINT32_MAX - 1000;
	zlrs_power_init(&p, 12, -17, start);
	run(start, 2500);
	zexpect_equal(p.current_dbm, 9);
	o.now_ms = o.feedback_ms + 1000;
	zexpect_equal(zlrs_power_update(&p, &o), 12);
}

ZTEST(PowerPolicy, test_InvalidInputRecoversButUnavailableQualityIsAllowed)
{
	reset_PowerPolicy();
	for (int16_t invalid : {0, -256}) {
		reset_PowerPolicy();
		run(1, 2500);
		o.rssi_dbm = invalid;
		zexpect_equal(sample(2751), 12);
	}
	reset_PowerPolicy();
	run(1, 2500);
	o.lq_percent = 101;
	zexpect_equal(sample(2751), 12);
}

ZTEST(PowerPolicy, test_EveryHardwareRangeAndFinalPartialStepStayBounded)
{
	reset_PowerPolicy();
	for (const auto &[ceiling, floor] :
	     {std::pair{12, -17}, {22, -10}, {14, -10}, {17, -10}, {10, -10}, {1, 0}, {-10, 0}}) {
		reset_PowerPolicy();
		zlrs_power_init(&p, ceiling, floor, 0);
		run(1, 35000);
		zexpect_equal(p.current_dbm, std::min(ceiling, floor));
		zexpect_equal(zlrs_power_recover(&p, 35002), ceiling);
	}
}
} // namespace

ZTEST_SUITE(PowerPolicy, NULL, NULL, NULL, NULL, NULL);

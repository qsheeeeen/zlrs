/* SPDX-License-Identifier: Apache-2.0 */
#include <gtest/gtest.h>
#include <cstdint>
#include <cstdio>

#include "../../app/src/platform/lr2021.h"
#include "../../app/src/platform/slot_timer.h"
#include <string.h>

TEST(RadioHelpers, PeriodicSyncAcquisition)
{
	const struct {
		uint32_t cadence_us;
		uint16_t sync_frames;
		uint32_t window_us;
	} bands[] = {{11092U, 450U, 4000U}, {48134U, 103U, 20000U}, {22184U, 225U, 9000U}};

	for (size_t band = 0U; band < sizeof(bands) / sizeof(bands[0]); band++) {
		const uint32_t cadence = bands[band].cadence_us;
		const uint32_t receiver_period =
			cadence + zlrs_slot_timer_scan_step_us(cadence, bands[band].sync_frames);
		const uint32_t sync_us = cadence * bands[band].sync_frames;

		/* Try every initial clock offset: a quiet connected Leader only sends periodic
		 * SYNC. The searching Follower must hear one within three such opportunities. */
		for (uint32_t phase = 0U; phase < cadence; phase++) {
			bool heard = false;

			for (uint32_t attempt = 1U; attempt <= 3U; attempt++) {
				const uint32_t offset =
					(attempt * sync_us + phase) % receiver_period;

				heard |= offset < bands[band].window_us;
			}
			if (!heard) {
				printf("SYNC acquisition failed: cadence=%u phase=%u\n", cadence,
				       phase);
				EXPECT_TRUE(heard);
				break;
			}
		}
	}
}

TEST(RadioHelpers, TransmitDeadlineReservesWholePacketAcrossTimerWrap)
{
	const uint32_t targets[] = {1234U, UINT32_MAX - 15U};
	const uint32_t airtimes[] = {100U, 1100U, 40000U};
	const struct { int32_t slack; bool fits; uint32_t allowed; } cases[] = {
		{-1, false, 0U}, {0, true, 0U}, {11, true, 11U},
		{50, true, 50U}, {100, true, 100U}, {500, true, 100U},
	};
	for (uint32_t target : targets) {
		for (uint32_t airtime : airtimes) {
			const uint32_t required = ZLRS_LR2021_TX_COMMAND_US +
				ZLRS_LR2021_STARTUP_US + airtime;
			for (const auto &c : cases) {
				uint32_t latest = 0U;
				const uint32_t end = target + required + c.slack;
				ASSERT_EQ(zlrs_lr2021_tx_deadline(target, end, airtime, &latest), c.fits);
				if (c.fits) {
					EXPECT_EQ(latest - target, c.allowed);
					EXPECT_GE(static_cast<int32_t>(end - latest - required), 0);
				}
			}
		}
	}
}

TEST(RadioHelpers, TemperatureConversions)
{
	/* ADC unit values are signed 13-bit Q8.5, including below-zero temperatures. */
	EXPECT_EQ(zlrs_lr2021_temperature_centi_c(0U), 0);
	EXPECT_EQ(zlrs_lr2021_temperature_centi_c(1U), 3);
	EXPECT_EQ(zlrs_lr2021_temperature_centi_c(800U), 2500);
	EXPECT_EQ(zlrs_lr2021_temperature_centi_c(0x1FFFU), -3);
	EXPECT_EQ(zlrs_lr2021_temperature_centi_c(0x1FE0U), -100);
	EXPECT_EQ(zlrs_lr2021_temperature_centi_c(0x0FFFU), 12796);
	EXPECT_EQ(zlrs_lr2021_temperature_centi_c(0x1000U), -12800);

}

/* SPDX-License-Identifier: Apache-2.0 */
#include "test.h"
#include "power.h"

static int8_t observe(zlrs_power_t *power, int16_t rssi, uint8_t lq, uint32_t now)
{
	return zlrs_power_update(power, rssi, lq, now, now);
}

/* Keep real feedback cadence through the hold, including timer wraparound. */
static uint32_t healthy(zlrs_power_t *power, uint32_t start, uint32_t duration)
{
	for (uint32_t elapsed = 0U; elapsed <= duration; elapsed += 250U) {
		observe(power, -60, 100U, start + elapsed);
	}
	return start + duration;
}

void test_power(void)
{
	printf("power: continuous health, distinct feedback, recovery, bounds and clock wrap\n");
	zlrs_power_t power;
	zlrs_power_init(&power, 12, -17, 0U);
	CHECK(power.current_dbm == 12);
	CHECK(zlrs_power_update(&power, -60, 100U, 0U, 1U) == 12);
	/* Boot/search time cannot qualify a reduction. */
	healthy(&power, 100001U, 39750U);
	CHECK(power.current_dbm == 12);
	CHECK(observe(&power, -60, 100U, 140001U) == 9);
	CHECK(power.lowers == 1U);
	/* Duplicate feedback cannot supply the three poor observations. */
	healthy(&power, 140251U, 1750U);
	CHECK(observe(&power, -60, 98U, 142002U) == 9);
	for (uint32_t now = 142003U; now < 142050U; now++) {
		CHECK(zlrs_power_update(&power, -60, 98U, 142002U, now) == 9);
	}
	CHECK(observe(&power, -60, 98U, 142050U) == 9);
	CHECK(observe(&power, -96, 100U, 142051U) == 12);
	CHECK(power.raises == 1U);
	/* 99% LQ and threshold equality interrupt continuous health. */
	healthy(&power, 142301U, 39750U);
	CHECK(observe(&power, -70, 100U, 182301U) == 12);
	healthy(&power, 182551U, 39750U);
	CHECK(observe(&power, -95, 99U, 222551U) == 12);
	healthy(&power, 222801U, 39750U);
	CHECK(power.current_dbm == 12);
	CHECK(observe(&power, -69, 100U, 262801U) == 9);
	CHECK(zlrs_power_update(&power, -69, 100U, 262801U, 263800U) == 9);
	CHECK(zlrs_power_update(&power, -69, 100U, 262801U, 263801U) == 12);
	CHECK(power.fallbacks == 1U);
	CHECK(zlrs_power_recover(&power, 263802U) == 12);
	CHECK(power.fallbacks == 1U);
	/* A caller that misses a feedback gap must still recover, even with new feedback. */
	healthy(&power, 300001U, 40000U);
	CHECK(power.current_dbm == 9);
	CHECK(observe(&power, -60, 100U, 341001U) == 12);
	CHECK(power.fallbacks == 2U);
	/* RSSI zero/out-of-range and impossible quality never qualify lowering. */
	healthy(&power, 341251U, 40000U);
	CHECK(power.current_dbm == 9);
	CHECK(observe(&power, 0, 100U, 381501U) == 12);
	healthy(&power, 381751U, 40000U);
	CHECK(observe(&power, -256, 100U, 422001U) == 12);
	healthy(&power, 422251U, 40000U);
	CHECK(observe(&power, -60, 101U, 462501U) == 12);
	/* Explicit disconnect bypasses the two-second hold and all step sizes. */
	healthy(&power, 462751U, 40000U);
	CHECK(power.current_dbm == 9);
	CHECK(observe(&power, -255, 100U, 502752U) == 9);
	CHECK(zlrs_power_recover(&power, 502753U) == 12);

	/* Both hardware ranges and every regional ceiling remain bounded. */
	const int8_t ceilings[] = {12, 22, 14, 17, 10};
	for (unsigned int i = 0; i < sizeof(ceilings) / sizeof(ceilings[0]); i++) {
		const int8_t floor = i == 0 ? -17 : -10;
		zlrs_power_init(&power, ceilings[i], floor, 0U);
		uint32_t next = 1U;
		for (unsigned int step = 0; step < 12U; step++) {
			next = healthy(&power, next, 40000U) + 250U;
			CHECK(power.current_dbm >= floor && power.current_dbm <= ceilings[i]);
		}
		CHECK(power.current_dbm == floor);
		/* Poor reception raises by one bounded 3 dB step. */
		CHECK(observe(&power, -60, 98U, next) == floor);
		CHECK(observe(&power, -60, 98U, next + 250U) == floor);
		CHECK(observe(&power, -60, 98U, next + 500U) == floor + 3);
		CHECK(zlrs_power_recover(&power, next + 501U) == ceilings[i]);
	}
	zlrs_power_init(&power, -10, 0, 0U);
	healthy(&power, 1U, 40000U);
	CHECK(power.current_dbm == -10 && power.lowers == 0U);
	zlrs_power_init(&power, 1, 0, 0U);
	healthy(&power, 1U, 40000U);
	CHECK(power.current_dbm == 0);

	const uint32_t start = UINT32_MAX - 1000U;
	zlrs_power_init(&power, 12, -17, start);
	const uint32_t finish = healthy(&power, start, 40000U);
	CHECK(power.current_dbm == 9);
	CHECK(zlrs_power_update(&power, -60, 100U, finish, finish + 999U) == 9);
	CHECK(zlrs_power_update(&power, -60, 100U, finish, finish + 1000U) == 12);
}

/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dynamic power tests: consecutive windows, hold time, ceilings and floors, hysteresis, and the
 */

#include "test.h"

#include "zlrs/link/power.h"

static void test_basic_steps(void)
{
	zlrs_power_ctl_t ctl;
	int8_t out = 0;
	uint32_t now = 1000U;

	printf("power: step up / down\n");

	/* ceiling 20 dBm, floor 0 dBm */
	zlrs_power_init(&ctl, 20, 0, now);
	CHECK(zlrs_power_current_dbm(&ctl) == 20);

	/* Good signal: after 3 consecutive windows drop one step (3 dB) */
	now += 5000U;
	CHECK(!zlrs_power_update(&ctl, true, -60, 100U, now, &out));
	CHECK(!zlrs_power_update(&ctl, true, -60, 100U, now, &out));
	CHECK(zlrs_power_update(&ctl, true, -60, 100U, now, &out));
	CHECK(out == 17);
	CHECK(ctl.lowers == 1U);

	/* Nothing moves during the hold time */
	now += 100U;
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(!zlrs_power_update(&ctl, true, -60, 100U, now, &out));
	}
	CHECK(zlrs_power_current_dbm(&ctl) == 17);

	/* Signal struggling (LQ below 90): after 3 windows raise one step */
	now += 5000U;
	CHECK(!zlrs_power_update(&ctl, true, -100, 60U, now, &out));
	CHECK(!zlrs_power_update(&ctl, true, -100, 60U, now, &out));
	CHECK(zlrs_power_update(&ctl, true, -100, 60U, now, &out));
	CHECK(out == 20);
	CHECK(ctl.raises == 1U);

	/* At the ceiling nothing moves and no counter is bumped */
	now += 5000U;
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(!zlrs_power_update(&ctl, true, -110, 10U, now, &out));
	}
	CHECK(zlrs_power_current_dbm(&ctl) == 20);
	CHECK(ctl.raises == 1U);
}

static void test_floor(void)
{
	zlrs_power_ctl_t ctl;
	int8_t out = 0;
	uint32_t now = 1000U;

	printf("power: floor\n");

	/* Ceiling 5 dBm, floor 0 dBm: never below 0 dBm */
	zlrs_power_init(&ctl, 5, 0, now);

	for (uint8_t step = 0U; step < 4U; step++) {
		now += 5000U;
		(void)zlrs_power_update(&ctl, true, -50, 100U, now, &out);
		(void)zlrs_power_update(&ctl, true, -50, 100U, now, &out);
		(void)zlrs_power_update(&ctl, true, -50, 100U, now, &out);
	}

	CHECK(zlrs_power_current_dbm(&ctl) == 0);

	/* No further drop below the floor */
	now += 5000U;
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(!zlrs_power_update(&ctl, true, -50, 100U, now, &out));
	}
	CHECK(zlrs_power_current_dbm(&ctl) == 0);
}

static void test_hysteresis(void)
{
	zlrs_power_ctl_t ctl;
	int8_t out = 0;
	uint32_t now = 1000U;

	printf("power: hysteresis\n");

	zlrs_power_init(&ctl, 20, 0, now);

	/* Middle ground (-80 dBm, LQ 95%): neither raise nor drop */
	now += 5000U;
	for (uint8_t i = 0U; i < 8U; i++) {
		CHECK(!zlrs_power_update(&ctl, true, -80, 95U, now, &out));
	}
	CHECK(zlrs_power_current_dbm(&ctl) == 20);

	/* RSSI high but LQ not: no drop (both conditions have to hold) */
	for (uint8_t i = 0U; i < 8U; i++) {
		CHECK(!zlrs_power_update(&ctl, true, -50, 96U, now, &out));
	}
	CHECK(zlrs_power_current_dbm(&ctl) == 20);
}

static void test_telemetry_loss(void)
{
	zlrs_power_ctl_t ctl;
	int8_t out = 0;
	uint32_t now = 1000U;

	printf("power: telemetry loss\n");

	zlrs_power_init(&ctl, 20, 0, now);

	/* Drop to the floor first */
	for (uint8_t step = 0U; step < 8U; step++) {
		now += 5000U;
		(void)zlrs_power_update(&ctl, true, -50, 100U, now, &out);
		(void)zlrs_power_update(&ctl, true, -50, 100U, now, &out);
		(void)zlrs_power_update(&ctl, true, -50, 100U, now, &out);
	}
	CHECK(zlrs_power_current_dbm(&ctl) == 0);

	/* Telemetry stops: the next poll (past 1 s) goes straight back to the ceiling and counts it */
	now += 1500U;
	CHECK(zlrs_power_update(&ctl, false, 0, 0U, now, &out));
	CHECK(out == 20);
	CHECK(ctl.fallbacks == 1U);

	/* Already at the ceiling: no repeated report */
	CHECK(!zlrs_power_update(&ctl, false, 0, 0U, now, &out));

	/* No timeout before the first telemetry (the first second after power-up) */
	{
		zlrs_power_ctl_t fresh;
		int8_t fresh_out = 0;

		zlrs_power_init(&fresh, 20, 0, now);
		now += 5000U;
		CHECK(!zlrs_power_update(&fresh, false, 0, 0U, now, &fresh_out));
		CHECK(fresh.fallbacks == 0U);
	}
}

void test_power(void)
{
	test_basic_steps();
	test_floor();
	test_hysteresis();
	test_telemetry_loss();
}

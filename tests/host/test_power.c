/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 动态功率测试：连续窗口、保持时间、上下限、滞回，以及"遥测断了先回上限"。
 */

#include "test.h"

#include "zlrs/link/power.h"

static void test_basic_steps(void)
{
	zlrs_power_ctl_t ctl;
	int8_t out = 0;
	uint32_t now = 1000U;

	printf("power: step up / down\n");

	/* 配置上限 20 dBm，最低 0 dBm */
	zlrs_power_init(&ctl, 20, 0, now);
	CHECK(zlrs_power_current_dbm(&ctl) == 20);

	/* 信号好：连续 3 个窗口后降一档（步长 3 dB） */
	now += 5000U;
	CHECK(!zlrs_power_update(&ctl, true, -60, 100U, now, &out));
	CHECK(!zlrs_power_update(&ctl, true, -60, 100U, now, &out));
	CHECK(zlrs_power_update(&ctl, true, -60, 100U, now, &out));
	CHECK(out == 17);
	CHECK(ctl.lowers == 1U);

	/* 保持时间内不动 */
	now += 100U;
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(!zlrs_power_update(&ctl, true, -60, 100U, now, &out));
	}
	CHECK(zlrs_power_current_dbm(&ctl) == 17);

	/* 信号吃力（LQ 掉到 90 以下）：连续 3 个窗口后升一档 */
	now += 5000U;
	CHECK(!zlrs_power_update(&ctl, true, -100, 60U, now, &out));
	CHECK(!zlrs_power_update(&ctl, true, -100, 60U, now, &out));
	CHECK(zlrs_power_update(&ctl, true, -100, 60U, now, &out));
	CHECK(out == 20);
	CHECK(ctl.raises == 1U);

	/* 到上限就不再动，也不刷计数 */
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

	/* 上限 5 dBm、下限 0 dBm：最多降到 0 dBm */
	zlrs_power_init(&ctl, 5, 0, now);

	for (uint8_t step = 0U; step < 4U; step++) {
		now += 5000U;
		(void)zlrs_power_update(&ctl, true, -50, 100U, now, &out);
		(void)zlrs_power_update(&ctl, true, -50, 100U, now, &out);
		(void)zlrs_power_update(&ctl, true, -50, 100U, now, &out);
	}

	CHECK(zlrs_power_current_dbm(&ctl) == 0);

	/* 下限以下不再降 */
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

	/* 中间地带（-80 dBm，LQ 95%）：既不升也不降 */
	now += 5000U;
	for (uint8_t i = 0U; i < 8U; i++) {
		CHECK(!zlrs_power_update(&ctl, true, -80, 95U, now, &out));
	}
	CHECK(zlrs_power_current_dbm(&ctl) == 20);

	/* RSSI 够高但 LQ 不够高：也不降（两条都要满足） */
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

	/* 先降到最低 */
	for (uint8_t step = 0U; step < 8U; step++) {
		now += 5000U;
		(void)zlrs_power_update(&ctl, true, -50, 100U, now, &out);
		(void)zlrs_power_update(&ctl, true, -50, 100U, now, &out);
		(void)zlrs_power_update(&ctl, true, -50, 100U, now, &out);
	}
	CHECK(zlrs_power_current_dbm(&ctl) == 0);

	/* 遥测停了：下一次轮询（超过 1 秒）直接回到上限并计数 */
	now += 1500U;
	CHECK(zlrs_power_update(&ctl, false, 0, 0U, now, &out));
	CHECK(out == 20);
	CHECK(ctl.fallbacks == 1U);

	/* 已经在上限：不再重复上报 */
	CHECK(!zlrs_power_update(&ctl, false, 0, 0U, now, &out));

	/* 还没收到过遥测时不该触发超时（上电头 1 秒内） */
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

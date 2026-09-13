/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 状态灯测试：优先级、每个状态的节奏（亮/灭窗口）、LQ 亮度分档、
 * 以及"只用颜色区分是不可靠的"——每个状态同时有节奏差异。
 */

#include "test.h"

#include <string.h>

#include "zlrs/led/led.h"

static bool is_off(zlrs_led_rgb_t c)
{
	return c.r == 0U && c.g == 0U && c.b == 0U;
}

static void test_config_mode_wins(void)
{
	zlrs_led_input_t in;

	printf("led: priority\n");

	memset(&in, 0, sizeof(in));
	in.config_mode = true;
	in.failsafe = true; /* 这两条同时成立时，灯只表现配置模式 */
	in.link_state = ZLRS_STATE_SEARCHING;

	/* 白，2 秒周期里亮 200 ms */
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 500U)));
	CHECK(!is_off(zlrs_led_frame(&in, 2000U)));

	/* 关掉配置模式：失控保护接管（红，5 Hz 快闪） */
	in.config_mode = false;
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 100U))); /* 亮 60 ms、灭 140 ms */
	CHECK(!is_off(zlrs_led_frame(&in, 200U)));
}

static void test_link_states(void)
{
	zlrs_led_input_t in;

	printf("led: link states\n");

	memset(&in, 0, sizeof(in));
	in.link_state = ZLRS_STATE_UNCONFIGURED;
	/* 紫 / 黄都是 1 Hz：t=0 亮，t=500 灭 */
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 500U)));
	in.link_state = ZLRS_STATE_UNPAIRED;
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 500U)));

	/* 配对中：4 Hz，亮 80 ms */
	in.link_state = ZLRS_STATE_PAIRING;
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 150U)));
	CHECK(!is_off(zlrs_led_frame(&in, 250U)));

	/* 搜索中：2 Hz，亮 80 ms */
	in.link_state = ZLRS_STATE_SEARCHING;
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 200U)));
	CHECK(!is_off(zlrs_led_frame(&in, 500U)));
}

static void test_connected_lq(void)
{
	zlrs_led_input_t in;
	zlrs_led_rgb_t strong;
	zlrs_led_rgb_t weak;
	zlrs_led_rgb_t bad;
	const zlrs_led_rgb_t green30 = { 0U, 76U, 0U };
	const zlrs_led_rgb_t green15 = { 0U, 38U, 0U };

	printf("led: connected and lq\n");

	memset(&in, 0, sizeof(in));
	in.configured = true;
	in.paired = true;
	in.link_state = ZLRS_STATE_CONNECTED;

	/* 常亮：不同时刻颜色一样 */
	in.lq_percent = 90U;
	strong = zlrs_led_frame(&in, 0U);
	CHECK(!is_off(strong));
	CHECK(memcmp(&strong, &green30, sizeof(strong)) == 0); /* 绿 255 × 30% */
	{
		const zlrs_led_rgb_t later = zlrs_led_frame(&in, 777U);

		CHECK(memcmp(&strong, &later, sizeof(strong)) == 0);
	}

	/* LQ 掉档：亮度降一档 */
	in.lq_percent = 60U;
	weak = zlrs_led_frame(&in, 0U);
	CHECK(memcmp(&weak, &green15, sizeof(weak)) == 0); /* 15% */

	/* LQ 很差：转琥珀色（不是暗绿） */
	in.lq_percent = 20U;
	bad = zlrs_led_frame(&in, 0U);
	CHECK(bad.r > bad.g);
}

static void test_drop_overlay(void)
{
	zlrs_led_input_t in;
	zlrs_led_rgb_t base;
	zlrs_led_rgb_t flashed;
	const zlrs_led_rgb_t green30 = { 0U, 76U, 0U };

	printf("led: drop overlay\n");

	memset(&in, 0, sizeof(in));
	in.configured = true;
	in.paired = true;
	in.link_state = ZLRS_STATE_CONNECTED;
	in.lq_percent = 90U;
	in.drop_recent = true;

	base = zlrs_led_frame(&in, 500U);
	flashed = zlrs_led_frame(&in, 50U); /* 每 2 秒单闪 120 ms */

	CHECK(!is_off(flashed));
	CHECK(flashed.r > base.r);
	CHECK(memcmp(&base, &green30, sizeof(base)) == 0);

	/* 没有丢弃时不会闪 */
	in.drop_recent = false;
	{
		const zlrs_led_rgb_t quiet = zlrs_led_frame(&in, 50U);

		CHECK(memcmp(&quiet, &base, sizeof(base)) == 0);
	}

	/* 未连接时不叠加：t=50 时搜索态是亮的青色，而不是丢弃提示的橙色 */
	in.link_state = ZLRS_STATE_SEARCHING;
	in.drop_recent = true;
	{
		const zlrs_led_rgb_t searching = zlrs_led_frame(&in, 50U);

		CHECK(searching.b > 0U);
		CHECK(searching.r == 0U);
	}
}

void test_led(void)
{
	test_config_mode_wins();
	test_link_states();
	test_connected_lq();
	test_drop_overlay();
}

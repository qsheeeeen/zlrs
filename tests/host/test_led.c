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
	in.uptime_ms = 10000U;
	in.config_mode = true;
	in.failsafe = true; /* 这两条同时成立时，灯只表现配置模式 */
	in.link_state = ZLRS_STATE_SEARCHING;

	/* 白，1 秒周期里亮 500 ms */
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 500U)));
	CHECK(!is_off(zlrs_led_frame(&in, 2000U)));

	/* 关掉配置模式：失控保护接管（红，1 Hz、亮 800 ms——最长的亮窗=最紧急） */
	in.config_mode = false;
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(!is_off(zlrs_led_frame(&in, 500U)));
	CHECK(is_off(zlrs_led_frame(&in, 900U)));
	CHECK(!is_off(zlrs_led_frame(&in, 1000U)));
}

static void test_link_states(void)
{
	zlrs_led_input_t in;

	printf("led: link states\n");

	memset(&in, 0, sizeof(in));
	in.uptime_ms = 10000U;
	in.link_state = ZLRS_STATE_UNCONFIGURED;
	/* 紫 0.5 Hz、黄 1 Hz：两者 t=0 亮、t=500 灭 */
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 500U)));
	in.link_state = ZLRS_STATE_UNPAIRED;
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 500U)));

	/* 配对中：1 Hz，亮 300 ms */
	in.link_state = ZLRS_STATE_PAIRING;
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 500U)));
	CHECK(!is_off(zlrs_led_frame(&in, 1000U)));

	/* 搜索中：0.5 Hz，亮 150 ms */
	in.link_state = ZLRS_STATE_SEARCHING;
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 200U)));
	CHECK(!is_off(zlrs_led_frame(&in, 2000U)));
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
	in.uptime_ms = 10000U;
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
	in.uptime_ms = 10000U;
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

/* 开机自检：前 1.5 秒亮角色色，之后进入状态显示 */
static void test_boot_role(void)
{
	zlrs_led_input_t in;

	printf("led: boot role\n");

	memset(&in, 0, sizeof(in));
	in.role_tx = true;
	in.link_state = ZLRS_STATE_UNPAIRED;
	in.uptime_ms = 0U;

	/* TX：蓝 */
	{
		const zlrs_led_rgb_t tx = zlrs_led_frame(&in, 0U);

		CHECK(tx.b > tx.r && tx.b > tx.g);
	}
	/* RX：绿 */
	in.role_tx = false;
	{
		const zlrs_led_rgb_t rx = zlrs_led_frame(&in, 0U);

		CHECK(rx.g > rx.r && rx.g > rx.b);
	}
	/* 自检窗口结束：回到"未配对"的黄色 1 Hz 慢闪 */
	in.uptime_ms = 2000U;
	{
		const zlrs_led_rgb_t later = zlrs_led_frame(&in, 0U);

		CHECK(later.r > 0U && later.b == 0U);
	}
}

void test_led(void)
{
	test_boot_role();
	test_config_mode_wins();
	test_link_states();
	test_connected_lq();
	test_drop_overlay();
}

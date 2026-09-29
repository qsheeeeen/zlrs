/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Status LED tests: priorities, the cadence of every state (on/off windows), LQ brightness
 * steps, and the rule that colour alone is unreliable: every state also differs in cadence.
 */

#include "test.h"

#include <string.h>

#include "zlrs/led/led.h"

static bool is_off(zlrs_led_rgb_t c)
{
	return c.r == 0U && c.g == 0U && c.b == 0U;
}

static void test_mgmt_mode_wins(void)
{
	zlrs_led_input_t in;

	printf("led: priority\n");

	memset(&in, 0, sizeof(in));
	in.uptime_ms = 10000U;
	in.mgmt_mode = true;
	in.failsafe = true; /* with both set, the LED only shows management mode */
	in.link_state = ZLRS_STATE_SEARCHING;

	/* White, on for 500 ms out of a 1 s period */
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 500U)));
	CHECK(!is_off(zlrs_led_frame(&in, 2000U)));

	/* Management mode off: failsafe takes over (red, 1 Hz, 800 ms on: the longest window is the alert) */
	in.mgmt_mode = false;
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
	/* Purple at 0.5 Hz, yellow at 1 Hz: both are on at t=0 and off at t=500 */
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 500U)));
	in.link_state = ZLRS_STATE_UNPAIRED;
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 500U)));

	/* Pairing: 1 Hz, 300 ms on */
	in.link_state = ZLRS_STATE_PAIRING;
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 500U)));
	CHECK(!is_off(zlrs_led_frame(&in, 1000U)));

	/* Searching: 0.5 Hz, 150 ms on */
	in.link_state = ZLRS_STATE_SEARCHING;
	CHECK(!is_off(zlrs_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_led_frame(&in, 200U)));
	CHECK(!is_off(zlrs_led_frame(&in, 2000U)));
}

static void test_failsafe_while_connected(void)
{
	zlrs_led_input_t in;

	printf("led: failsafe wins over connected\n");

	/* TX connected but with no handset input: red wins over green */
	memset(&in, 0, sizeof(in));
	in.uptime_ms = 10000U;
	in.link_state = ZLRS_STATE_CONNECTED;
	in.lq_percent = 100U;
	in.failsafe = true;
	{
		const zlrs_led_rgb_t f = zlrs_led_frame(&in, 0U);

		CHECK(f.r > f.g);
		CHECK(f.r > 0U);
	}

	/* Never connected means no failsafe from the caller: unpaired shows yellow (no blue) */
	memset(&in, 0, sizeof(in));
	in.uptime_ms = 10000U;
	in.link_state = ZLRS_STATE_UNPAIRED;
	{
		const zlrs_led_rgb_t f = zlrs_led_frame(&in, 0U);

		CHECK(f.r > 0U && f.g > 0U && f.b == 0U);
	}
}

static void test_connected_lq(void)
{
	zlrs_led_input_t in;
	zlrs_led_rgb_t strong;
	const zlrs_led_rgb_t rx_green30 = {0U, 76U, 0U};
	const zlrs_led_rgb_t tx_blue30 = {0U, 27U, 76U};

	printf("led: connected and lq\n");

	memset(&in, 0, sizeof(in));
	in.uptime_ms = 10000U;
	in.configured = true;
	in.paired = true;
	in.link_state = ZLRS_STATE_CONNECTED;

	/* RX (green theme): LQ changes cadence, never brightness */
	in.lq_percent = 90U;
	strong = zlrs_led_frame(&in, 0U);
	CHECK(!is_off(strong));
	CHECK(memcmp(&strong, &rx_green30, sizeof(strong)) == 0); /* green 255 x 30% */
	{
		const zlrs_led_rgb_t later = zlrs_led_frame(&in, 200U);

		CHECK(is_off(later));
		const zlrs_led_rgb_t next = zlrs_led_frame(&in, 500U);

		CHECK(memcmp(&strong, &next, sizeof(strong)) == 0);
	}

	/* Medium LQ: one 200 ms pulse per second, at the same brightness. */
	in.lq_percent = 60U;
	{
		const zlrs_led_rgb_t medium_on = zlrs_led_frame(&in, 0U);
		const zlrs_led_rgb_t medium_next = zlrs_led_frame(&in, 1000U);

		CHECK(memcmp(&medium_on, &rx_green30, sizeof(medium_on)) == 0);
		CHECK(is_off(zlrs_led_frame(&in, 300U)));
		CHECK(memcmp(&medium_on, &medium_next, sizeof(medium_on)) == 0);
	}

	/* Low LQ: one 200 ms pulse every two seconds, still at the same brightness. */
	in.lq_percent = 20U;
	{
		const zlrs_led_rgb_t low_on = zlrs_led_frame(&in, 0U);
		const zlrs_led_rgb_t low_next = zlrs_led_frame(&in, 2000U);

		CHECK(memcmp(&low_on, &rx_green30, sizeof(low_on)) == 0);
		CHECK(is_off(zlrs_led_frame(&in, 300U)));
		CHECK(memcmp(&low_on, &low_next, sizeof(low_on)) == 0);
	}

	/* The TX theme is blue: the same link state comes out in blue */
	in.role_tx = true;
	{
		const zlrs_led_rgb_t tx_on = zlrs_led_frame(&in, 0U);

		CHECK(memcmp(&tx_on, &tx_blue30, sizeof(tx_on)) == 0);
	}
}

static void test_drop_overlay(void)
{
	zlrs_led_input_t in;
	zlrs_led_rgb_t base;
	zlrs_led_rgb_t flashed;

	printf("led: drop overlay\n");

	memset(&in, 0, sizeof(in));
	in.uptime_ms = 10000U;
	in.configured = true;
	in.paired = true;
	in.link_state = ZLRS_STATE_CONNECTED;
	in.lq_percent = 90U;
	in.drop_recent = true;

	base = zlrs_led_frame(&in, 400U);
	flashed = zlrs_led_frame(&in, 50U); /* one 300 ms flash every 2 s */

	CHECK(!is_off(flashed));
	CHECK(flashed.r > base.r);
	CHECK(is_off(base));

	/* No drops means no flash */
	in.drop_recent = false;
	{
		const zlrs_led_rgb_t quiet = zlrs_led_frame(&in, 50U);

		CHECK(quiet.r == 0U && quiet.g > 0U && quiet.b == 0U);
	}

	/* No overlay while unconnected: at t=50 it is the searching theme colour, not drop orange */
	in.role_tx = true;
	in.link_state = ZLRS_STATE_SEARCHING;
	in.drop_recent = true;
	{
		const zlrs_led_rgb_t searching = zlrs_led_frame(&in, 50U);

		CHECK(searching.b > 0U);
		CHECK(searching.r == 0U);
	}
}

/* Boot self-test: the role colour for 1.5 s, then the state display takes over */
static void test_boot_role(void)
{
	zlrs_led_input_t in;

	printf("led: boot role\n");

	memset(&in, 0, sizeof(in));
	in.role_tx = true;
	in.link_state = ZLRS_STATE_UNPAIRED;
	in.uptime_ms = 0U;

	/* TX: blue */
	{
		const zlrs_led_rgb_t tx = zlrs_led_frame(&in, 0U);

		CHECK(tx.b > tx.r && tx.b > tx.g);
	}
	/* RX: green */
	in.role_tx = false;
	{
		const zlrs_led_rgb_t rx = zlrs_led_frame(&in, 0U);

		CHECK(rx.g > rx.r && rx.g > rx.b);
	}
	/* Self-test window over: back to the unpaired yellow blink */
	in.uptime_ms = 2000U;
	{
		const zlrs_led_rgb_t later = zlrs_led_frame(&in, 0U);

		CHECK(later.r > 0U && later.b == 0U);
	}
}

void test_led(void)
{
	test_boot_role();
	test_mgmt_mode_wins();
	test_link_states();
	test_failsafe_while_connected();
	test_connected_lq();
	test_drop_overlay();
}

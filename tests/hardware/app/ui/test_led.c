/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Status LED tests: priorities, color alternation, connected cadence and LQ brightness.
 */

#include <zephyr/ztest.h>

#include <string.h>

#include "ui_pattern.h"

#define CHECK(condition) zassert_true((condition), #condition)

static bool is_off(struct zlrs_led_color c)
{
	return c.r == 0U && c.g == 0U && c.b == 0U;
}

ZTEST(led_patterns, test_mgmt_mode_wins)
{
	struct zlrs_led_frame_input in;

	memset(&in, 0, sizeof(in));
	in.mgmt_mode = true;
	in.link_lost = true; /* with both set, the LED only shows management mode */
	in.link_state = ZLRS_STATE_SEARCHING;

	/* Management mode is steady white and outranks link loss. */
	{
		const struct zlrs_led_color white = {255U, 255U, 255U};
		const struct zlrs_led_color at_start = zlrs_app_led_frame(&in, 0U);
		const struct zlrs_led_color later = zlrs_app_led_frame(&in, 500U);

		CHECK(memcmp(&at_start, &white, sizeof(white)) == 0);
		CHECK(memcmp(&later, &white, sizeof(white)) == 0);
	}

	/* Management mode off: link loss alternates theme and red at 2 Hz. */
	in.mgmt_mode = false;
	{
		const struct zlrs_led_color theme = {0U, 255U, 0U};
		const struct zlrs_led_color red = {255U, 0U, 0U};
		const struct zlrs_led_color at_start = zlrs_app_led_frame(&in, 0U);
		const struct zlrs_led_color red_phase = zlrs_app_led_frame(&in, 250U);
		const struct zlrs_led_color next_cycle = zlrs_app_led_frame(&in, 500U);

		CHECK(memcmp(&at_start, &theme, sizeof(theme)) == 0);
		CHECK(memcmp(&red_phase, &red, sizeof(red)) == 0);
		CHECK(memcmp(&next_cycle, &theme, sizeof(theme)) == 0);
	}
}

ZTEST(led_patterns, test_link_states)
{
	struct zlrs_led_frame_input in;

	memset(&in, 0, sizeof(in));
	in.link_state = ZLRS_STATE_UNCONFIGURED;
	/* No root key keeps its purple 0.5 Hz pulse. */
	CHECK(!is_off(zlrs_app_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_app_led_frame(&in, 500U)));
	in.link_state = ZLRS_STATE_UNPAIRED;
	{
		const struct zlrs_led_color theme = {0U, 255U, 0U};
		const struct zlrs_led_color yellow = {255U, 180U, 0U};
		const struct zlrs_led_color at_start = zlrs_app_led_frame(&in, 0U);
		const struct zlrs_led_color yellow_phase = zlrs_app_led_frame(&in, 250U);
		const struct zlrs_led_color next_cycle = zlrs_app_led_frame(&in, 500U);

		CHECK(memcmp(&at_start, &theme, sizeof(theme)) == 0);
		CHECK(memcmp(&yellow_phase, &yellow, sizeof(yellow)) == 0);
		CHECK(memcmp(&next_cycle, &theme, sizeof(theme)) == 0);
	}

	/* Pairing keeps its theme pulse. */
	in.link_state = ZLRS_STATE_PAIRING;
	CHECK(!is_off(zlrs_app_led_frame(&in, 0U)));
	CHECK(is_off(zlrs_app_led_frame(&in, 500U)));
	CHECK(!is_off(zlrs_app_led_frame(&in, 1000U)));

	/* Searching is steady theme colour. */
	in.link_state = ZLRS_STATE_SEARCHING;
	CHECK(!is_off(zlrs_app_led_frame(&in, 0U)));
	CHECK(!is_off(zlrs_app_led_frame(&in, 200U)));
	CHECK(!is_off(zlrs_app_led_frame(&in, 2000U)));
}

ZTEST(led_patterns, test_link_lost_while_connected)
{
	struct zlrs_led_frame_input in;

	/* A connected unit in link loss alternates its RX theme and red. */
	memset(&in, 0, sizeof(in));
	in.link_state = ZLRS_STATE_CONNECTED;
	in.lq_percent = 100U;
	in.link_lost = true;
	{
		const struct zlrs_led_color f = zlrs_app_led_frame(&in, 0U);

		CHECK(f.r == 0U && f.g > 0U && f.b == 0U);
		{
			const struct zlrs_led_color other = zlrs_app_led_frame(&in, 250U);

			CHECK(other.r > 0U && other.g == 0U && other.b == 0U);
		}
	}

	/* Never connected means no link loss from the caller: unpaired shows yellow (no blue) */
	memset(&in, 0, sizeof(in));
	in.link_state = ZLRS_STATE_UNPAIRED;
	{
		const struct zlrs_led_color f = zlrs_app_led_frame(&in, 250U);

		CHECK(f.r > 0U && f.g > 0U && f.b == 0U);
	}
}

ZTEST(led_patterns, test_connected_lq)
{
	struct zlrs_led_frame_input in;
	struct zlrs_led_color strong;
	const struct zlrs_led_color rx_green = {0U, 255U, 0U};
	const struct zlrs_led_color tx_blue = {0U, 90U, 255U};

	memset(&in, 0, sizeof(in));
	in.link_state = ZLRS_STATE_CONNECTED;

	/* RX (green theme): LQ changes cadence and duty, never brightness */
	in.lq_percent = 90U;
	strong = zlrs_app_led_frame(&in, 0U);
	CHECK(!is_off(strong));
	CHECK(memcmp(&strong, &rx_green, sizeof(strong)) == 0);
	{
		const struct zlrs_led_color later = zlrs_app_led_frame(&in, 200U);

		CHECK(is_off(later));
		const struct zlrs_led_color next = zlrs_app_led_frame(&in, 250U);

		CHECK(memcmp(&strong, &next, sizeof(strong)) == 0);
	}

	/* Medium LQ: short 4 Hz pulse, at the same brightness. */
	in.lq_percent = 60U;
	{
		const struct zlrs_led_color medium_on = zlrs_app_led_frame(&in, 0U);
		const struct zlrs_led_color medium_next = zlrs_app_led_frame(&in, 250U);

		CHECK(memcmp(&medium_on, &rx_green, sizeof(medium_on)) == 0);
		CHECK(is_off(zlrs_app_led_frame(&in, 50U)));
		CHECK(memcmp(&medium_on, &medium_next, sizeof(medium_on)) == 0);
	}

	/* Low LQ: half-on 2 Hz pulse, still at the same brightness. */
	in.lq_percent = 20U;
	{
		const struct zlrs_led_color low_on = zlrs_app_led_frame(&in, 0U);
		const struct zlrs_led_color low_next = zlrs_app_led_frame(&in, 500U);

		CHECK(memcmp(&low_on, &rx_green, sizeof(low_on)) == 0);
		CHECK(is_off(zlrs_app_led_frame(&in, 250U)));
		CHECK(memcmp(&low_on, &low_next, sizeof(low_on)) == 0);
	}

	/* The TX theme is blue: the same link state comes out in blue */
	in.role_tx = true;
	{
		const struct zlrs_led_color tx_on = zlrs_app_led_frame(&in, 0U);

		CHECK(memcmp(&tx_on, &tx_blue, sizeof(tx_on)) == 0);
	}
}

ZTEST(led_patterns, test_data_drops)
{
	struct zlrs_led_frame_input in;
	struct zlrs_led_color base;
	struct zlrs_led_color flashed;

	memset(&in, 0, sizeof(in));
	in.link_state = ZLRS_STATE_CONNECTED;
	in.lq_percent = 90U;
	in.drop_recent = true;

	base = zlrs_app_led_frame(&in, 250U);
	flashed = zlrs_app_led_frame(&in, 0U); /* theme/orange alternation, 250 ms per colour */

	CHECK(!is_off(flashed));
	CHECK(flashed.r == 0U && flashed.g > 0U);
	CHECK(base.r > 0U && base.g > 0U);

	/* No drops means no flash */
	in.drop_recent = false;
	{
		const struct zlrs_led_color quiet = zlrs_app_led_frame(&in, 50U);

		CHECK(quiet.r == 0U && quiet.g > 0U && quiet.b == 0U);
	}

	/* A recent drop uses the same theme/orange indication while searching. */
	in.role_tx = true;
	in.link_state = ZLRS_STATE_SEARCHING;
	in.drop_recent = true;
	{
		const struct zlrs_led_color searching = zlrs_app_led_frame(&in, 250U);

		CHECK(searching.r > 0U && searching.g > 0U);
		CHECK(searching.b == 0U);
	}
}

ZTEST_SUITE(led_patterns, NULL, NULL, NULL, NULL, NULL);

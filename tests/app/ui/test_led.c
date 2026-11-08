/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the real UI thread through its zbus inputs and LED driver output. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include "rf.h"
#include "mgmt.h"
#include "serial.h"

ZBUS_CHAN_DEFINE(zlrs_rf_chan, zlrs_rf_status_t, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(zlrs_mode_chan, struct zlrs_mode, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(zlrs_serial_chan, struct zlrs_serial_status, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

struct sample {
	struct led_rgb color;
	uint32_t ms;
};
static struct k_spinlock capture_lock;
static struct sample samples[64];
static size_t sample_count;
static struct led_rgb last;
static uint32_t writes;
static bool reject_next;
static zlrs_rf_status_t link;
static struct zlrs_mode mode;
static struct zlrs_serial_status serial;
/* Hardware forwards captured output to the real pixel; native_sim has no strip alias. */
static const struct device *const physical_strip = DEVICE_DT_GET_OR_NULL(DT_ALIAS(led_strip));

static int record_rgb(const struct device *dev, struct led_rgb *pixels, size_t count)
{
	ARG_UNUSED(dev);
	if (count != 1U) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&capture_lock);
	writes++;
	if (reject_next) {
		reject_next = false;
		k_spin_unlock(&capture_lock, key);
		return -EIO;
	}
	k_spin_unlock(&capture_lock, key);
	if (physical_strip != NULL) {
		if (!device_is_ready(physical_strip)) {
			return -ENODEV;
		}
		const int rc = led_strip_update_rgb(physical_strip, pixels, count);
		if (rc != 0) {
			return rc;
		}
	}
	key = k_spin_lock(&capture_lock);
	last = pixels[0];
	if (sample_count < ARRAY_SIZE(samples)) {
		samples[sample_count++] = (struct sample){.color = last, .ms = k_uptime_get_32()};
	}
	k_spin_unlock(&capture_lock, key);
	return 0;
}

static size_t strip_length(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 1U;
}

static DEVICE_API(led_strip, led_api) = {.update_rgb = record_rgb, .length = strip_length};
DEVICE_DT_DEFINE(DT_NODELABEL(led_test), NULL, NULL, NULL, NULL, POST_KERNEL,
		 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &led_api);

static const struct led_rgb off = {0};
static const struct led_rgb white = {.r = 255U, .g = 255U, .b = 255U};
static const struct led_rgb purple = {.r = 160U, .b = 200U};
static const struct led_rgb yellow = {.r = 255U, .g = 180U};
static const struct led_rgb green = {.g = 255U};
static const struct led_rgb blue = {.g = 90U, .b = 255U};
static const struct led_rgb red = {.r = 255U};
static const struct led_rgb orange = {.r = 255U, .g = 80U};

static bool same_color(struct led_rgb a, struct led_rgb b)
{
	return a.r == b.r && a.g == b.g && a.b == b.b;
}

static struct led_rgb color_now(void)
{
	const k_spinlock_key_t key = k_spin_lock(&capture_lock);
	const struct led_rgb color = last;
	k_spin_unlock(&capture_lock, key);
	return color;
}

static uint32_t write_count(void)
{
	const k_spinlock_key_t key = k_spin_lock(&capture_lock);
	const uint32_t count = writes;
	k_spin_unlock(&capture_lock, key);
	return count;
}

static void clear_capture(void)
{
	const k_spinlock_key_t key = k_spin_lock(&capture_lock);
	sample_count = 0U;
	k_spin_unlock(&capture_lock, key);
}

static void publish(void)
{
	zassert_ok(zbus_chan_pub(&zlrs_rf_chan, &link, K_MSEC(20)));
	zassert_ok(zbus_chan_pub(&zlrs_mode_chan, &mode, K_MSEC(20)));
	zassert_ok(zbus_chan_pub(&zlrs_serial_chan, &serial, K_MSEC(20)));
	k_msleep(80);
}

static void wait_color(struct led_rgb expected, uint32_t timeout_ms)
{
	const int64_t deadline = k_uptime_get() + timeout_ms;
	while (!same_color(color_now(), expected) && k_uptime_get() < deadline) {
		k_msleep(10);
	}
	zassert_true(same_color(color_now(), expected), "LED did not reach the expected color");
}

static void expect_steady(struct led_rgb expected, uint32_t duration_ms)
{
	wait_color(expected, 100);
	clear_capture();
	k_msleep(duration_ms);
	const k_spinlock_key_t key = k_spin_lock(&capture_lock);
	bool steady = same_color(last, expected);
	for (size_t i = 0U; i < sample_count; i++) {
		steady &= same_color(samples[i].color, expected);
	}
	k_spin_unlock(&capture_lock, key);
	zassert_true(steady, "steady indication changed");
}

static void expect_pattern(struct led_rgb first, struct led_rgb second, uint32_t period_ms,
			   uint32_t first_ms, uint32_t duration_ms)
{
	clear_capture();
	k_msleep(duration_ms);
	struct sample captured[ARRAY_SIZE(samples)];
	const k_spinlock_key_t key = k_spin_lock(&capture_lock);
	const size_t count = sample_count;
	memcpy(captured, samples, count * sizeof(*samples));
	k_spin_unlock(&capture_lock, key);
	bool seen_first = false, seen_second = false;
	for (size_t i = 0U; i < count; i++) {
		const uint32_t phase = captured[i].ms % period_ms;
		/* The driver records after rendering, possibly across one clock tick. */
		const uint32_t prior_phase =
			(phase + period_ms - k_ticks_to_ms_ceil32(1U)) % period_ms;
		const bool is_first = same_color(captured[i].color, first);
		const bool is_second = same_color(captured[i].color, second);
		zassert_true((phase < first_ms ? is_first : is_second) ||
				     (prior_phase < first_ms ? is_first : is_second),
			     "wrong RGB at phase %u/%u", phase, period_ms);
		seen_first |= is_first;
		seen_second |= is_second;
	}
	if (!seen_first || !seen_second) {
		for (size_t i = 0U; i < count; i++) {
			printk("LED %u ms: RGB %u/%u/%u\n", captured[i].ms, captured[i].color.r,
			       captured[i].color.g, captured[i].color.b);
		}
	}
	zassert_true(seen_first && seen_second,
		     "both phases must reach the driver: period %u, on %u, writes %zu", period_ms,
		     first_ms, count);
}

static void expect_connected(struct led_rgb theme, uint8_t flashes, uint32_t beat_ms)
{
	clear_capture();
	k_msleep(9U * beat_ms + 100U);
	struct sample captured[ARRAY_SIZE(samples)];
	const k_spinlock_key_t key = k_spin_lock(&capture_lock);
	const size_t count = sample_count;
	memcpy(captured, samples, count * sizeof(*samples));
	k_spin_unlock(&capture_lock, key);
	uint32_t last_on = 0U;
	uint32_t on_min = UINT32_MAX, on_max = 0U;
	uint32_t gap_min = UINT32_MAX, gap_max = 0U;
	unsigned int rises = 0U, widths = 0U;
	bool was_on = false;
	int previous_gap = -1;
	for (size_t i = 0U; i < count; i++) {
		const bool is_on = same_color(captured[i].color, theme);
		zassert_true(is_on || same_color(captured[i].color, off));
		if (is_on) {
			if (rises != 0U) {
				const uint32_t gap = captured[i].ms - last_on;
				const uint32_t short_gap = flashes == 1U ? 3U * beat_ms : beat_ms;
				const uint32_t long_gap = flashes == 2U ? 2U * beat_ms : short_gap;
				const bool short_ok = abs((int32_t)gap - (int32_t)short_gap) <= 25;
				const bool long_ok = abs((int32_t)gap - (int32_t)long_gap) <= 25;
				zassert_true(short_ok || long_ok, "unexpected rise interval %u",
					     gap);
				if (flashes == 2U) {
					const int kind = short_ok ? 0 : 1;
					zassert_not_equal(
						kind, previous_gap,
						"two flashes must alternate short/long intervals");
					previous_gap = kind;
				}
				gap_min = MIN(gap_min, gap);
				gap_max = MAX(gap_max, gap);
			}
			last_on = captured[i].ms;
			rises++;
		} else if (was_on) {
			const uint32_t width = captured[i].ms - last_on;
			zassert_true(abs((int32_t)width - (int32_t)(beat_ms / 2U)) <= 25,
				     "pulse width %u", width);
			on_min = MIN(on_min, width);
			on_max = MAX(on_max, width);
			widths++;
		}
		was_on = is_on;
	}
	zassert_true(rises >= 2U * flashes && widths >= 2U, "not enough pulses: rises=%u widths=%u",
		     rises, widths);
	printk("CONNECTED role=%u mode=%u band=%u LQ=%u flashes=%u on_ms=%u..%u rise_ms=%u..%u\n",
	       link.role, link.band_mode, link.data_band, link.lq_percent, flashes, on_min, on_max,
	       gap_min, gap_max);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	link = (zlrs_rf_status_t){.role = ZLRS_ROLE_FOLLOWER,
				  .state = ZLRS_STATE_CONNECTED,
				  .data_band = ZLRS_DATA_BAND_24,
				  .band_mode = ZLRS_DATA_BAND_24,
				  .lq_percent = 100U,
				  .frame_period_us = 9080U};
	mode = (struct zlrs_mode){0};
	serial = (struct zlrs_serial_status){.ready = true};
	publish();
}

ZTEST(led_patterns, test_mgmt_mode_wins)
{
	link.state = ZLRS_STATE_SEARCHING;
	link.link_lost = true;
	serial.dropped = 1U;
	mode.session = true;
	publish();
	zassert_false(same_color(color_now(), white), "short management session flashed white");
	wait_color(white, 700);
	expect_steady(white, 600);
	mode.session = false;
	publish();
	expect_pattern(green, red, 500U, 250U, 1080U);
}

ZTEST(led_patterns, test_link_states)
{
	link.state = ZLRS_STATE_UNCONFIGURED;
	publish();
	expect_pattern(purple, off, 2000U, 200U, 4080U);
	link.state = ZLRS_STATE_UNPAIRED;
	publish();
	expect_pattern(green, yellow, 500U, 250U, 1080U);
	link.state = ZLRS_STATE_PAIRING;
	publish();
	expect_pattern(green, off, 1000U, 300U, 2080U);
	link.state = ZLRS_STATE_SEARCHING;
	publish();
	expect_steady(green, 200);
	link.role = ZLRS_ROLE_LEADER;
	publish();
	expect_steady(blue, 200);
}

ZTEST(led_patterns, test_link_lost_while_connected)
{
	link.link_lost = true;
	publish();
	expect_pattern(green, red, 500U, 250U, 1080U);
	link.band_mode = ZLRS_DATA_BAND_AUTO;
	link.data_band = ZLRS_DATA_BAND_SUBG;
	link.band_acquiring = true;
	publish();
	expect_pattern(green, red, 500U, 250U, 1080U);
	link.link_lost = false;
	link.fault = true;
	publish();
	expect_pattern(green, red, 500U, 250U, 1080U);
}

ZTEST(led_patterns, test_connected_lq)
{
	struct lq_case {
		uint8_t quality;
		uint8_t flashes;
	};
	static const struct lq_case full_range[] = {{100U, 3U}, {80U, 3U}, {79U, 2U},
						    {50U, 2U},  {49U, 1U}, {0U, 1U}};
	static const struct lq_case auto_24[] = {{100U, 3U}, {99U, 3U}, {98U, 2U},
						 {95U, 2U},  {94U, 1U}, {0U, 1U}};

	static const struct {
		zlrs_data_band_t mode;
		zlrs_data_band_t band;
		uint32_t beat_ms;
		const struct lq_case *cases;
		size_t count;
	} scenarios[] = {
		{ZLRS_DATA_BAND_24, ZLRS_DATA_BAND_24, 300U, full_range, ARRAY_SIZE(full_range)},
		{ZLRS_DATA_BAND_SUBG, ZLRS_DATA_BAND_SUBG, 600U, full_range,
		 ARRAY_SIZE(full_range)},
		{ZLRS_DATA_BAND_AUTO, ZLRS_DATA_BAND_24, 300U, auto_24, ARRAY_SIZE(auto_24)},
		{ZLRS_DATA_BAND_AUTO, ZLRS_DATA_BAND_SUBG, 600U, full_range,
		 ARRAY_SIZE(full_range)},
	};

	printk("Connected cadence output: %s\n",
	       physical_strip != NULL ? physical_strip->name : "capture only");
	for (size_t i = 0U; i < ARRAY_SIZE(scenarios); i++) {
		link.band_mode = scenarios[i].mode;
		link.data_band = scenarios[i].band;
		link.role = ZLRS_ROLE_FOLLOWER;
		for (size_t j = 0U; j < scenarios[i].count; j++) {
			link.lq_percent = scenarios[i].cases[j].quality;
			publish();
			expect_connected(green, scenarios[i].cases[j].flashes,
					 scenarios[i].beat_ms);
		}
		link.role = ZLRS_ROLE_LEADER;
		link.lq_percent = 100U;
		publish();
		expect_connected(blue, 3U, scenarios[i].beat_ms);
	}
}

ZTEST(led_patterns, test_connected_band_changes)
{
	link.band_mode = ZLRS_DATA_BAND_AUTO;
	link.lq_percent = 97U;
	publish();
	expect_connected(green, 2U, 300U);
	/* The active band changes the cadence and LQ scale even with unchanged LQ. */
	link.data_band = ZLRS_DATA_BAND_SUBG;
	publish();
	expect_connected(green, 3U, 600U);
	link.data_band = ZLRS_DATA_BAND_24;
	publish();
	expect_connected(green, 2U, 300U);
}

ZTEST(led_patterns, test_data_drops)
{
	link.state = ZLRS_STATE_SEARCHING;
	serial.dropped++;
	publish();
	expect_pattern(green, orange, 500U, 250U, 600U);
	k_msleep(500);
	expect_steady(green, 200);
	link.role = ZLRS_ROLE_LEADER;
	link.data_drop_bytes++;
	publish();
	expect_pattern(blue, orange, 500U, 250U, 600U);
}

ZTEST(led_patterns, test_write_retry)
{
	link.state = ZLRS_STATE_SEARCHING;
	publish();
	expect_steady(green, 100);
	const uint32_t count = write_count();
	const k_spinlock_key_t key = k_spin_lock(&capture_lock);
	reject_next = true;
	k_spin_unlock(&capture_lock, key);
	link.role = ZLRS_ROLE_LEADER;
	publish();
	wait_color(blue, 200);
	zassert_equal(write_count(), count + 2U, "failed output must be retried");
	k_msleep(200);
	zassert_equal(write_count(), count + 2U, "unchanged RGB must not be written again");
}

ZTEST_SUITE(led_patterns, NULL, NULL, before, NULL, NULL);

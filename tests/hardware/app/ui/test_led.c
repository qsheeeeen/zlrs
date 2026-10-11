/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the real UI thread through its zbus inputs and LED driver output. */
#include <errno.h>
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

static int record_rgb(const struct device *dev, struct led_rgb *pixels, size_t count)
{
	ARG_UNUSED(dev);
	if (count != 1U) {
		return -EINVAL;
	}
	const k_spinlock_key_t key = k_spin_lock(&capture_lock);
	writes++;
	if (reject_next) {
		reject_next = false;
		k_spin_unlock(&capture_lock, key);
		return -EIO;
	}
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

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	link = (zlrs_rf_status_t){.role = ZLRS_ROLE_RX,
				  .state = ZLRS_STATE_CONNECTED,
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
	link.role = ZLRS_ROLE_TX;
	publish();
	expect_steady(blue, 200);
}

ZTEST(led_patterns, test_link_lost_while_connected)
{
	link.link_lost = true;
	publish();
	expect_pattern(green, red, 500U, 250U, 1080U);
	link.link_lost = false;
	link.fault = true;
	publish();
	expect_pattern(green, red, 500U, 250U, 1080U);
}

ZTEST(led_patterns, test_connected_lq)
{
	link.lq_percent = 90U;
	publish();
	expect_pattern(green, off, 250U, 200U, 830U);
	link.lq_percent = 60U;
	publish();
	expect_pattern(green, off, 250U, 50U, 830U);
	link.lq_percent = 20U;
	publish();
	expect_pattern(green, off, 500U, 250U, 1080U);
	link.role = ZLRS_ROLE_TX;
	link.lq_percent = 90U;
	publish();
	expect_pattern(blue, off, 250U, 200U, 830U);
}

ZTEST(led_patterns, test_data_drops)
{
	link.state = ZLRS_STATE_SEARCHING;
	serial.dropped++;
	publish();
	expect_pattern(green, orange, 500U, 250U, 600U);
	k_msleep(500);
	expect_steady(green, 200);
	link.role = ZLRS_ROLE_TX;
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
	link.role = ZLRS_ROLE_TX;
	publish();
	wait_color(blue, 200);
	zassert_equal(write_count(), count + 2U, "failed output must be retried");
	k_msleep(200);
	zassert_equal(write_count(), count + 2U, "unchanged RGB must not be written again");
}

ZTEST_SUITE(led_patterns, NULL, NULL, before, NULL, NULL);

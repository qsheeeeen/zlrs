/*
 * Status LED: consume system state, choose the display pattern and write it to the board's pixel.
 * All LED behaviour belongs to the application; the producers publish facts, not colours.
 *
 * There is deliberately no per-LED-kind code here: the strip is whatever devicetree instantiates
 * behind Zephyr's led_strip API — WS2812 over PIO, SPI or UART, APA102, … — and a board without one
 * has no device at all. The thread then keeps draining the bus and simply has nothing to show
 * (doc/10-ux.md).
 *
 * Why a thread at all: the strip write is the slowest step of the LED path and the main loop is the
 * superframe. The thread is preemptive, so it only runs while the link thread is blocked, and the
 * strip ends up with exactly one writer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <string.h>

#include "rf/rf.h"
#include "mgmt/mode.h"
#include "data/data.h"
#include "ui/led.h"

LOG_MODULE_REGISTER(zlrs_led, CONFIG_APP_LOG_LEVEL);

/* 20 Hz looks continuous to the eye and costs nothing on the strip (doc/10-ux.md) */
#define LED_REFRESH_MS 50U

/* Debug images attenuate the complete frame so the bench pixel is not glaring. */
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
#define LED_OUTPUT_BRIGHTNESS_PERCENT CONFIG_ZLRS_APP_DEBUG_LED_BRIGHTNESS_PERCENT
#else
#define LED_OUTPUT_BRIGHTNESS_PERCENT 100U
#endif

/*
 * A management session is only worth showing once it survives a tool round-trip: the update tool
 * and `zlrs-mgmt.py` open a session per command and leave within ~100 ms, and flashing white for
 * that looks like a fault.
 */
#define LED_CONFIG_DELAY_MS 500U

/* The drop hint is an overlay on the connected colour, so it has to end on its own */
#define LED_DROP_HOLD_MS 1000U

#define LED_PIXELS 1U

/*
 * The link and RAC threads are cooperative (doc/06-architecture.md 6), so this one is preemptive:
 * it runs only while the link thread is blocked, and the link thread preempts it again the moment
 * it wakes. A strip write therefore never stretches a slot.
 */
#define LED_THREAD_PRIORITY   K_PRIO_PREEMPT(7)
#define LED_THREAD_STACK_SIZE 2048U
#define LED_THREAD_NAME       "zlrs_led"

/*
 * Notification queue depth. Broadcasts are change-driven, so a burst is a few messages at most and
 * the thread drains them on its next pass; zbus logs an error for every notification that does not
 * fit, which is why the depth has headroom rather than the minimum.
 */
#define LED_SUB_QUEUE_SIZE 8U

ZBUS_SUBSCRIBER_DEFINE(zlrs_led_sub, LED_SUB_QUEUE_SIZE);

/* The LED thread is the consumer of the system state; the channels themselves have no observers */
ZBUS_CHAN_ADD_OBS(zlrs_link_chan, zlrs_led_sub, 0);
ZBUS_CHAN_ADD_OBS(zlrs_alert_chan, zlrs_led_sub, 0);
ZBUS_CHAN_ADD_OBS(zlrs_mode_chan, zlrs_led_sub, 0);
ZBUS_CHAN_ADD_OBS(zlrs_data_chan, zlrs_led_sub, 0);

/* The board's status pixel: NULL when this board has no `zlrs-led` alias or the node is off */
static const struct device *const led_strip = DEVICE_DT_GET_OR_NULL(DT_ALIAS(zlrs_led));

/*
 * Show one frame. The renderer produces one colour (LED_PIXELS); everything about turning it
 * into a signal — colour order, pixel count, reset timing — belongs to the driver behind the strip
 * API and comes from devicetree.
 */
static int led_write(struct zlrs_led_color color)
{
	static bool error_logged;
	static struct led_rgb pixels[LED_PIXELS];

	if (!device_is_ready(led_strip)) {
		return -1; /* no status LED on this board */
	}

	pixels[0].r = (uint8_t)(((uint16_t)color.r * LED_OUTPUT_BRIGHTNESS_PERCENT) / 100U);
	pixels[0].g = (uint8_t)(((uint16_t)color.g * LED_OUTPUT_BRIGHTNESS_PERCENT) / 100U);
	pixels[0].b = (uint8_t)(((uint16_t)color.b * LED_OUTPUT_BRIGHTNESS_PERCENT) / 100U);

	/*
	 * A failed strip write has to be reported: the LED is the only local feedback, and when the
	 * driver really breaks, "the LED is dark" looks like "the state never happened" while
	 * everything keeps running. The caller retries, so report the first failure only.
	 */
	if (led_strip_update_rgb(led_strip, pixels, LED_PIXELS) != 0) {
		if (!error_logged) {
			error_logged = true;
			LOG_ERR("led strip update failed (%s); status led is stale",
				led_strip->name);
		}

		return -1;
	}

	return 0;
}

static void led_thread(void *p1, void *p2, void *p3)
{
	const struct zbus_channel *chan;
	struct zlrs_link_status link = {0};
	struct zlrs_alerts alerts = {0};
	struct zlrs_mode mode = {0};
	struct zlrs_data_status data = {0};
	uint32_t drop_bytes = 0U;
	uint32_t drop_ms = 0U;
	uint32_t mgmt_since = 0U;
	bool mgmt_was = false;
	bool have_state = false;
	bool write_ok = false;
	struct zlrs_led_color on_strip = {0};

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_thread_name_set(k_current_get(), LED_THREAD_NAME);

	if (!device_is_ready(led_strip)) {
		LOG_WRN("no status led on this board: the state is only reported, not shown");
	}

	for (;;) {
		/* A state change wakes the thread at once; the timeout keeps pattern phases moving. */
		if (zbus_sub_wait(&zlrs_led_sub, &chan, K_MSEC(LED_REFRESH_MS)) == 0) {
			have_state = true;
		}

		/* The strip stays dark until the first broadcast: the channels' initial values are
		 * placeholders, not something the application asked for. An alarm published before the
		 * first frame (a fault during init, for one) lights the LED here too — it must not wait
		 * for the superframe. */
		if (!have_state) {
			continue;
		}

		/* Only the wake-up comes from the bus: the channels hold the newest values, and reading
		 * all three keeps a missed wake from leaving a stale mixture on the LED. */
		(void)zbus_chan_read(&zlrs_link_chan, &link, K_NO_WAIT);
		(void)zbus_chan_read(&zlrs_alert_chan, &alerts, K_NO_WAIT);
		(void)zbus_chan_read(&zlrs_mode_chan, &mode, K_NO_WAIT);
		(void)zbus_chan_read(&zlrs_data_chan, &data, K_NO_WAIT);

		const uint32_t now_ms = k_uptime_get_32();

		/* A drop is a change in the cumulative counter; how long to show it is our decision */
		if (alerts.data_drop_bytes + data.dropped != drop_bytes) {
			drop_bytes = alerts.data_drop_bytes + data.dropped;
			drop_ms = now_ms;
		}

		/* White marks a session that is really being configured, not a tool round-trip */
		if (mode.session != mgmt_was) {
			mgmt_was = mode.session;
			mgmt_since = now_ms;
		}

		/* Faults and failsafe share the red/theme pattern. Management mode wins over alarms,
		 * followed by recent drops and the underlying link state. */
		const bool alarm = alerts.failsafe || alerts.fault;
		const struct zlrs_led_frame_input in = {
			.mgmt_mode =
				mgmt_was && (uint32_t)(now_ms - mgmt_since) >= LED_CONFIG_DELAY_MS,
			.failsafe = alarm,
			.drop_recent =
				drop_bytes > 0U && (uint32_t)(now_ms - drop_ms) < LED_DROP_HOLD_MS,
			.role_tx = link.role_tx,
			.link_state = link.state,
			.lq_percent = link.lq_percent,
		};

		const struct zlrs_led_color color = zlrs_app_led_frame(&in, now_ms);

		/* The strip holds the colour it was given, so an unchanged frame is not worth a PIO
		 * push; a write that failed is retried, or the strip would stay stale for good. */
		if (!write_ok || memcmp(&color, &on_strip, sizeof(color)) != 0) {
			on_strip = color;
			write_ok = led_write(color) == 0;
		}
	}
}

K_THREAD_DEFINE(zlrs_led_tid, LED_THREAD_STACK_SIZE, led_thread, NULL, NULL, NULL,
		LED_THREAD_PRIORITY, 0, 0);

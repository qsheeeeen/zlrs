/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <string.h>

#include "rf/rf.h"
#include "mgmt/mode.h"
#include "data/data.h"
#include "ui_pattern.h"
#include "ui.h"

LOG_MODULE_REGISTER(zlrs_ui, CONFIG_APP_LOG_LEVEL);

ZBUS_CHAN_DEFINE(zlrs_input_chan, struct zlrs_input, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

#define UI_POLL_MS 20U
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_button))
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(zlrs_button), gpios);
#endif

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
 * The RF owner is cooperative (doc/06-architecture.md), so this one is preemptive:
 * it runs only while the link thread is blocked, and the link thread preempts it again the moment
 * it wakes. A strip write therefore never stretches a slot.
 */
#define UI_THREAD_PRIORITY   K_PRIO_PREEMPT(7)
#define UI_THREAD_STACK_SIZE 2048U
#define UI_THREAD_NAME       "zlrs_ui"

/*
 * Notification queue depth. Broadcasts are change-driven, so a burst is a few messages at most and
 * the thread drains them on its next pass; zbus logs an error for every notification that does not
 * fit, which is why the depth has headroom rather than the minimum.
 */
#define UI_SUB_QUEUE_SIZE 8U

ZBUS_SUBSCRIBER_DEFINE(zlrs_ui_sub, UI_SUB_QUEUE_SIZE);

/* The LED thread is the consumer of the system state; the channels themselves have no observers */
ZBUS_CHAN_ADD_OBS(zlrs_rf_chan, zlrs_ui_sub, 0);
ZBUS_CHAN_ADD_OBS(zlrs_mode_chan, zlrs_ui_sub, 0);
ZBUS_CHAN_ADD_OBS(zlrs_data_chan, zlrs_ui_sub, 0);

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

static void ui_thread(void *p1, void *p2, void *p3)
{
	const struct zbus_channel *chan;
	zlrs_rf_status_t link = {0};
	struct zlrs_mode mode = {0};
	struct zlrs_data_status data = {0};
	uint32_t drop_bytes = 0U;
	uint32_t drop_ms = 0U;
	uint32_t mgmt_since = 0U;
	bool mgmt_was = false;
	struct zlrs_input input = {0};
	bool button_ready = false;
	bool down = false;
	bool fired = false;
	bool input_pending = false;
	uint32_t down_since = 0U;
	uint32_t sample_ms = 0U;
	uint32_t render_ms = 0U;
	bool write_ok = false;
	struct zlrs_led_color on_strip = {0};

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_thread_name_set(k_current_get(), UI_THREAD_NAME);

	if (!device_is_ready(led_strip)) {
		LOG_WRN("no status led on this board: the state is only reported, not shown");
	}

#if DT_NODE_EXISTS(DT_ALIAS(zlrs_button))
	button_ready = gpio_is_ready_dt(&button) && gpio_pin_configure_dt(&button, GPIO_INPUT) == 0;
#endif
	for (;;) {
		bool changed = zbus_sub_wait(&zlrs_ui_sub, &chan, K_MSEC(UI_POLL_MS)) == 0;
		while (zbus_sub_wait(&zlrs_ui_sub, &chan, K_NO_WAIT) == 0) {
			changed = true;
		}
		const uint32_t now_ms = k_uptime_get_32();
		if (button_ready && (uint32_t)(now_ms - sample_ms) >= UI_POLL_MS) {
			sample_ms = now_ms;
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_button))
			const int pressed = gpio_pin_get_dt(&button);
			if (pressed > 0 && !down) {
				down = true;
				fired = false;
				down_since = now_ms;
			} else if (pressed > 0 && !fired &&
				   (uint32_t)(now_ms - down_since) >=
					   CONFIG_ZLRS_APP_PAIR_HOLD_MS) {
				fired = true;
				input.button_holds++;
				input_pending = true;
			} else if (pressed == 0) {
				down = false;
			}
#endif
		}
		if (input_pending && zbus_chan_pub(&zlrs_input_chan, &input, K_NO_WAIT) == 0) {
			input_pending = false;
		}
		if (!changed && (uint32_t)(now_ms - render_ms) < LED_REFRESH_MS) {
			continue;
		}
		render_ms = now_ms;
		(void)zbus_chan_read(&zlrs_rf_chan, &link, K_NO_WAIT);
		(void)zbus_chan_read(&zlrs_mode_chan, &mode, K_NO_WAIT);
		(void)zbus_chan_read(&zlrs_data_chan, &data, K_NO_WAIT);
		/* Polling also recovers a missed initial notification. */
		if (link.frame_period_us == 0U && !link.fault) {
			continue;
		}

		/* A drop is a change in the cumulative counter; how long to show it is our decision */
		if (link.data_drop_bytes + data.dropped != drop_bytes) {
			drop_bytes = link.data_drop_bytes + data.dropped;
			drop_ms = now_ms;
		}

		/* White marks a session that is really being configured, not a tool round-trip */
		if (mode.session != mgmt_was) {
			mgmt_was = mode.session;
			mgmt_since = now_ms;
		}

		/* Faults and link loss share the red/theme pattern. Management mode wins over alarms,
		 * followed by recent drops and the underlying link state. */
		const bool alarm = link.link_lost || link.fault;
		const struct zlrs_led_frame_input in = {
			.mgmt_mode =
				mgmt_was && (uint32_t)(now_ms - mgmt_since) >= LED_CONFIG_DELAY_MS,
			.link_lost = alarm,
			.drop_recent =
				drop_bytes > 0U && (uint32_t)(now_ms - drop_ms) < LED_DROP_HOLD_MS,
			.role_tx = link.role == ZLRS_ROLE_TX,
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

K_THREAD_DEFINE(zlrs_ui_tid, UI_THREAD_STACK_SIZE, ui_thread, NULL, NULL, NULL, UI_THREAD_PRIORITY,
		0, 0);

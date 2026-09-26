/*
 * Status LED thread: the consumer of the system state that renders it.
 *
 * The application publishes facts (app/src/sys_bus.h); *this* file decides what they look like and
 * turns them into the abstract frame the port writes. The priority table and the colours are
 * core/led's (pure C, host-tested in tests/host/test_led.c) and the thread is their only caller,
 * so nothing a producer touches names a colour, and the port (app/src/led_port.c) only knows how
 * to put a colour on the device. The timings the state does not carry live here too: the
 * config-mode delay that keeps a tool round-trip from flashing white, the one-second drop overlay,
 * and the boot self-test window.
 *
 * Why a thread at all (doc/11-ux.md): the strip write — a PIO push plus the WS2812 latch delay —
 * is the slowest step of the LED path, and the main loop is the superframe. The thread is
 * preemptive, so it only runs while the link thread is blocked, and the strip ends up with
 * exactly one writer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <string.h>

#include "led_port.h"
#include "sys_bus.h"
#include "zlrs/led/led.h"

LOG_MODULE_REGISTER(zlrs_led_thread, CONFIG_APP_LOG_LEVEL);

/* 20 Hz looks continuous to the eye and costs nothing on the strip (doc/11-ux.md) */
#define LED_REFRESH_MS 50U

/*
 * A config session is only worth showing once it survives a tool round-trip: the update tool and
 * `zlrs-config.py` open a session per command and leave within ~100 ms, and flashing white for
 * that looks like a fault.
 */
#define LED_CONFIG_DELAY_MS 500U

/* The drop hint is an overlay on the connected colour, so it has to end on its own */
#define LED_DROP_HOLD_MS 1000U

/*
 * The link and RAC threads are cooperative (doc/07-architecture.md 6), so this one is preemptive:
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
#define LED_SUB_QUEUE_SIZE 4U

ZBUS_SUBSCRIBER_DEFINE(zlrs_led_sub, LED_SUB_QUEUE_SIZE);

/* The LED thread is the consumer of the system state; the channels themselves have no observers */
ZBUS_CHAN_ADD_OBS(zlrs_link_chan, zlrs_led_sub, 0);
ZBUS_CHAN_ADD_OBS(zlrs_alert_chan, zlrs_led_sub, 0);
ZBUS_CHAN_ADD_OBS(zlrs_mode_chan, zlrs_led_sub, 0);

static void led_thread(void *p1, void *p2, void *p3)
{
	const struct zbus_channel *chan;
	struct zlrs_link_status link = {0};
	struct zlrs_alerts alerts = {0};
	struct zlrs_mode mode = {0};
	uint32_t drop_bytes = 0U;
	uint32_t drop_ms = 0U;
	uint32_t config_since = 0U;
	bool config_was = false;
	bool have_state = false;
	bool write_ok = false;
	zlrs_led_rgb_t on_strip = {0};

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_thread_name_set(k_current_get(), LED_THREAD_NAME);

	/* This thread owns the strip: the device lookup happens here (app/src/led_port.c) */
	(void)zlrs_led_port_init();

	for (;;) {
		/* A state change wakes the thread at once; the timeout keeps the blink phase and the
		 * boot self-test window moving while nothing changes. */
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

		const uint32_t now_ms = k_uptime_get_32();

		/* A drop is a change in the cumulative counter; how long to show it is our decision */
		if (alerts.data_drop_bytes != drop_bytes) {
			drop_bytes = alerts.data_drop_bytes;
			drop_ms = now_ms;
		}

		/* White marks a session that is really being configured, not a tool round-trip */
		if (mode.config != config_was) {
			config_was = mode.config;
			config_since = now_ms;
		}

		/*
		 * An alarm outranks the boot self-test: it has to show at once rather than after the
		 * role-colour window, so the engine is asked past that window (doc/11-ux.md). The
		 * engine shows both the failsafe and the fault as the red "no usable link" colour.
		 */
		const bool alarm = alerts.failsafe || alerts.fault;
		const zlrs_led_input_t in = {
			.config_mode = config_was &&
				       (uint32_t)(now_ms - config_since) >= LED_CONFIG_DELAY_MS,
			.configured = link.configured,
			.paired = link.paired,
			.failsafe = alarm,
			.drop_recent = drop_bytes > 0U &&
				       (uint32_t)(now_ms - drop_ms) < LED_DROP_HOLD_MS,
			.role_tx = link.role_tx,
			.link_state = link.state,
			.lq_percent = link.lq_percent,
			.uptime_ms = alarm ? ZLRS_LED_BOOT_MS : now_ms,
		};

		const zlrs_led_rgb_t color = zlrs_led_frame(&in, now_ms);

		/* The strip holds the colour it was given, so an unchanged frame is not worth a PIO
		 * push; a write that failed is retried, or the strip would stay stale for good. */
		if (!write_ok || memcmp(&color, &on_strip, sizeof(color)) != 0) {
			on_strip = color;
			write_ok = zlrs_led_port_update(color) == 0;
		}
	}
}

K_THREAD_DEFINE(zlrs_led_tid, LED_THREAD_STACK_SIZE, led_thread, NULL, NULL, NULL,
	LED_THREAD_PRIORITY, 0, 0);

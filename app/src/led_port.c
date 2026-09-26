/*
 * Status LED port, see app/src/led_port.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_port.h"

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>

#include "caps.h"

LOG_MODULE_REGISTER(zlrs_led_port, CONFIG_APP_LOG_LEVEL);

/*
 * Without a strip there is no device to look up and nothing to write, and the pattern engine may
 * be trimmed as well: the no-op below is the whole port then (08-firmware 4). The LED thread keeps
 * running as the consumer of the system state (app/src/led_thread.c) either way.
 */
#if !ZLRS_LED_AVAILABLE

int zlrs_led_port_init(void)
{
#if ZLRS_HAS_LED
	LOG_WRN("led strip driver trimmed from this build: status led disabled");
#else
	LOG_WRN("no led strip device on this board: status led disabled");
#endif
	return -1;
}

int zlrs_led_port_update(zlrs_led_rgb_t color)
{
	ARG_UNUSED(color);

	return -1;
}

#else

static bool ready;
static bool led_error_logged;
static struct led_rgb pixels[ZLRS_LED_PIXELS];
static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(zlrs_led));

int zlrs_led_port_init(void)
{
	if (!device_is_ready(strip)) {
		LOG_ERR("led strip not ready (%s)", strip->name);
		return -1;
	}

	ready = true;
	LOG_INF("status led ready (%s)", strip->name);

	return 0;
}

int zlrs_led_port_update(zlrs_led_rgb_t color)
{
	if (!ready) {
		return -1;
	}

	/* ZLRS_LED_PIXELS is 1: the renderer produces one colour, the chain here is one pixel. The
	 * driver applies that chain's colour order and reset timing (devicetree). */
	pixels[0].r = color.r;
	pixels[0].g = color.g;
	pixels[0].b = color.b;

	/*
	 * A failed strip write has to be reported: the LED is the only local feedback, and when the
	 * PIO driver really breaks, "the LED is dark" looks like "the state never happened" while
	 * everything keeps running. The caller retries, so report the first failure only.
	 */
	if (led_strip_update_rgb(strip, pixels, ZLRS_LED_PIXELS) != 0) {
		if (!led_error_logged) {
			led_error_logged = true;
			LOG_ERR("led strip update failed (%s); status led is stale", strip->name);
		}

		return -1;
	}

	return 0;
}

#endif /* CONFIG_ZLRS_LED */

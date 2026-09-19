/*
 * Status LED driver, see app/src/led_port.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_port.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "zlrs/led/led.h"

LOG_MODULE_REGISTER(zlrs_led_port, CONFIG_APP_LOG_LEVEL);

/* 20 Hz looks continuous to the eye without spending time on the strip */
#define LED_UPDATE_MS 50U

/*
 * An alias is not enough, the strip driver has to be compiled in (CONFIG_LED_STRIP): trimming
 * the driver while leaving the alias makes DEVICE_DT_GET reference a device that does not exist.
 */
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_led)) && IS_ENABLED(CONFIG_LED_STRIP)
#define LED_AVAILABLE 1
#else
#define LED_AVAILABLE 0
#endif

/*
 * The pattern engine (core/led) is trimmable too: with it off this port becomes a no-op and the
 * application never has to care whether a LED exists (see docs/design/08-firmware.md 4).
 */
#if !IS_ENABLED(CONFIG_ZLRS_LED) || !LED_AVAILABLE
int zlrs_led_port_init(void)
{
#if IS_ENABLED(CONFIG_ZLRS_LED)
	LOG_WRN("no led strip device on this board: status led disabled");
#endif
	return -1;
}

void zlrs_led_port_update(const zlrs_led_input_t *in, uint32_t now_ms)
{
	(void)in;
	(void)now_ms;
}

#else

static bool ready;
static uint32_t last_update_ms;
static bool led_error_logged;
static struct led_rgb pixels[ZLRS_LED_PIXELS];
static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(zlrs_led));

int zlrs_led_port_init(void)
{
	if (!LED_AVAILABLE) {
		LOG_WRN("no zlrs-led alias on this board: status led disabled");
		return -1;
	}

	if (!device_is_ready(strip)) {
		LOG_ERR("led strip not ready (%s)", strip->name);
		return -1;
	}

	ready = true;
	LOG_INF("status led ready (%s)", strip->name);

	return 0;
}

void zlrs_led_port_update(const zlrs_led_input_t *in, uint32_t now_ms)
{
	const zlrs_led_rgb_t color = zlrs_led_frame(in, now_ms);

	if (!ready || (uint32_t)(now_ms - last_update_ms) < LED_UPDATE_MS) {
		return;
	}
	last_update_ms = now_ms;

	pixels[0].r = color.r;
	pixels[0].g = color.g;
	pixels[0].b = color.b;

	/*
	 * A failed strip write has to be reported: the LED is the only local feedback, and when the
	 * PIO driver really breaks, "the LED is dark" looks like "the state never happened" while
	 * everything keeps running. Called at 20 Hz, so report it once.
	 */
	if (led_strip_update_rgb(strip, pixels, ZLRS_LED_PIXELS) != 0 && !led_error_logged) {
		led_error_logged = true;
		LOG_ERR("led strip update failed (%s); status led is stale", strip->name);
	}
}

#endif /* CONFIG_ZLRS_LED */

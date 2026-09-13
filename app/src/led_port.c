/*
 * 状态灯驱动，见 app/src/led_port.h。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_port.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zlrs_led_port, CONFIG_APP_LOG_LEVEL);

/* 20 Hz 足够让闪烁看起来连续，又不会把时间花在刷灯上 */
#define LED_UPDATE_MS 50U

#if DT_NODE_EXISTS(DT_ALIAS(zlrs_led))
static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(zlrs_led));
#define LED_AVAILABLE 1
#else
#define LED_AVAILABLE 0
#endif

static bool ready;
static uint32_t last_update_ms;
static struct led_rgb pixels[ZLRS_LED_PIXELS];

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

	(void)led_strip_update_rgb(strip, pixels, ZLRS_LED_PIXELS);
}

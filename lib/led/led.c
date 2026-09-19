/*
 * Status LED colours and cadence, see include/zlrs/led/led.h.
 *
 * Brightness is deliberately capped low (<= 30%): this is an indicator for the eye, not a
 * light source. It saves power and does not glare at night.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/led/led.h"

#include <stddef.h>

/* Palette: a handful of colours that stay distinguishable from each other */
static const zlrs_led_rgb_t off = {0U, 0U, 0U};
static const zlrs_led_rgb_t white = {255U, 255U, 255U};
static const zlrs_led_rgb_t purple = {160U, 0U, 200U};
static const zlrs_led_rgb_t yellow = {255U, 180U, 0U};
static const zlrs_led_rgb_t blue = {0U, 90U, 255U};
static const zlrs_led_rgb_t green = {0U, 255U, 0U};
static const zlrs_led_rgb_t red = {255U, 0U, 0U};
static const zlrs_led_rgb_t orange = {255U, 80U, 0U};

/* Scale brightness by a percentage (0-100) */
static zlrs_led_rgb_t scale(zlrs_led_rgb_t color, uint8_t percent)
{
	zlrs_led_rgb_t out;

	out.r = (uint8_t)(((uint16_t)color.r * percent) / 100U);
	out.g = (uint8_t)(((uint16_t)color.g * percent) / 100U);
	out.b = (uint8_t)(((uint16_t)color.b * percent) / 100U);

	return out;
}

/*
 * Blink: on for on_ms out of every period_ms, off the rest of the time.
 * Convention: periods are at least one second (1 Hz at most) and urgency comes from colour
 * plus duty cycle, never from faster blinking. See docs/design/11-ux.md.
 */
static zlrs_led_rgb_t blink(zlrs_led_rgb_t color, uint32_t now_ms, uint32_t period_ms,
			    uint32_t on_ms)
{
	return ((now_ms % period_ms) < on_ms) ? color : off;
}

/* One flash every period_ms, used to overlay a hint on a steady colour */
static bool single_flash(uint32_t now_ms, uint32_t period_ms, uint32_t on_ms)
{
	return (now_ms % period_ms) < on_ms;
}

/*
 * Theme colour: blue for TX, green for RX. A healthy link only ever shows the theme colour,
 * with cadence and brightness marking the state. Low LQ is not an error, so the colour stays.
 */
static zlrs_led_rgb_t theme_color(const zlrs_led_input_t *in)
{
	return in->role_tx ? blue : green;
}

/* Link state -> colour and cadence; only CONNECTED looks at LQ */
static zlrs_led_rgb_t link_state_frame(const zlrs_led_input_t *in, uint32_t now_ms)
{
	const zlrs_led_rgb_t theme = theme_color(in);

	switch (in->link_state) {
	case ZLRS_STATE_CONNECTED:
		/* Steady theme colour, LQ only changes brightness: >=80 30%, 50-79 18%, <50 10% */
		if (in->lq_percent >= 80U) {
			return scale(theme, 30U);
		}
		if (in->lq_percent >= 50U) {
			return scale(theme, 18U);
		}
		return scale(theme, 10U);
	case ZLRS_STATE_PAIRING:
		return blink(scale(theme, 60U), now_ms, 1000U, 300U);
	case ZLRS_STATE_SEARCHING:
		return blink(scale(theme, 40U), now_ms, 2000U, 150U);
	case ZLRS_STATE_UNPAIRED:
		/* One of the shared alert colours: waiting for pairing */
		return blink(scale(yellow, 40U), now_ms, 1000U, 120U);
	case ZLRS_STATE_UNCONFIGURED:
	default:
		/* One of the shared alert colours: waiting for a root key */
		return blink(scale(purple, 40U), now_ms, 2000U, 200U);
	}
}

zlrs_led_rgb_t zlrs_led_frame(const zlrs_led_input_t *in, uint32_t now_ms)
{
	zlrs_led_rgb_t frame;

	if (in == NULL) {
		return off;
	}

	/* 1. Config mode: white, slow blink */
	if (in->config_mode) {
		return blink(scale(white, 30U), now_ms, 1000U, 500U);
	}

	/* 2. Boot self-test: steady theme colour for 1.5 s (blue = TX, green = RX) */
	if (in->uptime_ms < 1500U) {
		return scale(theme_color(in), 30U);
	}

	/* 3. Failsafe: red, fast blink — "no usable link" matters most */
	if (in->failsafe) {
		return blink(scale(red, 60U), now_ms, 1000U, 800U);
	}

	/* 4. link state */
	frame = link_state_frame(in, now_ms);

	/* 5. Drop hint: a single flash every 2 s on top of the steady CONNECTED colour */
	if (in->link_state == ZLRS_STATE_CONNECTED && in->drop_recent &&
	    single_flash(now_ms, 2000U, 300U)) {
		frame = scale(orange, 60U);
	}

	return frame;
}

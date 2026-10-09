/* SPDX-License-Identifier: Apache-2.0 */
#include "ui/led.h"
#include <stddef.h>

static const struct zlrs_led_color off = {0U, 0U, 0U};
static const struct zlrs_led_color white = {255U, 255U, 255U};
static const struct zlrs_led_color purple = {160U, 0U, 200U};
static const struct zlrs_led_color yellow = {255U, 180U, 0U};
static const struct zlrs_led_color blue = {0U, 90U, 255U};
static const struct zlrs_led_color green = {0U, 255U, 0U};
static const struct zlrs_led_color red = {255U, 0U, 0U};
static const struct zlrs_led_color orange = {255U, 80U, 0U};

static struct zlrs_led_color blink(struct zlrs_led_color color, uint32_t now_ms, uint32_t period_ms,
				   uint32_t on_ms)
{
	return ((now_ms % period_ms) < on_ms) ? color : off;
}

static struct zlrs_led_color alternate(struct zlrs_led_color first, struct zlrs_led_color second,
				       uint32_t now_ms, uint32_t period_ms)
{
	return ((now_ms % period_ms) < (period_ms / 2U)) ? first : second;
}

static struct zlrs_led_color theme_color(bool role_tx)
{
	return role_tx ? blue : green;
}

static struct zlrs_led_color link_state_frame(const struct zlrs_led_frame_input *in,
					      uint32_t now_ms)
{
	const struct zlrs_led_color theme = theme_color(in->role_tx);

	switch (in->link_state) {
	case ZLRS_STATE_CONNECTED:
		if (in->lq_percent >= 80U) {
			return blink(theme, now_ms, 250U, 200U);
		}
		if (in->lq_percent >= 50U) {
			return blink(theme, now_ms, 250U, 50U);
		}
		return blink(theme, now_ms, 500U, 250U);
	case ZLRS_STATE_PAIRING:
		return blink(theme, now_ms, 1000U, 300U);
	case ZLRS_STATE_SEARCHING:
		return theme;
	case ZLRS_STATE_UNPAIRED:
		return alternate(theme, yellow, now_ms, 500U);
	case ZLRS_STATE_UNCONFIGURED:
	default:
		return blink(purple, now_ms, 2000U, 200U);
	}
}

struct zlrs_led_color zlrs_app_led_frame(const struct zlrs_led_frame_input *in, uint32_t now_ms)
{
	if (in == NULL) {
		return off;
	}
	if (in->mgmt_mode) {
		return white;
	}
	if (in->failsafe) {
		return alternate(theme_color(in->role_tx), red, now_ms, 500U);
	}
	if (in->drop_recent) {
		return alternate(theme_color(in->role_tx), orange, now_ms, 500U);
	}
	return link_state_frame(in, now_ms);
}

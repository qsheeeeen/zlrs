/*
 * Application status LED pattern interface. Kept inside the app because its policy is product UI,
 * not a portable protocol or link feature.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/link/state.h"

struct zlrs_led_color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

struct zlrs_led_frame_input {
	bool mgmt_mode;
	bool link_lost;
	bool drop_recent;
	bool role_tx;
	zlrs_link_state_t link_state;
	uint8_t lq_percent;
};

struct zlrs_led_color zlrs_app_led_frame(const struct zlrs_led_frame_input *in, uint32_t now_ms);

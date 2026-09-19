/*
 * Status LED: one WS2812 pixel expresses link state and alerts. Colour and cadence both carry
 * meaning (colour alone fails in bright light or with colour blindness). Priorities: 11-ux.
 * app/src/led_port.c。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/link/link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint8_t r;
	uint8_t g;
	uint8_t b;
} zlrs_led_rgb_t;

typedef struct {
	bool config_mode;  /* in config mode (the app filters out blink-and-miss sessions, see ports.c) */
	bool configured;   /* root key present */
	bool paired;       /* pairing record present */
	bool failsafe;     /* sending/outputting failsafe values (TX input timeout, RX unconnected) */
	bool drop_recent;  /* a drop in the last second (passthrough backlog or ARQ give-up) */
	bool role_tx;      /* role, used only during the boot self-test window */
	uint8_t link_state; /* control link state, ZLRS_STATE_* */
	uint8_t lq_percent;
	uint32_t uptime_ms; /* ms since power-up: the first 1.5 s show the role colour */
} zlrs_led_input_t;

/* Colour to show now. now_ms is a millisecond clock, used only for blink phase */
zlrs_led_rgb_t zlrs_led_frame(const zlrs_led_input_t *in, uint32_t now_ms);

/* Number of pixels to drive: one on the prototype */
#define ZLRS_LED_PIXELS 1U

#ifdef __cplusplus
}
#endif

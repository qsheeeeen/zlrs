/*
 * Status LED: one WS2812 pixel expresses link state and alerts. Colour and cadence both carry
 * meaning (colour alone fails in bright light or with colour blindness). Priorities: 11-ux.
 * The application's LED thread (app/src/led_thread.c) fills this from the system state it
 * subscribes to, and app/src/led_port.c writes the result to the strip.
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
	bool config_mode; /* show the config colour (the consumer delays it, see app/src/led_thread.c) */
	bool configured; /* root key present */
	bool paired;     /* pairing record present */
	bool failsafe;   /* sending/outputting failsafe values (TX input timeout, RX unconnected) */
	bool drop_recent;   /* a drop in the last second (passthrough backlog or ARQ give-up) */
	bool role_tx;       /* role, used only during the boot self-test window */
	uint8_t link_state; /* control link state, ZLRS_STATE_* */
	uint8_t lq_percent;
	uint32_t uptime_ms; /* ms since power-up: the first ZLRS_LED_BOOT_MS show the role colour */
} zlrs_led_input_t;

/* Colour to show now. now_ms is a millisecond clock, used only for blink phase */
zlrs_led_rgb_t zlrs_led_frame(const zlrs_led_input_t *in, uint32_t now_ms);

/* Boot self-test window: the role colour is shown for this long after power-up */
#define ZLRS_LED_BOOT_MS 1500U

/* Number of pixels to drive: one on the prototype */
#define ZLRS_LED_PIXELS 1U

#ifdef __cplusplus
}
#endif

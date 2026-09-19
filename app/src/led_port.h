/*
 * Status LED driver side: write the colour core/led computes to the on-board WS2812 (needs the
 * `zlrs-led` alias and degrades to a no-op without it). See doc/11-ux.md.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "zlrs/led/led.h"

/* Look up the device; -1 when the board has no LED, later update calls stay safe */
int zlrs_led_port_init(void);

/* Turn the state into a colour and refresh (rate-limited to 20 Hz); now_ms uses the link clock */
void zlrs_led_port_update(const zlrs_led_input_t *in, uint32_t now_ms);

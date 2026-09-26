/*
 * Status LED port: the abstract frame the renderer computed, onto the device this board has.
 *
 * This file knows nothing about link states, priorities or cadence — that is the renderer's job
 * and it lives with the thread that calls in (app/src/led_thread.c). What is left is the mapping
 * from one abstract pixel (core/led's zlrs_led_rgb_t, device independent) to the concrete device:
 * the on-board WS2812 chain, whose colour order, pixel count and reset timing come from devicetree
 * (`zlrs-led`). Without such a device the port becomes a no-op (see doc/11-ux.md).
 *
 * The LED thread is the only caller: the strip has exactly one writer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "zlrs/led/led.h"

/* Look up the device; -1 when the board has no usable strip, later updates stay safe */
int zlrs_led_port_init(void);

/* Show one frame: 0 when the strip took it, -1 when it did not (the caller retries and reports,
 * see app/src/led_thread.c) */
int zlrs_led_port_update(zlrs_led_rgb_t color);

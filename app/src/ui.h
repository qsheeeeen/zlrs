/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/zbus/zbus.h>

struct zlrs_input {
	uint32_t button_holds;
};

ZBUS_CHAN_DECLARE(zlrs_input_chan);

#define ZLRS_BUTTON_PAIR_WINDOW_MS 30000

/* Boot-time safety window; explicit USB maintenance is independent of the button. */
bool zlrs_button_pairing_allowed(void);

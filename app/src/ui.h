/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <zephyr/zbus/zbus.h>

struct zlrs_input {
	uint32_t button_holds;
};

ZBUS_CHAN_DECLARE(zlrs_input_chan);

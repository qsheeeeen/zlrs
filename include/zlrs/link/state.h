/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Public link state values; no link instance or mutable storage is exposed here. */
typedef enum {
	ZLRS_STATE_UNCONFIGURED = 0,
	ZLRS_STATE_UNPAIRED,
	ZLRS_STATE_PAIRING,
	ZLRS_STATE_SEARCHING,
	ZLRS_STATE_CONNECTED,
} zlrs_link_state_t;

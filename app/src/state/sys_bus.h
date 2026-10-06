/*
 * System state, broadcast on zbus.
 *
 * Producers publish what the device *is* doing and nothing about how it should look: opinions
 * live with the consumers. The LED thread (app/src/ui/led.c) is the one consumer today and it
 * owns the display rules — colours, cadence and priorities live in app/src/ui/led.c
 * (doc/10-ux.md).
 *
 * One channel per fact, because a channel has one writer: a publisher writes the whole message,
 * so two owners on one channel would each blank the other's field.
 *
 *   zlrs_link_chan   what the control link is doing (app/src/main.c, every frame)
 *   zlrs_alert_chan  failsafe, faults and passthrough drops (app/src/main.c, then the fault loop)
 *   zlrs_mode_chan   an open management session (app/src/mgmt/mode.c)
 *   zlrs_input_chan  what the user asked for with the hardware (app/src/ui/button.c)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/zbus/zbus.h>

/* What the control link is doing; state is a ZLRS_STATE_* (include/zlrs/link/link.h) */
struct zlrs_link_status {
	bool role_tx;
	bool configured; /* root key present */
	bool paired;     /* pairing record present */
	uint8_t state;   /* control link state, ZLRS_STATE_* */
	uint8_t lq_percent;
};

/* What is wrong: the facts behind the alert colours, not the colours themselves */
struct zlrs_alerts {
	bool failsafe;            /* TX: no stick data; RX: no link — both after a first connection */
	bool fault;               /* fatal error, the device is off the air (the fault loop publishes) */
	uint32_t data_drop_bytes; /* passthrough backlog or ARQ give-up, cumulative */
};

/* What the device is doing, as opposed to how a link is doing */
struct zlrs_mode {
	bool session; /* a management session is open */
};

/* What the device is being asked for through its hardware; counters, so a late reader still sees it */
struct zlrs_input {
	uint32_t button_holds; /* long presses that reached the pairing threshold */
};

ZBUS_CHAN_DECLARE(zlrs_link_chan, zlrs_alert_chan, zlrs_mode_chan, zlrs_input_chan);

/*
 * Publishers. The callers run every frame; each helper only broadcasts when the message changed,
 * so the bus carries state changes instead of a heartbeat (app/src/state/sys_bus.c).
 */
void zlrs_sys_publish_link(const struct zlrs_link_status *status);
void zlrs_sys_publish_alerts(const struct zlrs_alerts *alerts);
void zlrs_sys_publish_mode(const struct zlrs_mode *mode);
void zlrs_sys_publish_input(const struct zlrs_input *input);

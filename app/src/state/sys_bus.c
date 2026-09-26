/*
 * System state channels, see app/src/state/sys_bus.h.
 *
 * The channels are defined with no observers: who listens is the consumer's business, and the LED
 * thread adds itself statically (ZBUS_CHAN_ADD_OBS in app/src/ui/led.c). A second consumer —
 * the shell, a log exporter, telemetry — can do the same without touching anything here.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sys_bus.h"

#include <string.h>

#include <zephyr/kernel.h>

ZBUS_CHAN_DEFINE(zlrs_link_chan, struct zlrs_link_status, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0));

ZBUS_CHAN_DEFINE(zlrs_alert_chan, struct zlrs_alerts, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0));

ZBUS_CHAN_DEFINE(zlrs_mode_chan, struct zlrs_mode, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0));

ZBUS_CHAN_DEFINE(zlrs_input_chan, struct zlrs_input, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(0));

/*
 * Publishing is fire and forget: the message is already in the channel, so a consumer that missed
 * the notification reads the same value on its next pass (app/src/ui/led.c).
 */

void zlrs_sys_publish_link(const struct zlrs_link_status *status)
{
	static struct zlrs_link_status last;
	static bool published;

	if (published && memcmp(&last, status, sizeof(last)) == 0) {
		return;
	}
	last = *status;
	published = true;

	(void)zbus_chan_pub(&zlrs_link_chan, status, K_NO_WAIT);
}

void zlrs_sys_publish_alerts(const struct zlrs_alerts *alerts)
{
	static struct zlrs_alerts last;
	static bool published;

	if (published && memcmp(&last, alerts, sizeof(last)) == 0) {
		return;
	}
	last = *alerts;
	published = true;

	(void)zbus_chan_pub(&zlrs_alert_chan, alerts, K_NO_WAIT);
}

void zlrs_sys_publish_mode(const struct zlrs_mode *mode)
{
	static struct zlrs_mode last;
	static bool published;

	if (published && memcmp(&last, mode, sizeof(last)) == 0) {
		return;
	}
	last = *mode;
	published = true;

	(void)zbus_chan_pub(&zlrs_mode_chan, mode, K_NO_WAIT);
}

void zlrs_sys_publish_input(const struct zlrs_input *input)
{
	static struct zlrs_input last;
	static bool published;

	if (published && memcmp(&last, input, sizeof(last)) == 0) {
		return;
	}
	last = *input;
	published = true;

	(void)zbus_chan_pub(&zlrs_input_chan, input, K_NO_WAIT);
}

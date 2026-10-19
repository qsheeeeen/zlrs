/*
 * Data link I/O: the passthrough byte stream between host and air.
 *
 * The link itself lives in core/link; this is its host side — the endpoint the host talks to, the
 * RX/TX rings and the drop counters the data link reports (doc/05-architecture.md).
 * The implementation binds directly to devicetree UART/CDC devices.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/zbus/zbus.h>

struct zlrs_serial_status {
	bool ready;
	bool usb_active;
	bool input_paused;
	uint32_t dropped;
};

ZBUS_CHAN_DECLARE(zlrs_serial_chan);
/* Publish changed serial status to zbus from the main thread. */
void zlrs_serial_publish_state(void);

/* For bench debugging: state of one data endpoint (`zlrs ports` prints it) */
typedef struct {
	const char *name;
	bool ready;          /* the endpoint exists on the board and the device is ready */
	bool active;         /* UART by default; configured, nonsuspended USB takes priority */
	uint32_t rx_total;   /* bytes received since boot */
	uint32_t tx_total;   /* bytes handed to the peripheral since boot */
	uint32_t rx_dropped; /* UART overflow and discarded inactive-endpoint bytes */
	uint32_t rx_queued;
	uint32_t tx_queued;
	bool rx_paused; /* USB input is waiting for application ring space */
} zlrs_serial_state_t;

/*
 * Read the state of both data endpoints, returns how many were filled in.
 * Device readiness is independent of whether a USB host is present.
 */
size_t zlrs_serial_state(zlrs_serial_state_t *out, size_t count);

/* baud = 0 keeps the board default (115200); non-zero overrides it (set by the management tool) */
int zlrs_serial_init(uint32_t baud);

/* Pull data out of the RX ring buffer, returns the bytes actually read */
uint16_t zlrs_serial_read(uint8_t *buf, uint16_t len);

/* Accept a whole air packet, or leave it with RF for retry when the endpoint is full. */
bool zlrs_serial_receive(const uint8_t *buf, uint16_t len);

/* Bytes never delivered: UART overflow plus those received on the inactive endpoint */
uint32_t zlrs_serial_dropped(void);

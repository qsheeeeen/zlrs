/*
 * Data link I/O: the passthrough byte stream between host and air.
 *
 * The link itself lives in core/link; this is its host side — the endpoint the host talks to, the
 * RX/TX rings and the drop counters the data link reports (doc/06-architecture.md).
 * The implementation binds directly to devicetree UART/CDC devices.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/zbus/zbus.h>

struct zlrs_data_status {
	bool ready;
	bool usb_active;
	bool input_paused;
	uint32_t dropped;
};
ZBUS_CHAN_DECLARE(zlrs_data_chan);
void zlrs_link_data_poll(void);

/* For bench debugging: state of one data endpoint (`zlrs ports` prints it) */
typedef struct {
	const char *name;
	bool ready; /* the endpoint exists on the board and the device is ready */
	bool active; /* this is the selected endpoint (selected locally from DTR and received bytes) */
	uint32_t rx_total;   /* bytes received (evidence that the host is using it) */
	uint32_t rx_dropped; /* UART overflow and discarded inactive-endpoint bytes */
	uint32_t rx_queued;
	uint32_t tx_queued;
	bool rx_paused; /* USB input is waiting for application ring space */
} zlrs_link_data_state_t;

/*
 * Read the state of both data endpoints, returns how many were filled in.
 * The choice itself (DTR, host bytes seen) is implemented locally in data.c.
 */
size_t zlrs_link_data_state(zlrs_link_data_state_t *out, size_t count);

/* baud = 0 keeps the board default (115200); non-zero overrides it (set by the management tool) */
int zlrs_link_data_init(uint32_t baud);

/* Pull data out of the RX ring buffer, returns the bytes actually read */
uint16_t zlrs_link_data_read(uint8_t *buf, uint16_t len);

/* Accept a whole air packet, or leave it with RF for retry when the endpoint is full. */
bool zlrs_link_data_receive(const uint8_t *buf, uint16_t len);

/* Bytes never delivered: UART overflow plus those received on the inactive endpoint */
uint32_t zlrs_link_data_dropped(void);

/* Used during management mode: drop bytes from both endpoints (counted, never delivered) */
void zlrs_link_data_discard_rx(void);

/*
 * Data link I/O: the passthrough byte stream between host and air.
 *
 * The link itself lives in core/link; this is its host side — the endpoint the host talks to, the
 * RX/TX rings and the drop counters the data link reports (doc/07-architecture.md 5). Which device
 * the implementation binds directly to devicetree UART/CDC devices.
 *
 * Naming: `<dir>/<file>.c` carries `zlrs_<dir>_<file>_*()`, so this is `zlrs_link_data_*()`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* For bench debugging: state of one data endpoint (`zlrs ports` prints it) */
typedef struct {
	const char *name;
	bool ready; /* the endpoint exists on the board and the device is ready */
	bool active; /* this is the selected endpoint (selected locally from DTR and received bytes) */
	uint32_t rx_total;   /* bytes received (evidence that the host is using it) */
	uint32_t rx_dropped; /* bytes dropped because this was not the active endpoint */
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

/* Queue into the TX ring buffer and start the IRQ transmit, returns the bytes accepted */
uint16_t zlrs_link_data_write(const uint8_t *buf, uint16_t len);

/* Bytes never delivered: dropped when full plus those received on the inactive endpoint */
uint32_t zlrs_link_data_dropped(void);
/* Bytes the air side handed to the link that the host side had no room for */
uint32_t zlrs_link_data_tx_dropped(void);

/* Used during management mode: drop bytes from both endpoints (counted, never delivered) */
void zlrs_link_data_discard_rx(void);

/*
 * Serial ports: the board's logical ports and the endpoints behind them.
 *
 * A logical port arrives from devicetree as a pair of optional endpoints — a UART and a USB CDC —
 * and which one the host is on is a hardware fact this file owns (doc/08-firmware.md 1). What
 * flows through the port is the link's business (`link/data.c` for the passthrough,
 * `link/crsf.c` for the handset side); the management protocol has its own CDC (`mgmt/port.c`).
 *
 * Naming: `<dir>/<file>.c` carries `zlrs_<dir>_<file>_*()`, so this is `zlrs_port_serial_*()`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

/* A logical port has at most two endpoints; endpoint order is fixed: 0 = UART, 1 = USB CDC */
#define ZLRS_PORT_SERIAL_MAX 2

struct zlrs_port_serial_pair {
	const struct device *uart; /* NULL = this alias is not wired on the board */
	const struct device *cdc;
};

/* The two logical ports of this product: control (CRSF) and data (passthrough) */
const struct zlrs_port_serial_pair *zlrs_port_serial_control(void);
const struct zlrs_port_serial_pair *zlrs_port_serial_data(void);

/* Endpoint by index; NULL when that alias is not wired on this board */
const struct device *zlrs_port_serial_endpoint(const struct zlrs_port_serial_pair *pair,
					      size_t idx);

/*
 * The endpoint the host is on: USB while it is in use, the UART once something talks on it, NULL
 * when neither is ready. Why USB alone is not the answer is in serial.c.
 */
const struct device *zlrs_port_serial_active(const struct zlrs_port_serial_pair *pair);

/* Evidence for that choice: host bytes seen (called from the endpoint's RX ISR) */
void zlrs_port_serial_rx_seen(const struct device *dev, uint32_t bytes);

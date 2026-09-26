/*
 * Serial ports, see ports/serial.h: device handles and endpoint choice.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ports/serial.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include <stdbool.h>
#include <stddef.h>

LOG_MODULE_REGISTER(zlrs_port_serial, CONFIG_APP_LOG_LEVEL);

/* Device handles: the aliases a board wires (doc/08-firmware.md 1) */

static const struct zlrs_port_serial_pair control_pair = {
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_control_uart))
	.uart = DEVICE_DT_GET(DT_ALIAS(zlrs_control_uart)),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_control_cdc))
	.cdc = DEVICE_DT_GET(DT_ALIAS(zlrs_control_cdc)),
#endif
};

static const struct zlrs_port_serial_pair data_pair = {
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_data_uart))
	.uart = DEVICE_DT_GET(DT_ALIAS(zlrs_data_uart)),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_data_cdc))
	.cdc = DEVICE_DT_GET(DT_ALIAS(zlrs_data_cdc)),
#endif
};

const struct device *zlrs_port_serial_endpoint(const struct zlrs_port_serial_pair *pair,
					      size_t idx)
{
	return (idx == 0U) ? pair->uart : pair->cdc;
}

/* "USB is in use" means the host really opened the CDC: enumeration alone does not count */
static bool cdc_opened(const struct device *dev)
{
	uint32_t dtr = 0U;

	if (!device_is_ready(dev)) {
		return false;
	}

	if (uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr) != 0) {
		return false;
	}

	return dtr != 0U;
}

/*
 * Evidence that the host is using this endpoint. The textbook answer is the CDC's DTR, but
 * over USB/IP forwarding SET_CONTROL_LINE_STATE may never arrive, and trusting DTR alone
 * leaves the endpoint unselected (its bytes are dropped as "not the active one"). So a second
 * signal counts: host bytes seen. The ISRs record it before checking the active endpoint.
 */
/*
 * A stray byte is not evidence: an unconnected (or briefly floating) line can produce one, and
 * switching a port to it sends the passthrough data into a cable that has nothing on the other end.
 * A real peer sends a stream, so a few bytes count and one does not.
 */
#define HOST_SEEN_MIN_BYTES 4U
#define HOST_SEEN_MAX       ZLRS_PORT_SERIAL_MAX * 2

static struct host_seen {
	const struct device *dev;
	uint32_t bytes;
} host_seen_table[HOST_SEEN_MAX];

void zlrs_port_serial_rx_seen(const struct device *dev, uint32_t bytes)
{
	for (size_t i = 0U; i < ARRAY_SIZE(host_seen_table); i++) {
		if (host_seen_table[i].dev == dev) {
			host_seen_table[i].bytes += bytes;
			return;
		}
		if (host_seen_table[i].dev == NULL) {
			host_seen_table[i].dev = dev;
			host_seen_table[i].bytes = bytes;
			return;
		}
	}
}

static bool host_seen_any(const struct device *dev)
{
	for (size_t i = 0U; i < ARRAY_SIZE(host_seen_table); i++) {
		if (host_seen_table[i].dev == dev) {
			return host_seen_table[i].bytes >= HOST_SEEN_MIN_BYTES;
		}
	}

	return false;
}

/* With both present USB wins while it is in use; NULL when there is neither */
const struct device *zlrs_port_serial_active(const struct zlrs_port_serial_pair *pair)
{
	const bool uart_ok = (pair->uart != NULL) && device_is_ready(pair->uart);
	const bool cdc_ok = (pair->cdc != NULL) && device_is_ready(pair->cdc);

	if (uart_ok && cdc_ok) {
		const bool cdc_in_use = cdc_opened(pair->cdc) || host_seen_any(pair->cdc);
		const bool uart_in_use = host_seen_any(pair->uart);

		/*
		 * USB is the port the host is already on; the UART only wins once something is actually
		 * talking on it. With the old "UART unless the CDC is in use" rule, a board that receives
		 * air data but never gets a host write (the receive side of a passthrough) kept routing to
		 * a UART that on the bench has nothing attached, and the data went nowhere.
		 */
		if (cdc_in_use || !uart_in_use) {
			return pair->cdc;
		}

		return pair->uart;
	}

	if (uart_ok) {
		return pair->uart;
	}

	return cdc_ok ? pair->cdc : NULL;
}

const struct zlrs_port_serial_pair *zlrs_port_serial_control(void)
{
	return &control_pair;
}

const struct zlrs_port_serial_pair *zlrs_port_serial_data(void)
{
	return &data_pair;
}

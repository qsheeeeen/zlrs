/*
 * Data link I/O, see link/data.h: rings, ISR and counters for the passthrough.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "link/data.h"

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include <stdbool.h>
#include <stddef.h>

#include "ports/serial.h"

LOG_MODULE_REGISTER(zlrs_link_data, CONFIG_APP_LOG_LEVEL);

/*
 * Sized for the air side: an ARQ frame can carry several hundred bytes and the host drains the
 * port in bursts, so a kilobyte per direction left both ends dropping data that had already been
 * received correctly.
 */
#define DATA_RX_RING_LEN 4096U
#define DATA_TX_RING_LEN 4096U

/* One buffer set per endpoint; dev == NULL means the alias is not wired */
struct data_ep {
	const struct device *dev;
	struct ring_buf rx;
	struct ring_buf tx;
	uint8_t rx_mem[DATA_RX_RING_LEN];
	uint8_t tx_mem[DATA_TX_RING_LEN];
	uint32_t rx_dropped;
	uint32_t rx_total;   /* bytes received (one of the endpoint-choice inputs) */
	uint32_t tx_dropped; /* bytes the air side handed over that the host side had no room for */
	bool ready;
};

static struct data_ep data_eps[ZLRS_PORT_SERIAL_MAX];
static uint32_t data_ready_ports;

static struct data_ep *data_ep_of(const struct device *dev)
{
	for (size_t i = 0U; i < ZLRS_PORT_SERIAL_MAX; i++) {
		if (data_eps[i].ready && data_eps[i].dev == dev) {
			return &data_eps[i];
		}
	}

	return NULL;
}

static void data_isr(const struct device *dev, void *user_data)
{
	struct data_ep *const ep = user_data;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t chunk[64];
			const int n = uart_fifo_read(dev, chunk, sizeof(chunk));

			if (n > 0) {
				/* Record the evidence first: endpoint choice depends on it, see zlrs_port_serial_active() */
				zlrs_port_serial_rx_seen(dev, (uint32_t)n);
				ep->rx_total += (uint32_t)n;
			}

			if (n > 0 && ring_buf_put(&ep->rx, chunk, (uint32_t)n) != (uint32_t)n) {
				ep->rx_dropped++; /* passthrough is best-effort: drop when full */
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *claim = NULL;
			const uint32_t claimed = ring_buf_get_claim(&ep->tx, &claim, 64U);

			if (claimed == 0U) {
				/* A timer can preempt a CDC callback and enqueue output. Recheck under
				 * the IRQ lock so this callback cannot disable that new TX wakeup. */
				const unsigned int key = irq_lock();

				if (ring_buf_is_empty(&ep->tx)) {
					uart_irq_tx_disable(dev);
				}
				irq_unlock(key);
				continue;
			}

			const int sent = uart_fifo_fill(dev, claim, (int)claimed);

			ring_buf_get_finish(&ep->tx, (sent > 0) ? (uint32_t)sent : 0U);
		}
	}
}

/* Drop bytes from the inactive endpoint: keeping them would send stale data after a switch */
static void data_ep_drop_rx(struct data_ep *const ep)
{
	uint8_t scratch[32];
	uint32_t n;

	while ((n = ring_buf_get(&ep->rx, scratch, sizeof(scratch))) > 0U) {
		ep->rx_dropped += n;
	}
}

void zlrs_link_data_discard_rx(void)
{
	for (size_t i = 0U; i < ZLRS_PORT_SERIAL_MAX; i++) {
		if (data_eps[i].ready) {
			data_ep_drop_rx(&data_eps[i]);
		}
	}
}

size_t zlrs_link_data_state(zlrs_link_data_state_t *out, size_t count)
{
	const struct device *const active = zlrs_port_serial_active(zlrs_port_serial_data());
	size_t written = 0U;

	for (size_t i = 0U; (i < ZLRS_PORT_SERIAL_MAX) && (written < count); i++) {
		if (data_eps[i].dev == NULL) {
			continue;
		}

		out[written].name = data_eps[i].dev->name;
		out[written].ready = data_eps[i].ready;
		out[written].active = (data_eps[i].dev == active);
		out[written].rx_total = data_eps[i].rx_total;
		out[written].rx_dropped = data_eps[i].rx_dropped;
		written++;
	}

	return written;
}

int zlrs_link_data_init(uint32_t baud)
{
	for (size_t i = 0U; i < ZLRS_PORT_SERIAL_MAX; i++) {
		const struct device *const dev =
			zlrs_port_serial_endpoint(zlrs_port_serial_data(), i);

		if (dev == NULL) {
			continue; /* alias not wired on this board */
		}

		if (!device_is_ready(dev)) {
			LOG_ERR("data port %s not ready", dev->name);
			continue;
		}

		if (baud > 0U) {
			const int rc = uart_line_ctrl_set(dev, UART_LINE_CTRL_BAUD_RATE, baud);

			if (rc != 0) {
				LOG_WRN("data port %s baud %u rejected: %d", dev->name,
					(unsigned int)baud, rc);
			}
		}

		ring_buf_init(&data_eps[i].rx, sizeof(data_eps[i].rx_mem), data_eps[i].rx_mem);
		ring_buf_init(&data_eps[i].tx, sizeof(data_eps[i].tx_mem), data_eps[i].tx_mem);
		data_eps[i].dev = dev;
		data_eps[i].ready = true;
		data_ready_ports++;

		/*
		 * Must be checked: a driver without IRQ transfers returns -ENOSYS/-ENOTSUP, and ignoring
		 * that means TX works while RX is silently dead. On failure the endpoint is marked
		 * unusable so the layers above take the "no such endpoint" path. rx_enable returns void.
		 */
		if (uart_irq_callback_user_data_set(dev, data_isr, &data_eps[i]) != 0) {
			LOG_ERR("data port %s: RX setup failed; endpoint disabled", dev->name);
			data_eps[i].ready = false;
			data_ready_ports--;
			continue;
		}
		uart_irq_rx_enable(dev);

		LOG_INF("data port ready (%s)", dev->name);
	}

	if (data_ready_ports == 0U) {
		LOG_WRN("no data port on this board: passthrough disabled");
		return -1;
	}

	return 0;
}

uint16_t zlrs_link_data_read(uint8_t *buf, uint16_t len)
{
	const struct device *const active = zlrs_port_serial_active(zlrs_port_serial_data());
	uint16_t got = 0U;

	for (size_t i = 0U; i < ZLRS_PORT_SERIAL_MAX; i++) {
		if (!data_eps[i].ready) {
			continue;
		}

		if (data_eps[i].dev == active) {
			got += (uint16_t)ring_buf_get(&data_eps[i].rx, buf + got,
						      (uint32_t)(len - got));
			continue;
		}

		data_ep_drop_rx(
			&data_eps[i]); /* drop bytes that arrived on the inactive endpoint */
	}

	return got;
}

uint16_t zlrs_link_data_write(const uint8_t *buf, uint16_t len)
{
	struct data_ep *const ep = data_ep_of(zlrs_port_serial_active(zlrs_port_serial_data()));

	if (ep == NULL) {
		return 0U;
	}

	const uint32_t written = ring_buf_put(&ep->tx, buf, len);

	if (written > 0U) {
		/* tx_enable returns void; a failed send only shows up as a peer timeout or retransmit */
		uart_irq_tx_enable(ep->dev);
	}
	if (written < len) {
		/* Counted, not silent: this is passthrough data the host may never see */
		ep->tx_dropped += (uint32_t)(len - written);
	}

	return (uint16_t)written;
}

uint32_t zlrs_link_data_tx_dropped(void)
{
	uint32_t dropped = 0U;

	for (size_t i = 0U; i < ZLRS_PORT_SERIAL_MAX; i++) {
		dropped += data_eps[i].tx_dropped;
	}

	return dropped;
}

uint32_t zlrs_link_data_dropped(void)
{
	uint32_t dropped = 0U;

	for (size_t i = 0U; i < ZLRS_PORT_SERIAL_MAX; i++) {
		dropped += data_eps[i].rx_dropped;
	}

	return dropped;
}

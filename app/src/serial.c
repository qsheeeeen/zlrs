/*
 * Data link I/O, see serial.h: rings, ISR and counters for the passthrough.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "serial.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

LOG_MODULE_REGISTER(zlrs_serial, CONFIG_APP_LOG_LEVEL);

ZBUS_CHAN_DEFINE(zlrs_serial_chan, struct zlrs_serial_status, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

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
	struct k_work notify_work;
	uint8_t rx_mem[DATA_RX_RING_LEN];
	uint8_t tx_mem[DATA_TX_RING_LEN];
	uint32_t rx_dropped;
	uint32_t rx_total; /* bytes received (one of the endpoint-choice inputs) */
	bool ready;
	bool rx_paused;
};

enum {
	DATA_UART,
	DATA_USB
};
static struct data_ep data_eps[2] = {
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_data_uart))
	[DATA_UART].dev = DEVICE_DT_GET(DT_ALIAS(zlrs_data_uart)),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_data_cdc))
	[DATA_USB].dev = DEVICE_DT_GET(DT_ALIAS(zlrs_data_cdc)),
#endif
};

/* USB wins after DTR or four received bytes; otherwise a used UART wins.
 * Default to USB so a receive-only host does not have to send first. */
static struct data_ep *data_active(void)
{
	struct data_ep *const uart = &data_eps[DATA_UART];
	struct data_ep *const usb = &data_eps[DATA_USB];
	uint32_t dtr = 0U;

	if (!usb->ready) {
		return uart->ready ? uart : NULL;
	}
	if (!uart->ready) {
		return usb;
	}
	(void)uart_line_ctrl_get(usb->dev, UART_LINE_CTRL_DTR, &dtr);
	return (dtr != 0U || usb->rx_total >= 4U || uart->rx_total < 4U) ? usb : uart;
}

static void data_isr(const struct device *dev, void *user_data)
{
	struct data_ep *const ep = user_data;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t chunk[64];
			const uint32_t room = ring_buf_space_get(&ep->rx);

			/* Leave USB bytes in the CDC FIFO when the application ring is full.
			 * Its OUT endpoint then NAKs naturally instead of accepting and dropping
			 * the rest of the host stream. UART without flow control still drains. */
			if (room == 0U && ep == &data_eps[DATA_USB]) {
				const unsigned int key = irq_lock();

				if (ring_buf_space_get(&ep->rx) == 0U) {
					ep->rx_paused = true;
					uart_irq_rx_disable(dev);
				}
				irq_unlock(key);
				continue;
			}
			const uint32_t want = room > 0U ? MIN(room, sizeof(chunk)) : sizeof(chunk);
			const int n = uart_fifo_read(dev, chunk, want);

			if (n > 0) {
				const uint32_t written = ring_buf_put(&ep->rx, chunk, (uint32_t)n);

				ep->rx_total += (uint32_t)n;
				ep->rx_dropped += (uint32_t)n - written;
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

/* Peripheral notifications run outside the RF owner. CDC callbacks share a work
 * queue with USB transfer bookkeeping and must remain its only FIFO callers. */
static void data_notify(struct k_work *work)
{
	struct data_ep *const ep = CONTAINER_OF(work, struct data_ep, notify_work);
	const unsigned int key = irq_lock();

	/* Serialize this wakeup with the callback's full-ring check. A consumer must
	 * not free space just before the producer disables its notification. */
	if (ep->rx_paused && ring_buf_space_get(&ep->rx) >= 64U) {
		ep->rx_paused = false;
		uart_irq_rx_enable(ep->dev);
	}
	irq_unlock(key);
	if (!ring_buf_is_empty(&ep->tx)) {
		uart_irq_tx_enable(ep->dev);
	}
}

static void data_ep_resume_rx(struct data_ep *ep)
{
	if (ep->rx_paused && ring_buf_space_get(&ep->rx) >= 64U) {
		(void)k_work_submit(&ep->notify_work);
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
	data_ep_resume_rx(ep);
}

void zlrs_serial_discard_rx(void)
{
	for (size_t i = 0U; i < ARRAY_SIZE(data_eps); i++) {
		if (data_eps[i].ready) {
			data_ep_drop_rx(&data_eps[i]);
		}
	}
}

size_t zlrs_serial_state(zlrs_serial_state_t *out, size_t count)
{
	const struct data_ep *const active = data_active();
	size_t written = 0U;

	for (size_t i = 0U; (i < ARRAY_SIZE(data_eps)) && (written < count); i++) {
		if (data_eps[i].dev == NULL) {
			continue;
		}

		out[written].name = data_eps[i].dev->name;
		out[written].ready = data_eps[i].ready;
		out[written].active = (&data_eps[i] == active);
		out[written].rx_total = data_eps[i].rx_total;
		out[written].rx_dropped = data_eps[i].rx_dropped;
		out[written].rx_queued = ring_buf_size_get(&data_eps[i].rx);
		out[written].tx_queued = ring_buf_size_get(&data_eps[i].tx);
		out[written].rx_paused = data_eps[i].rx_paused;
		written++;
	}

	return written;
}

int zlrs_serial_init(uint32_t baud)
{
	for (size_t i = 0U; i < ARRAY_SIZE(data_eps); i++) {
		const struct device *const dev = data_eps[i].dev;

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
		k_work_init(&data_eps[i].notify_work, data_notify);

		/*
		 * Must be checked: a driver without IRQ transfers returns -ENOSYS/-ENOTSUP, and ignoring
		 * that means TX works while RX is silently dead. On failure the endpoint is marked
		 * unusable so the layers above take the "no such endpoint" path. rx_enable returns void.
		 */
		if (uart_irq_callback_user_data_set(dev, data_isr, &data_eps[i]) != 0) {
			LOG_ERR("data port %s: RX setup failed; endpoint disabled", dev->name);
			data_eps[i].ready = false;
			continue;
		}
		data_eps[i].ready = true;
		uart_irq_rx_enable(dev);

		LOG_INF("data port ready (%s)", dev->name);
	}

	if (data_active() == NULL) {
		LOG_WRN("no data port on this board: passthrough disabled");
		return -1;
	}

	return 0;
}

uint16_t zlrs_serial_read(uint8_t *buf, uint16_t len)
{
	const struct data_ep *const active = data_active();
	uint16_t got = 0U;

	for (size_t i = 0U; i < ARRAY_SIZE(data_eps); i++) {
		if (!data_eps[i].ready) {
			continue;
		}

		if (&data_eps[i] == active) {
			got += (uint16_t)ring_buf_get(&data_eps[i].rx, buf + got,
						      (uint32_t)(len - got));
			data_ep_resume_rx(&data_eps[i]);
			continue;
		}

		data_ep_drop_rx(
			&data_eps[i]); /* drop bytes that arrived on the inactive endpoint */
	}

	return got;
}

bool zlrs_serial_receive(const uint8_t *buf, uint16_t len)
{
	struct data_ep *const ep = data_active();

	if (ep == NULL) {
		return false;
	}
	const unsigned int key = irq_lock();
	if (ring_buf_space_get(&ep->tx) < len) {
		irq_unlock(key);
		return false;
	}
	(void)ring_buf_put(&ep->tx, buf, len);
	irq_unlock(key);
	(void)k_work_submit(&ep->notify_work);
	return true;
}

uint32_t zlrs_serial_dropped(void)
{
	uint32_t dropped = 0U;

	for (size_t i = 0U; i < ARRAY_SIZE(data_eps); i++) {
		dropped += data_eps[i].rx_dropped;
	}

	return dropped;
}

void zlrs_serial_publish_state(void)
{
	const struct data_ep *const ep = data_active();
	const struct zlrs_serial_status status = {
		.ready = ep != NULL,
		.usb_active = ep == &data_eps[DATA_USB],
		.input_paused = ep != NULL && ep->rx_paused,
		.dropped = zlrs_serial_dropped(),
	};
	static struct zlrs_serial_status last;
	static bool published;
	if ((!published || last.ready != status.ready || last.usb_active != status.usb_active ||
	     last.input_paused != status.input_paused || last.dropped != status.dropped) &&
	    zbus_chan_pub(&zlrs_serial_chan, &status, K_NO_WAIT) == 0) {
		last = status;
		published = true;
	}
}

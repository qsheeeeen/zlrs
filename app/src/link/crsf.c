/*
 * Control link I/O, see link/crsf.h: endpoint, frame assembly and TX ring for CRSF.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "link/crsf.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "zlrs/crsf/crsf.h"

LOG_MODULE_REGISTER(zlrs_link_crsf, CONFIG_APP_LOG_LEVEL);

/* Frame records are stored as 1 byte length + 1 byte type + payload */
#define CRSF_FRAME_SLOT_LEN (2U + ZLRS_CRSF_PAYLOAD_MAX_LEN)
#define CRSF_FRAME_RING_LEN 512U
#define CRSF_TX_RING_LEN    1024U

/* One TX buffer per endpoint; dev == NULL means the alias is not wired */
struct crsf_ep {
	const struct device *dev;
	struct ring_buf tx;
	uint8_t tx_mem[CRSF_TX_RING_LEN];
	uint32_t rx_total;
	bool ready;
};

static uint8_t crsf_frame_ring_mem[CRSF_FRAME_RING_LEN];
static struct ring_buf crsf_frame_ring;
enum {
	CRSF_UART,
	CRSF_USB
};
static struct crsf_ep crsf_eps[2] = {
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_control_uart))
	[CRSF_UART].dev = DEVICE_DT_GET(DT_ALIAS(zlrs_control_uart)),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_control_cdc))
	[CRSF_USB].dev = DEVICE_DT_GET(DT_ALIAS(zlrs_control_cdc)),
#endif
};
static zlrs_crsf_parser_t crsf_parser;
static const struct device *crsf_rx_source; /* endpoint that last fed the parser */
static uint32_t crsf_rx_dropped;

/* USB wins after DTR or four received bytes; otherwise a used UART wins.
 * Default to USB so a receive-only host does not have to send first. */
static struct crsf_ep *crsf_active(void)
{
	struct crsf_ep *const uart = &crsf_eps[CRSF_UART];
	struct crsf_ep *const usb = &crsf_eps[CRSF_USB];
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

static void crsf_isr(const struct device *dev, void *user_data)
{
	struct crsf_ep *const ep = user_data;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t chunk[32];
			const int n = uart_fifo_read(dev, chunk, sizeof(chunk));

			if (n > 0) {
				ep->rx_total += (uint32_t)n;
			}

			/* Check the active endpoint after recording: the first frame must not be lost to lag */
			if (ep != crsf_active()) {
				continue; /* not the active endpoint: incoming bytes do not count */
			}

			if (dev != crsf_rx_source) {
				/* Endpoint switch: void the partial frame instead of stitching two endpoints together */
				zlrs_crsf_parser_init(&crsf_parser);
				crsf_rx_source = dev;
			}

			for (int i = 0; i < n; i++) {
				uint8_t record[CRSF_FRAME_SLOT_LEN];
				const enum zlrs_crsf_result result =
					zlrs_crsf_feed(&crsf_parser, chunk[i]);

				if (result != ZLRS_CRSF_FRAME) {
					continue;
				}

				record[0] = crsf_parser.payload_len;
				record[1] = crsf_parser.type;
				memcpy(&record[2], ZLRS_CRSF_PAYLOAD(&crsf_parser),
				       crsf_parser.payload_len);

				const uint32_t size = (uint32_t)crsf_parser.payload_len + 2U;

				/* One ISR producer: the thread consumer can only increase free space. */
				if (ring_buf_space_get(&crsf_frame_ring) < size) {
					crsf_rx_dropped++;
					continue;
				}
				(void)ring_buf_put(&crsf_frame_ring, record, size);
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *claim = NULL;
			const uint32_t claimed = ring_buf_get_claim(&ep->tx, &claim, 64U);

			if (claimed == 0U) {
				const unsigned int key = irq_lock();

				if (ring_buf_is_empty(&ep->tx)) {
					uart_irq_tx_disable(dev);
				}
				irq_unlock(key);
				continue;
			}

			if (ep != crsf_active()) {
				uart_irq_tx_disable(
					dev); /* queued bytes wait for the endpoint to come back */
				continue;
			}

			const int sent = uart_fifo_fill(dev, claim, (int)claimed);

			ring_buf_get_finish(&ep->tx, (sent > 0) ? (uint32_t)sent : 0U);
		}
	}
}

int zlrs_link_crsf_init(void)
{
	ring_buf_init(&crsf_frame_ring, sizeof(crsf_frame_ring_mem), crsf_frame_ring_mem);
	zlrs_crsf_parser_init(&crsf_parser);

	for (size_t i = 0U; i < ARRAY_SIZE(crsf_eps); i++) {
		const struct device *const dev = crsf_eps[i].dev;

		if (dev == NULL) {
			continue; /* alias not wired on this board */
		}

		if (!device_is_ready(dev)) {
			LOG_ERR("CRSF endpoint %s not ready", dev->name);
			continue;
		}

		ring_buf_init(&crsf_eps[i].tx, sizeof(crsf_eps[i].tx_mem), crsf_eps[i].tx_mem);

		if (uart_irq_callback_user_data_set(dev, crsf_isr, &crsf_eps[i]) != 0) {
			LOG_ERR("CRSF endpoint %s: RX setup failed; endpoint disabled", dev->name);
			crsf_eps[i].ready = false;
			continue;
		}
		crsf_eps[i].ready = true;
		uart_irq_rx_enable(dev);

		LOG_INF("CRSF endpoint ready (%s)", dev->name);
	}

	if (crsf_active() == NULL) {
		LOG_WRN("no zlrs-control-uart / zlrs-control-cdc alias: CRSF port disabled");
		return -1;
	}

	return 0;
}

bool zlrs_link_crsf_ready(void)
{
	return crsf_active() != NULL;
}

uint8_t zlrs_link_crsf_write(uint8_t type, const uint8_t *payload, uint8_t payload_len)
{
	uint8_t frame[ZLRS_CRSF_FRAME_MAX_LEN];
	const uint8_t addr =
		(type == ZLRS_CRSF_TYPE_RC_CHANNELS) ? ZLRS_CRSF_ADDR_FC : ZLRS_CRSF_ADDR_TX_MODULE;
	const uint8_t len = zlrs_crsf_encode(frame, addr, type, payload, payload_len);
	struct crsf_ep *const ep = crsf_active();

	if (ep == NULL || len == 0U) {
		return 0U;
	}

	/* One thread producer: the ISR consumer can only increase free space.
	 * Drop whole frames under backpressure; a partial frame corrupts the next one's CRC. */
	if (ring_buf_space_get(&ep->tx) < len) {
		return 0U;
	}
	(void)ring_buf_put(&ep->tx, frame, len);

	uart_irq_tx_enable(ep->dev);

	return len;
}

uint8_t zlrs_link_crsf_read(uint8_t *type, uint8_t *payload, uint8_t max_len)
{
	uint8_t header[2];
	uint8_t len;

	if (ring_buf_size_get(&crsf_frame_ring) < 2U) {
		return 0U;
	}

	if (ring_buf_peek(&crsf_frame_ring, header, sizeof(header)) != sizeof(header)) {
		return 0U;
	}
	if (header[0] > max_len) {
		/* Payload buffer too small: drop the whole frame rather than misalign the buffer */
		uint8_t scratch[CRSF_FRAME_SLOT_LEN];

		(void)ring_buf_get(&crsf_frame_ring, scratch, (uint32_t)header[0] + 2U);
		return 0U;
	}
	if (ring_buf_size_get(&crsf_frame_ring) < (uint32_t)header[0] + 2U) {
		return 0U;
	}

	(void)ring_buf_get(&crsf_frame_ring, header, sizeof(header));
	len = header[0];
	*type = header[1];
	(void)ring_buf_get(&crsf_frame_ring, payload, len);

	return len;
}

uint32_t zlrs_link_crsf_rx_dropped(void)
{
	return crsf_rx_dropped;
}

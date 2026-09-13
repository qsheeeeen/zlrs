/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "serial_port.h"

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

LOG_MODULE_REGISTER(zlrs_serial, CONFIG_APP_LOG_LEVEL);

#define RX_RING_LEN 1024U
#define TX_RING_LEN 1024U

static const struct device *const uart = DEVICE_DT_GET(DT_ALIAS(zlrs_data_uart));

static uint8_t rx_ring_mem[RX_RING_LEN];
static uint8_t tx_ring_mem[TX_RING_LEN];
static struct ring_buf rx_ring;
static struct ring_buf tx_ring;
static uint32_t rx_dropped;

static void uart_isr(const struct device *dev, void *user_data)
{
	(void)user_data;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t chunk[64];
			const int n = uart_fifo_read(dev, chunk, sizeof(chunk));

			if (n > 0 && ring_buf_put(&rx_ring, chunk, (uint32_t)n) != (uint32_t)n) {
				rx_dropped++; /* 透传是 best-effort，满了就丢 */
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *claim = NULL;
			const uint32_t claimed = ring_buf_get_claim(&tx_ring, &claim, 64U);

			if (claimed == 0U) {
				uart_irq_tx_disable(dev);
				continue;
			}

			const int sent = uart_fifo_fill(dev, claim, (int)claimed);

			ring_buf_get_finish(&tx_ring, (sent > 0) ? (uint32_t)sent : 0U);
		}
	}
}

int zlrs_serial_init(void)
{
	if (!device_is_ready(uart)) {
		LOG_ERR("数据串口未就绪");
		return -1;
	}

	ring_buf_init(&rx_ring, sizeof(rx_ring_mem), rx_ring_mem);
	ring_buf_init(&tx_ring, sizeof(tx_ring_mem), tx_ring_mem);

	uart_irq_callback_user_data_set(uart, uart_isr, NULL);
	uart_irq_rx_enable(uart);

	LOG_INF("数据串口就绪（%s）", uart->name);

	return 0;
}

uint16_t zlrs_serial_read(uint8_t *buf, uint16_t len)
{
	return (uint16_t)ring_buf_get(&rx_ring, buf, len);
}

uint16_t zlrs_serial_write(const uint8_t *buf, uint16_t len)
{
	const uint32_t written = ring_buf_put(&tx_ring, buf, len);

	if (written > 0U) {
		uart_irq_tx_enable(uart);
	}

	return (uint16_t)written;
}

uint32_t zlrs_serial_dropped(void)
{
	return rx_dropped;
}

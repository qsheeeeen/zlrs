/*
 * CRSF 端口的 Zephyr 侧实现：中断收字节 → 解析器 → 帧环形缓冲，主循环取走。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "crsf_port.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include <string.h>

#include "zlrs/crsf/crsf.h"

LOG_MODULE_REGISTER(zlrs_crsf_port, CONFIG_APP_LOG_LEVEL);

/* 帧缓冲按"1 字节长度 + 1 字节类型 + 载荷"存放 */
#define FRAME_SLOT_LEN (2U + ZLRS_CRSF_PAYLOAD_MAX_LEN)
#define FRAME_RING_LEN 512U
#define TX_RING_LEN 1024U

#if DT_NODE_EXISTS(DT_ALIAS(zlrs_crsf_uart))
static const struct device *const uart = DEVICE_DT_GET(DT_ALIAS(zlrs_crsf_uart));
#define CRSF_PORT_AVAILABLE 1
#else
#define CRSF_PORT_AVAILABLE 0
#endif

static uint8_t frame_ring_mem[FRAME_RING_LEN];
static uint8_t tx_ring_mem[TX_RING_LEN];
static struct ring_buf frame_ring;
static struct ring_buf tx_ring;
static zlrs_crsf_parser_t parser;
static uint32_t rx_dropped;
static bool ready;

static void uart_isr(const struct device *dev, void *user_data)
{
	(void)user_data;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t chunk[32];
			const int n = uart_fifo_read(dev, chunk, sizeof(chunk));

			for (int i = 0; i < n; i++) {
				uint8_t record[FRAME_SLOT_LEN];
				const enum zlrs_crsf_result result =
					zlrs_crsf_feed(&parser, chunk[i]);

				if (result != ZLRS_CRSF_FRAME) {
					continue;
				}

				record[0] = parser.payload_len;
				record[1] = parser.type;
				memcpy(&record[2], ZLRS_CRSF_PAYLOAD(&parser),
				       parser.payload_len);

				if (ring_buf_put(&frame_ring, record,
						 (uint32_t)parser.payload_len + 2U) !=
				    (uint32_t)parser.payload_len + 2U) {
					rx_dropped++;
				}
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

int zlrs_crsf_port_init(void)
{
	ring_buf_init(&frame_ring, sizeof(frame_ring_mem), frame_ring_mem);
	ring_buf_init(&tx_ring, sizeof(tx_ring_mem), tx_ring_mem);
	zlrs_crsf_parser_init(&parser);

	if (!CRSF_PORT_AVAILABLE) {
		LOG_WRN("no zlrs-crsf-uart alias on this board: CRSF port disabled");
		return -1;
	}

	if (!device_is_ready(uart)) {
		LOG_ERR("CRSF port not ready (%s)", uart->name);
		return -1;
	}

	uart_irq_callback_user_data_set(uart, uart_isr, NULL);
	uart_irq_rx_enable(uart);
	ready = true;

	LOG_INF("CRSF port ready (%s)", uart->name);

	return 0;
}

bool zlrs_crsf_port_ready(void)
{
	return ready;
}

uint8_t zlrs_crsf_port_write(uint8_t type, const uint8_t *payload, uint8_t payload_len)
{
	uint8_t frame[ZLRS_CRSF_FRAME_MAX_LEN];
	const uint8_t addr = (type == ZLRS_CRSF_TYPE_RC_CHANNELS)
				     ? ZLRS_CRSF_ADDR_FC
				     : ZLRS_CRSF_ADDR_TX_MODULE;
	const uint8_t len = zlrs_crsf_encode(frame, addr, type, payload, payload_len);

	if (!ready || len == 0U) {
		return 0U;
	}

	if (ring_buf_put(&tx_ring, frame, len) != len) {
		return 0U; /* 发送缓冲满：CRSF 是尽力而为，丢帧并计数交给调用方 */
	}

	uart_irq_tx_enable(uart);

	return len;
}

uint8_t zlrs_crsf_port_read(uint8_t *type, uint8_t *payload, uint8_t max_len)
{
	uint8_t header[2];
	uint8_t len;

	if (ring_buf_size_get(&frame_ring) < 2U) {
		return 0U;
	}

	if (ring_buf_peek(&frame_ring, header, sizeof(header)) != sizeof(header)) {
		return 0U;
	}
	if (header[0] > max_len) {
		/* 载荷缓冲区太小：整帧丢弃，避免缓冲错位 */
		uint8_t scratch[FRAME_SLOT_LEN];

		(void)ring_buf_get(&frame_ring, scratch, (uint32_t)header[0] + 2U);
		return 0U;
	}
	if (ring_buf_size_get(&frame_ring) < (uint32_t)header[0] + 2U) {
		return 0U;
	}

	(void)ring_buf_get(&frame_ring, header, sizeof(header));
	len = header[0];
	*type = header[1];
	(void)ring_buf_get(&frame_ring, payload, len);

	return len;
}

uint32_t zlrs_crsf_port_rx_dropped(void)
{
	return rx_dropped;
}

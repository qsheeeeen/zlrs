/*
 * Management port, see mgmt/port.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mgmt/port.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include <stdbool.h>
#include <stdint.h>

#include "platform/caps.h"
#include "platform/recovery.h"
#include "zlrs/mgmt/mgmt.h"

LOG_MODULE_REGISTER(zlrs_mgmt_port, CONFIG_APP_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZLRS_MGMT)

#define CFG_PORT_RX_RING_LEN 512U
#define CFG_PORT_TX_RING_LEN 512U

struct cfg_port {
	const struct device *dev;
	struct ring_buf rx;
	struct ring_buf tx;
	uint8_t rx_mem[CFG_PORT_RX_RING_LEN];
	uint8_t tx_mem[CFG_PORT_TX_RING_LEN];
	uint32_t rx_dropped;
	bool ready;
};

static struct cfg_port cfg_port;

static void cfg_port_isr(const struct device *dev, void *user_data)
{
	struct cfg_port *const p = user_data;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t chunk[32];
			const int n = uart_fifo_read(dev, chunk, sizeof(chunk));

			if (n > 0 && ring_buf_put(&p->rx, chunk, (uint32_t)n) != (uint32_t)n) {
				p->rx_dropped++;
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *claim = NULL;
			const uint32_t claimed = ring_buf_get_claim(&p->tx, &claim, 64U);

			if (claimed == 0U) {
				uart_irq_tx_disable(dev);
				continue;
			}

			const int sent = uart_fifo_fill(dev, claim, (int)claimed);

			ring_buf_get_finish(&p->tx, (sent > 0) ? (uint32_t)sent : 0U);
		}
	}
}

int zlrs_mgmt_port_init(void)
{
#if ZLRS_PORT_MGMT
	const struct device *const dev = DEVICE_DT_GET(DT_ALIAS(zlrs_mgmt_cdc));

	if (!device_is_ready(dev)) {
		LOG_ERR("management port %s not ready", dev->name);
		return -1;
	}

	ring_buf_init(&cfg_port.rx, sizeof(cfg_port.rx_mem), cfg_port.rx_mem);
	ring_buf_init(&cfg_port.tx, sizeof(cfg_port.tx_mem), cfg_port.tx_mem);
	cfg_port.dev = dev;

	/* Check it: without IRQ support it is -ENOSYS/-ENOTSUP, and ignoring that loses commands */
	if (uart_irq_callback_user_data_set(dev, cfg_port_isr, &cfg_port) != 0) {
		LOG_ERR("management port %s: RX setup failed", dev->name);
		cfg_port.dev = NULL;
		return -1;
	}

	uart_irq_rx_enable(dev);
	cfg_port.ready = true;

	LOG_INF("management port ready (%s)", dev->name);

	return 0;
#else
	LOG_WRN("no zlrs-mgmt-cdc alias: management port disabled");

	return -1;
#endif
}

bool zlrs_mgmt_port_ready(void)
{
	return cfg_port.ready;
}

uint16_t zlrs_mgmt_port_read(uint8_t *buf, uint16_t len)
{
	if (!cfg_port.ready) {
		return 0U;
	}

	return (uint16_t)ring_buf_get(&cfg_port.rx, buf, len);
}

uint16_t zlrs_mgmt_port_write(const uint8_t *buf, uint16_t len)
{
	uint32_t written;

	if (!cfg_port.ready) {
		return 0U;
	}

	written = ring_buf_put(&cfg_port.tx, buf, len);
	if (written > 0U) {
		uart_irq_tx_enable(cfg_port.dev);
	}

	return (uint16_t)written;
}

bool zlrs_mgmt_port_poll_enter(void)
{
	static zlrs_mgmt_parser_t parser;
	static bool parser_ready;
	uint8_t chunk[ZLRS_MGMT_CHUNK];
	const uint16_t got = zlrs_mgmt_port_read(chunk, sizeof(chunk));
	bool enter = false;

	if (got == 0U) {
		return false;
	}

	if (!parser_ready) {
		zlrs_mgmt_parser_init(&parser);
		parser_ready = true;
	}

	for (uint16_t i = 0U; i < got; i++) {
		if (zlrs_mgmt_feed(&parser, chunk[i]) != 1) {
			continue;
		}

		const uint8_t cmd = parser.cmd;
		uint8_t reply[ZLRS_MGMT_FRAME_MAX_LEN];
		uint8_t payload[2];
		uint8_t len;

		if (cmd == ZLRS_MGMT_CMD_ENTER) {
			enter = true;
		}

		/* Recovery needs no setting up: reboot once acknowledged, the main loop never returns */
		if (cmd == ZLRS_MGMT_CMD_RECOVERY && zlrs_platform_recovery_enter() == 0) {
			payload[0] = cmd;
			payload[1] = ZLRS_MGMT_OK;
			len = zlrs_mgmt_encode(reply, sizeof(reply), ZLRS_MGMT_CMD_ACK, payload,
					      sizeof(payload));
			(void)zlrs_mgmt_port_write(reply, len);
		}

		/* Outside a session everything answers "wrong state"; EXIT is a no-op */
		payload[0] = cmd;
		payload[1] = (cmd == ZLRS_MGMT_CMD_ENTER || cmd == ZLRS_MGMT_CMD_EXIT)
				     ? ZLRS_MGMT_OK
				     : ZLRS_MGMT_E_STATE;

		len = zlrs_mgmt_encode(reply, sizeof(reply), ZLRS_MGMT_CMD_ACK, payload,
				      sizeof(payload));
		(void)zlrs_mgmt_port_write(reply, len);
	}

	return enter;
}

#endif /* CONFIG_ZLRS_MGMT */

/*
 * Port layer, see ports.h: endpoint choice, data port, CRSF port, config mode.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ports.h"

#include <zephyr/app_version.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/ring_buffer.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "led_port.h"
#include "store.h"
#include "watchdog.h"
#include "zlrs/config/config.h"
#include "zlrs/crsf/crsf.h"
#include "zlrs/link/link.h"

LOG_MODULE_REGISTER(zlrs_ports, CONFIG_APP_LOG_LEVEL);

/* ===== 1. endpoint choice ================================================= */

/* A logical port has at most two endpoints: UART + USB CDC */
#define PORT_MAX 2

struct port_pair {
	const struct device *uart; /* NULL = this alias is not wired on the board */
	const struct device *cdc;
};

static const struct port_pair crsf_pair = {
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_crsf_uart))
	.uart = DEVICE_DT_GET(DT_ALIAS(zlrs_crsf_uart)),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_crsf_cdc))
	.cdc = DEVICE_DT_GET(DT_ALIAS(zlrs_crsf_cdc)),
#endif
};

static const struct port_pair data_pair = {
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_data_uart))
	.uart = DEVICE_DT_GET(DT_ALIAS(zlrs_data_uart)),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_data_cdc))
	.cdc = DEVICE_DT_GET(DT_ALIAS(zlrs_data_cdc)),
#endif
};

/* Endpoint order is fixed: 0 = UART, 1 = USB CDC */
static const struct device *port_ep_dev(const struct port_pair *pair, size_t idx)
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
#define HOST_SEEN_MAX PORT_MAX * 2
static const struct device *host_seen_devs[HOST_SEEN_MAX];

static void host_seen_mark(const struct device *dev)
{
	for (size_t i = 0U; i < ARRAY_SIZE(host_seen_devs); i++) {
		if (host_seen_devs[i] == dev) {
			return;
		}
		if (host_seen_devs[i] == NULL) {
			host_seen_devs[i] = dev;
			return;
		}
	}
}

static bool host_seen_any(const struct device *dev)
{
	for (size_t i = 0U; i < ARRAY_SIZE(host_seen_devs); i++) {
		if (host_seen_devs[i] == dev) {
			return true;
		}
	}

	return false;
}

/* With both present USB wins while it is in use; NULL when there is neither */
static const struct device *port_active(const struct port_pair *pair)
{
	const bool uart_ok = (pair->uart != NULL) && device_is_ready(pair->uart);
	const bool cdc_ok = (pair->cdc != NULL) && device_is_ready(pair->cdc);

	if (uart_ok && cdc_ok) {
		const bool cdc_in_use = cdc_opened(pair->cdc) || host_seen_any(pair->cdc);

		return cdc_in_use ? pair->cdc : pair->uart;
	}

	if (uart_ok) {
		return pair->uart;
	}

	return cdc_ok ? pair->cdc : NULL;
}

/* ===== 2. data port: passthrough ========================================== */

#define DATA_RX_RING_LEN 1024U
#define DATA_TX_RING_LEN 1024U

/* One buffer set per endpoint; dev == NULL means the alias is not wired */
struct data_ep {
	const struct device *dev;
	struct ring_buf rx;
	struct ring_buf tx;
	uint8_t rx_mem[DATA_RX_RING_LEN];
	uint8_t tx_mem[DATA_TX_RING_LEN];
	uint32_t rx_dropped;
	uint32_t rx_total; /* bytes received (one of the endpoint-choice inputs) */
	bool ready;
};

static struct data_ep data_eps[PORT_MAX];
static uint32_t data_ready_ports;

static struct data_ep *data_ep_of(const struct device *dev)
{
	for (size_t i = 0U; i < PORT_MAX; i++) {
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
				/* Record the evidence first: endpoint choice depends on it, see port_active() */
				host_seen_mark(dev);
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
				uart_irq_tx_disable(dev);
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

void zlrs_data_port_discard_rx(void)
{
	for (size_t i = 0U; i < PORT_MAX; i++) {
		if (data_eps[i].ready) {
			data_ep_drop_rx(&data_eps[i]);
		}
	}
}

size_t zlrs_data_port_state(zlrs_port_state_t *out, size_t count)
{
	const struct device *const active = port_active(&data_pair);
	size_t written = 0U;

	for (size_t i = 0U; (i < PORT_MAX) && (written < count); i++) {
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

int zlrs_data_port_init(uint32_t baud)
{
	for (size_t i = 0U; i < PORT_MAX; i++) {
		const struct device *const dev = port_ep_dev(&data_pair, i);

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

uint16_t zlrs_data_port_read(uint8_t *buf, uint16_t len)
{
	const struct device *const active = port_active(&data_pair);
	uint16_t got = 0U;

	for (size_t i = 0U; i < PORT_MAX; i++) {
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

uint16_t zlrs_data_port_write(const uint8_t *buf, uint16_t len)
{
	struct data_ep *const ep = data_ep_of(port_active(&data_pair));

	if (ep == NULL) {
		return 0U;
	}

	const uint32_t written = ring_buf_put(&ep->tx, buf, len);

	if (written > 0U) {
		/* tx_enable returns void; a failed send only shows up as a peer timeout or retransmit */
		uart_irq_tx_enable(ep->dev);
	}

	return (uint16_t)written;
}

uint32_t zlrs_data_port_dropped(void)
{
	uint32_t dropped = 0U;

	for (size_t i = 0U; i < PORT_MAX; i++) {
		dropped += data_eps[i].rx_dropped;
	}

	return dropped;
}

/* ===== 3. CRSF port ======================================================= */

/* Frame records are stored as 1 byte length + 1 byte type + payload */
#define CRSF_FRAME_SLOT_LEN (2U + ZLRS_CRSF_PAYLOAD_MAX_LEN)
#define CRSF_FRAME_RING_LEN 512U
#define CRSF_TX_RING_LEN    1024U

/* One TX buffer per endpoint; dev == NULL means the alias is not wired */
struct crsf_ep {
	const struct device *dev;
	struct ring_buf tx;
	uint8_t tx_mem[CRSF_TX_RING_LEN];
	bool ready;
};

static uint8_t crsf_frame_ring_mem[CRSF_FRAME_RING_LEN];
static struct ring_buf crsf_frame_ring;
static struct crsf_ep crsf_eps[PORT_MAX];
static zlrs_crsf_parser_t crsf_parser;
static const struct device *crsf_rx_source; /* endpoint that last fed the parser */
static uint32_t crsf_rx_dropped;
static bool crsf_ready;

static struct crsf_ep *crsf_ep_of(const struct device *dev)
{
	for (size_t i = 0U; i < PORT_MAX; i++) {
		if (crsf_eps[i].ready && crsf_eps[i].dev == dev) {
			return &crsf_eps[i];
		}
	}

	return NULL;
}

static void crsf_isr(const struct device *dev, void *user_data)
{
	struct crsf_ep *const ep = user_data;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t chunk[32];
			const int n = uart_fifo_read(dev, chunk, sizeof(chunk));

			if (n > 0) {
				/* Record the evidence first: endpoint choice depends on it, see port_active() */
				host_seen_mark(dev);
			}

			/* Check the active endpoint after recording: the first frame must not be lost to lag */
			if (dev != port_active(&crsf_pair)) {
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

				if (ring_buf_put(&crsf_frame_ring, record,
						 (uint32_t)crsf_parser.payload_len + 2U) !=
				    (uint32_t)crsf_parser.payload_len + 2U) {
					crsf_rx_dropped++;
				}
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *claim = NULL;
			const uint32_t claimed = ring_buf_get_claim(&ep->tx, &claim, 64U);

			if (claimed == 0U) {
				uart_irq_tx_disable(dev);
				continue;
			}

			if (dev != port_active(&crsf_pair)) {
				uart_irq_tx_disable(
					dev); /* queued bytes wait for the endpoint to come back */
				continue;
			}

			const int sent = uart_fifo_fill(dev, claim, (int)claimed);

			ring_buf_get_finish(&ep->tx, (sent > 0) ? (uint32_t)sent : 0U);
		}
	}
}

int zlrs_crsf_port_init(void)
{
	ring_buf_init(&crsf_frame_ring, sizeof(crsf_frame_ring_mem), crsf_frame_ring_mem);
	zlrs_crsf_parser_init(&crsf_parser);

	for (size_t i = 0U; i < PORT_MAX; i++) {
		const struct device *const dev = port_ep_dev(&crsf_pair, i);

		if (dev == NULL) {
			continue; /* alias not wired on this board */
		}

		if (!device_is_ready(dev)) {
			LOG_ERR("CRSF endpoint %s not ready", dev->name);
			continue;
		}

		ring_buf_init(&crsf_eps[i].tx, sizeof(crsf_eps[i].tx_mem), crsf_eps[i].tx_mem);
		crsf_eps[i].dev = dev;
		crsf_eps[i].ready = true;
		crsf_ready = true;

		if (uart_irq_callback_user_data_set(dev, crsf_isr, &crsf_eps[i]) != 0) {
			LOG_ERR("CRSF endpoint %s: RX setup failed; endpoint disabled", dev->name);
			crsf_eps[i].ready = false;
			continue;
		}
		uart_irq_rx_enable(dev);

		LOG_INF("CRSF endpoint ready (%s)", dev->name);
	}

	if (!crsf_ready) {
		LOG_WRN("no zlrs-crsf-uart / zlrs-crsf-cdc alias: CRSF port disabled");
		return -1;
	}

	return 0;
}

bool zlrs_crsf_port_ready(void)
{
	return crsf_ready;
}

uint8_t zlrs_crsf_port_write(uint8_t type, const uint8_t *payload, uint8_t payload_len)
{
	uint8_t frame[ZLRS_CRSF_FRAME_MAX_LEN];
	const uint8_t addr =
		(type == ZLRS_CRSF_TYPE_RC_CHANNELS) ? ZLRS_CRSF_ADDR_FC : ZLRS_CRSF_ADDR_TX_MODULE;
	const uint8_t len = zlrs_crsf_encode(frame, addr, type, payload, payload_len);
	struct crsf_ep *const ep = crsf_ep_of(port_active(&crsf_pair));

	if (!crsf_ready || ep == NULL || len == 0U) {
		return 0U;
	}

	if (ring_buf_put(&ep->tx, frame, len) != len) {
		return 0U; /* TX buffer full: CRSF is best-effort, the caller counts the drop */
	}

	uart_irq_tx_enable(ep->dev);

	return len;
}

uint8_t zlrs_crsf_port_read(uint8_t *type, uint8_t *payload, uint8_t max_len)
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

uint32_t zlrs_crsf_port_rx_dropped(void)
{
	return crsf_rx_dropped;
}

/* ===== 4. config mode ===================================================== */

#if IS_ENABLED(CONFIG_ZLRS_CONFIG)

#define CFG_RX_CHUNK     32U
#define CFG_EXIT_POLL_MS 50U

/*
 * How long the white LED has to show before this counts as "really configuring". The update
 * tool and `zlrs-config.py` open a session per command and leave within ~100 ms; flashing
 * white for that looks like a fault, so the pattern only switches once a session lasts.
 */
#define CFG_LED_DELAY_MS 500U

/* ---- config port: a dedicated CDC that is always on USB ------------------ */

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
static const zlrs_config_ops_t *cfg_ops;
/* The host sent its confirm command (touched from the main thread only) */
static bool cfg_image_confirmed;

void zlrs_config_set_ops(const zlrs_config_ops_t *ops)
{
	cfg_ops = ops;
}

bool zlrs_config_image_confirmed(void)
{
	return cfg_image_confirmed;
}

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

int zlrs_config_port_init(void)
{
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_config_cdc))
	const struct device *const dev = DEVICE_DT_GET(DT_ALIAS(zlrs_config_cdc));

	if (!device_is_ready(dev)) {
		LOG_ERR("config port %s not ready", dev->name);
		return -1;
	}

	ring_buf_init(&cfg_port.rx, sizeof(cfg_port.rx_mem), cfg_port.rx_mem);
	ring_buf_init(&cfg_port.tx, sizeof(cfg_port.tx_mem), cfg_port.tx_mem);
	cfg_port.dev = dev;

	/* Check it: without IRQ support it is -ENOSYS/-ENOTSUP, and ignoring that loses commands */
	if (uart_irq_callback_user_data_set(dev, cfg_port_isr, &cfg_port) != 0) {
		LOG_ERR("config port %s: RX setup failed", dev->name);
		cfg_port.dev = NULL;
		return -1;
	}

	uart_irq_rx_enable(dev);
	cfg_port.ready = true;

	LOG_INF("config port ready (%s)", dev->name);

	return 0;
#else
	LOG_WRN("no zlrs-config-cdc alias: config port disabled");

	return -1;
#endif
}

bool zlrs_config_port_ready(void)
{
	return cfg_port.ready;
}

static uint16_t cfg_port_read(uint8_t *buf, uint16_t len)
{
	if (!cfg_port.ready) {
		return 0U;
	}

	return (uint16_t)ring_buf_get(&cfg_port.rx, buf, len);
}

static uint16_t cfg_port_write(const uint8_t *buf, uint16_t len)
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

bool zlrs_config_port_poll_enter(void)
{
	static zlrs_cfg_parser_t parser;
	static bool parser_ready;
	uint8_t chunk[CFG_RX_CHUNK];
	const uint16_t got = cfg_port_read(chunk, sizeof(chunk));

	if (got == 0U) {
		return false;
	}

	if (!parser_ready) {
		zlrs_cfg_parser_init(&parser);
		parser_ready = true;
	}

	bool enter = false;

	for (uint16_t i = 0U; i < got; i++) {
		if (zlrs_cfg_feed(&parser, chunk[i]) != 1) {
			continue;
		}

		const uint8_t cmd = parser.cmd;
		uint8_t reply[ZLRS_CFG_FRAME_MAX_LEN];
		uint8_t payload[2];
		uint8_t len;

		if (cmd == ZLRS_CFG_CMD_ENTER) {
			enter = true;
		}

		/* Recovery needs no configuration: reboot once acknowledged, the main loop never returns */
		if (cmd == ZLRS_CFG_CMD_RECOVERY && cfg_ops != NULL &&
		    cfg_ops->enter_recovery != NULL) {
			payload[0] = cmd;
			payload[1] = ZLRS_CFG_OK;
			len = zlrs_cfg_encode(reply, sizeof(reply), ZLRS_CFG_CMD_ACK, payload,
					      sizeof(payload));
			(void)cfg_port_write(reply, len);
			(void)cfg_ops->enter_recovery();
		}

		/* Outside a session everything answers "wrong state"; EXIT is a no-op */
		payload[0] = cmd;
		payload[1] = (cmd == ZLRS_CFG_CMD_ENTER || cmd == ZLRS_CFG_CMD_EXIT)
				     ? ZLRS_CFG_OK
				     : ZLRS_CFG_E_STATE;

		len = zlrs_cfg_encode(reply, sizeof(reply), ZLRS_CFG_CMD_ACK, payload,
				      sizeof(payload));
		(void)cfg_port_write(reply, len);
	}

	return enter;
}

/* ---- session ------------------------------------------------------------- */

static zlrs_cfg_parser_t cfg_parser;
static uint8_t cfg_tx_line[ZLRS_CFG_FRAME_MAX_LEN];
static zlrs_link_t *cfg_link; /* the link config mode acts on (the control link) */
static uint8_t *cfg_failsafe_out;
/* ---- backend: land commands in NVS and the link layer --------------------- */

static int cfg_save_root_key(void *ctx, const uint8_t key[ZLRS_KEY_LEN])
{
	(void)ctx;

	if (zlrs_store_save_root_key(key) != 0) {
		return ZLRS_CFG_E_STORE;
	}

	/* Takes effect immediately, no reboot needed */
	zlrs_link_set_root_key(cfg_link, key, k_uptime_get_32());

	LOG_INF("root key written");

	return ZLRS_CFG_OK;
}

static int cfg_read_status(void *ctx, zlrs_cfg_status_t *out)
{
	(void)ctx;

	out->version = ZLRS_CFG_VERSION;
	out->role = cfg_link->cfg.role;
	out->state = cfg_link->state;
	out->flags = 0U;
	if (cfg_link->configured) {
		out->flags |= ZLRS_CFG_FLAG_KEY;
	}
	if (cfg_link->paired) {
		out->flags |= ZLRS_CFG_FLAG_PAIRED;
		memcpy(out->peer_id, cfg_link->peer_id, ZLRS_DEV_ID_LEN);
	}
	if (cfg_link->session_valid) {
		out->flags |= ZLRS_CFG_FLAG_SESSION;
	}
	memcpy(out->dev_id, cfg_link->dev_id, ZLRS_DEV_ID_LEN);
	out->lq_percent = cfg_link->stats.lq_percent;
	out->band = ZLRS_SUBG_BAND;
	{
		/* Report the stored value first (just written, not yet live), the runtime one otherwise */
		uint8_t stored_region;
		int8_t stored_power;

		out->region =
			(zlrs_store_load_region(&stored_region) == (int)sizeof(stored_region))
				? stored_region
				: ((cfg_ops != NULL && cfg_ops->region != NULL) ? cfg_ops->region()
										: 0U);
		out->tx_power_dbm =
			(zlrs_store_load_power(&stored_power) == (int)sizeof(stored_power))
				? stored_power
				: ((cfg_ops != NULL && cfg_ops->power != NULL) ? cfg_ops->power()
									       : 0);
	}
	{
		uint32_t baud;

		if (zlrs_store_load_baud(&baud) != (int)sizeof(baud)) {
			baud = 0U;
		}
		out->data_baud = baud;
	}

	return ZLRS_CFG_OK;
}

static int cfg_save_failsafe(void *ctx, const uint8_t channels[ZLRS_CFG_FAILSAFE_LEN])
{
	(void)ctx;

	if (zlrs_store_save_failsafe(channels) != 0) {
		return ZLRS_CFG_E_STORE;
	}
	if (cfg_failsafe_out != NULL) {
		memcpy(cfg_failsafe_out, channels, ZLRS_CFG_FAILSAFE_LEN);
	}
	/* The new values travel to the peer with the next pairing */
	zlrs_link_set_failsafe(cfg_link, channels, ZLRS_CFG_FAILSAFE_LEN);

	LOG_INF("failsafe values written");

	return ZLRS_CFG_OK;
}

static int cfg_save_baud(void *ctx, uint32_t baud)
{
	(void)ctx;

	if (baud < 9600U || baud > 4000000U) {
		return ZLRS_CFG_E_PARAM;
	}
	if (zlrs_store_save_baud(baud) != 0) {
		return ZLRS_CFG_E_STORE;
	}

	LOG_INF("data port baud set to %u (takes effect on next boot)", (unsigned int)baud);

	return ZLRS_CFG_OK;
}

static int cfg_pair(void *ctx)
{
	(void)ctx;

	zlrs_link_press(cfg_link, k_uptime_get_32());
	LOG_INF("pairing triggered by config tool");

	return ZLRS_CFG_OK;
}

static int cfg_save_region(void *ctx, uint8_t region)
{
	(void)ctx;

	if (cfg_ops == NULL || cfg_ops->save_region == NULL) {
		return ZLRS_CFG_E_STATE;
	}

	return cfg_ops->save_region(region);
}

static int cfg_save_power(void *ctx, int8_t dbm)
{
	(void)ctx;

	if (cfg_ops == NULL || cfg_ops->save_power == NULL) {
		return ZLRS_CFG_E_STATE;
	}

	return cfg_ops->save_power(dbm);
}

static int cfg_erase_pair(void *ctx)
{
	(void)ctx;

	if (cfg_ops == NULL || cfg_ops->erase_pair == NULL) {
		return ZLRS_CFG_E_STATE;
	}

	return cfg_ops->erase_pair();
}

static int cfg_factory_reset(void *ctx)
{
	(void)ctx;

	if (cfg_ops == NULL || cfg_ops->factory_reset == NULL) {
		return ZLRS_CFG_E_STATE;
	}

	return cfg_ops->factory_reset();
}

static int cfg_enter_recovery(void *ctx)
{
	(void)ctx;

	if (cfg_ops == NULL || cfg_ops->enter_recovery == NULL) {
		return ZLRS_CFG_E_STATE;
	}

	return cfg_ops->enter_recovery();
}

/* Host confirms this upgrade; the whole flow depends on it (see doc/08-firmware.md 6) */
static int cfg_confirm_image(void *ctx)
{
	(void)ctx;

	cfg_image_confirmed = true;
	LOG_INF("host confirmed this build");

	return ZLRS_CFG_OK;
}

/* Hand the build string to the host so it can check which version is running */
static int cfg_read_build(void *ctx, char *out, size_t out_len)
{
	(void)ctx;

	if (out == NULL || out_len == 0U) {
		return ZLRS_CFG_E_PARAM;
	}

	(void)snprintf(out, out_len, "%s", APP_VERSION_STRING);

	return ZLRS_CFG_OK;
}

/*
 * MCUboot hash of the running image: read IMAGE_TLV_SHA256 from this image's TLV area.
 * The update tool tells "the image I just sent" from "already rolled back": build strings
 * can match, hashes cannot. Only MCUboot's public format is used, no internal headers.
 */
static int cfg_read_image_hash(void *ctx, uint8_t out[ZLRS_CFG_IMAGE_HASH_LEN])
{
	(void)ctx;

#ifdef CONFIG_BOOTLOADER_MCUBOOT
#define MCUBOOT_IMAGE_MAGIC    0x96f3b83dU
#define MCUBOOT_TLV_INFO_MAGIC 0x6907U
#define MCUBOOT_TLV_SHA256     0x10U
	struct {
		uint32_t magic;
		uint32_t load_addr;
		uint16_t hdr_size;
		uint16_t protect_tlv_size;
		uint32_t img_size;
	} __packed hdr;
	const struct flash_area *fa;
	uint32_t off, tlv_end;

	if (flash_area_open(PARTITION_ID(slot0_partition), &fa) != 0) {
		return ZLRS_CFG_E_STATE;
	}

	if (flash_area_read(fa, 0U, &hdr, sizeof(hdr)) != 0 || hdr.magic != MCUBOOT_IMAGE_MAGIC) {
		flash_area_close(fa);
		return ZLRS_CFG_E_STATE;
	}

	/* The TLV area starts with an info header: magic(0x6907) + length (including these 4 bytes) */
	off = (uint32_t)hdr.hdr_size + hdr.img_size + hdr.protect_tlv_size;

	{
		uint8_t info[4];
		uint16_t magic;
		uint16_t total;

		if (flash_area_read(fa, off, info, sizeof(info)) != 0) {
			flash_area_close(fa);
			return ZLRS_CFG_E_STATE;
		}

		magic = (uint16_t)info[0] | ((uint16_t)info[1] << 8);
		total = (uint16_t)info[2] | ((uint16_t)info[3] << 8);

		if (magic != MCUBOOT_TLV_INFO_MAGIC) {
			flash_area_close(fa);
			return ZLRS_CFG_E_STATE;
		}

		tlv_end = off + total;
		off += 4U;
	}

	while (off + 4U + ZLRS_CFG_IMAGE_HASH_LEN <= tlv_end) {
		uint8_t tlv[4];
		uint16_t len;
		uint8_t type;

		if (flash_area_read(fa, off, tlv, sizeof(tlv)) != 0) {
			break;
		}
		type = tlv[0];
		len = (uint16_t)tlv[2] | ((uint16_t)tlv[3] << 8);

		if (type == MCUBOOT_TLV_SHA256 && len == ZLRS_CFG_IMAGE_HASH_LEN) {
			const int rc = flash_area_read(fa, off + 4U, out, ZLRS_CFG_IMAGE_HASH_LEN);

			flash_area_close(fa);
			return (rc == 0) ? ZLRS_CFG_OK : ZLRS_CFG_E_STATE;
		}

		/* 0xFF padding follows the last TLV; type=0xFF means stop */
		if (type == 0xFFU) {
			break;
		}
		off += 4U + len;
	}

	flash_area_close(fa);
#endif

	return ZLRS_CFG_E_STATE;
}

static const zlrs_cfg_backend_t cfg_backend = {
	.save_root_key = cfg_save_root_key,
	.read_status = cfg_read_status,
	.save_failsafe = cfg_save_failsafe,
	.save_baud = cfg_save_baud,
	.save_region = cfg_save_region,
	.save_power = cfg_save_power,
	.pair = cfg_pair,
	.erase_pair = cfg_erase_pair,
	.factory_reset = cfg_factory_reset,
	.enter_recovery = cfg_enter_recovery,
	.confirm_image = cfg_confirm_image,
	.read_build = cfg_read_build,
	.read_image_hash = cfg_read_image_hash,
};

/* ---- session ------------------------------------------------------------- */

int zlrs_config_mode_run(zlrs_link_t *control, uint8_t *failsafe)
{
	const uint32_t timeout_ms = (uint32_t)CONFIG_ZLRS_APP_CONFIG_TIMEOUT_S * 1000U;
	uint32_t last_activity_ms = k_uptime_get_32();
	const uint32_t started_ms = last_activity_ms;
	bool exit_requested = false;

	cfg_link = control;
	cfg_failsafe_out = failsafe;

	if (!cfg_port.ready) {
		LOG_WRN("no config port: config mode unavailable");
		return -1;
	}

	/* No superframe to race here, so keep the logic as plain as possible */
	zlrs_cfg_parser_init(&cfg_parser);

	LOG_INF("config mode: %u s idle timeout, exit on EXIT command",
		(unsigned int)CONFIG_ZLRS_APP_CONFIG_TIMEOUT_S);

	/* Disarm the watchdog for the session: writing a phrase runs 100000 PBKDF2 rounds (tens of
	 * seconds) and an 8 s watchdog would reset mid-way. Config is attended; re-arm on exit. */
	zlrs_watchdog_disarm();

	while (!exit_requested && (uint32_t)(k_uptime_get_32() - last_activity_ms) < timeout_ms) {
		uint8_t chunk[CFG_RX_CHUNK];
		bool any = false;
		zlrs_led_input_t led_in;
		const uint32_t now_ms = k_uptime_get_32();

		zlrs_watchdog_feed();

		/* Slow white = not in the normal working state */
		memset(&led_in, 0, sizeof(led_in));
		led_in.config_mode = (uint32_t)(now_ms - started_ms) >= CFG_LED_DELAY_MS;
		led_in.configured = cfg_link->configured;
		led_in.paired = cfg_link->paired;
		led_in.role_tx = IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX);
		led_in.link_state = cfg_link->state;
		led_in.lq_percent = cfg_link->stats.lq_percent;
		led_in.uptime_ms = now_ms;
		zlrs_led_port_update(&led_in, led_in.uptime_ms);

		/* Radio stopped in config mode: data port bytes are dropped and counted */
		zlrs_data_port_discard_rx();

		const uint16_t got = cfg_port_read(chunk, sizeof(chunk));
		for (uint16_t i = 0U; i < got; i++) {
			any = true;

			const int result = zlrs_cfg_feed(&cfg_parser, chunk[i]);

			if (result != 1) {
				continue;
			}

			last_activity_ms = k_uptime_get_32();

			if (cfg_parser.cmd == ZLRS_CFG_CMD_EXIT) {
				exit_requested = true;
			}

			const uint8_t len = zlrs_cfg_dispatch(
				&cfg_backend, cfg_parser.cmd, ZLRS_CFG_PAYLOAD(&cfg_parser),
				cfg_parser.payload_len, cfg_tx_line, sizeof(cfg_tx_line));

			if (len > 0U) {
				(void)cfg_port_write(cfg_tx_line, len);
			}
		}

		if (!any) {
			k_msleep(CFG_EXIT_POLL_MS);
		}
	}

	LOG_INF("config mode finished");

	zlrs_watchdog_arm();

	return exit_requested ? 0 : 1;
}

#else /* !CONFIG_ZLRS_CONFIG */

/* Config protocol compiled out: the application still calls in, this says "no config mode" */
int zlrs_config_mode_run(zlrs_link_t *control, uint8_t *failsafe)
{
	(void)control;
	(void)failsafe;

	return -1;
}

#endif /* CONFIG_ZLRS_CONFIG */

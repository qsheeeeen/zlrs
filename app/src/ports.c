/*
 * 端口层实现，见 ports.h：端点选择 → 数据口 → CRSF 端口 → 配置模式。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ports.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "board.h"
#include "led_port.h"
#include "store.h"
#include "watchdog.h"
#include "zlrs/config/config.h"
#include "zlrs/crsf/crsf.h"
#include "zlrs/link/link.h"

LOG_MODULE_REGISTER(zlrs_ports, CONFIG_APP_LOG_LEVEL);

/* ===== 1. 端点选择 ======================================================= */

/* 一个逻辑端口最多两块端点：UART + USB CDC */
#define PORT_MAX 2

struct port_pair {
	const struct device *uart; /* NULL = 板上没挂这条别名 */
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

/* 端点顺序固定：0 = UART，1 = USB CDC */
static const struct device *port_ep_dev(const struct port_pair *pair, size_t idx)
{
	return (idx == 0U) ? pair->uart : pair->cdc;
}

/* "USB 连上"取主机真正打开 CDC 那一刻：枚举完没人读算不上在用 */
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
 * "主机在用这块端点"的证据。标准做法是 CDC 的 DTR，但 USB/IP 转发等路径上
 * SET_CONTROL_LINE_STATE 不一定送到设备，只认 DTR 会导致端点永远选不中（收到的
 * 字节被当"非当前端点"丢掉）。所以再认一条：**收到过主机字节**——ISR 在判断当前
 * 端点之前先记这笔（见 data_isr / crsf_isr），第一帧不会被选择滞后吃掉。
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

/* 两块都在时 USB 在用就选 USB；都没有返回 NULL */
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

/* ===== 2. 数据口：透传 =================================================== */

#define DATA_RX_RING_LEN 1024U
#define DATA_TX_RING_LEN 1024U

/* 一块端点一套缓冲；dev == NULL 表示板上没挂这条别名 */
struct data_ep {
	const struct device *dev;
	struct ring_buf rx;
	struct ring_buf tx;
	uint8_t rx_mem[DATA_RX_RING_LEN];
	uint8_t tx_mem[DATA_TX_RING_LEN];
	uint32_t rx_dropped;
	uint32_t rx_total; /* 收到过多少字节（端点二选一的证据之一） */
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
				/* 先记证据：端点选择的判据，见 port_active() */
				host_seen_mark(dev);
				ep->rx_total += (uint32_t)n;
			}

			if (n > 0 &&
			    ring_buf_put(&ep->rx, chunk, (uint32_t)n) != (uint32_t)n) {
				ep->rx_dropped++; /* 透传是 best-effort，满了就丢 */
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

/* 不是当前端点的进账直接丢掉：留着的话，切回来会送出陈旧的字节 */
static void data_ep_drop_rx(struct data_ep *const ep)
{
	uint8_t scratch[32];
	uint32_t n;

	while ((n = ring_buf_get(&ep->rx, scratch, sizeof(scratch))) > 0U) {
		ep->rx_dropped += n;
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
			continue; /* 板上没挂这条别名 */
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

		ring_buf_init(&data_eps[i].rx, sizeof(data_eps[i].rx_mem),
			      data_eps[i].rx_mem);
		ring_buf_init(&data_eps[i].tx, sizeof(data_eps[i].tx_mem),
			      data_eps[i].tx_mem);
		data_eps[i].dev = dev;
		data_eps[i].ready = true;
		data_ready_ports++;

		/*
		 * 必须判：驱动不支持 IRQ 收发时返回 -ENOSYS/-ENOTSUP，忽略就是
		 * "能发不能收"且毫无迹象。失败即把端点标成不可用，让上层走
		 * "端点不存在"路径。rx_enable 返回 void，判不了。
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

		data_ep_drop_rx(&data_eps[i]); /* 非当前端点的进账丢弃 */
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
		/* tx_enable 返回 void，发不出去只能靠对端超时/重传暴露 */
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

/* ===== 3. CRSF 端口 ====================================================== */

/* 帧缓冲按"1 字节长度 + 1 字节类型 + 载荷"存放 */
#define CRSF_FRAME_SLOT_LEN (2U + ZLRS_CRSF_PAYLOAD_MAX_LEN)
#define CRSF_FRAME_RING_LEN 512U
#define CRSF_TX_RING_LEN 1024U

/* 一块端点一套发送缓冲；dev == NULL 表示板上没挂这条别名 */
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
static const struct device *crsf_rx_source; /* 上一次喂解析器的端点 */
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
				/* 先记证据：端点选择的判据，见 port_active() */
				host_seen_mark(dev);
			}

			/* 记完证据再判断当前端点：第一帧不该被选择滞后吃掉 */
			if (dev != port_active(&crsf_pair)) {
				continue; /* 不是当前端点：收进来也不做数 */
			}

			if (dev != crsf_rx_source) {
				/* 切换端点：作废半截帧，免得把两块端点的字节拼成一帧 */
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
				uart_irq_tx_disable(dev); /* 排队中的字节等切回来再发 */
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
			continue; /* 板上没挂这条别名 */
		}

		if (!device_is_ready(dev)) {
			LOG_ERR("CRSF endpoint %s not ready", dev->name);
			continue;
		}

		ring_buf_init(&crsf_eps[i].tx, sizeof(crsf_eps[i].tx_mem),
			      crsf_eps[i].tx_mem);
		crsf_eps[i].dev = dev;
		crsf_eps[i].ready = true;
		crsf_ready = true;

		if (uart_irq_callback_user_data_set(dev, crsf_isr, &crsf_eps[i]) != 0) {
			LOG_ERR("CRSF endpoint %s: RX setup failed; endpoint disabled",
				dev->name);
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
	const uint8_t addr = (type == ZLRS_CRSF_TYPE_RC_CHANNELS)
				     ? ZLRS_CRSF_ADDR_FC
				     : ZLRS_CRSF_ADDR_TX_MODULE;
	const uint8_t len = zlrs_crsf_encode(frame, addr, type, payload, payload_len);
	struct crsf_ep *const ep = crsf_ep_of(port_active(&crsf_pair));

	if (!crsf_ready || ep == NULL || len == 0U) {
		return 0U;
	}

	if (ring_buf_put(&ep->tx, frame, len) != len) {
		return 0U; /* 发送缓冲满：CRSF 是尽力而为，丢帧并计数交给调用方 */
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
		/* 载荷缓冲区太小：整帧丢弃，避免缓冲错位 */
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

/* ===== 4. 配置模式 ======================================================= */

#if IS_ENABLED(CONFIG_ZLRS_CONFIG)

#define CFG_RX_CHUNK 32U
#define CFG_EXIT_POLL_MS 50U

static zlrs_cfg_parser_t cfg_parser;
static uint8_t cfg_tx_line[ZLRS_CFG_FRAME_MAX_LEN];
static zlrs_link_t *cfg_link; /* 配置模式作用的链路（控制链路） */
static uint8_t *cfg_failsafe_out;

/* ---- backend：把命令落到 NVS 与链路层 ---------------------------------- */

static int cfg_save_root_key(void *ctx, const uint8_t key[ZLRS_KEY_LEN])
{
	(void)ctx;

	if (zlrs_store_save_root_key(key) != 0) {
		return ZLRS_CFG_E_STORE;
	}

	/* 立刻生效，不必等重启 */
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
	/* 下次配对时把新值下发给对端 */
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

static const zlrs_cfg_backend_t cfg_backend = {
	.save_root_key = cfg_save_root_key,
	.read_status = cfg_read_status,
	.save_failsafe = cfg_save_failsafe,
	.save_baud = cfg_save_baud,
	.pair = cfg_pair,
};

/* ---- 会话 -------------------------------------------------------------- */

int zlrs_config_mode_run(zlrs_link_t *control, uint8_t *failsafe)
{
	const uint32_t timeout_ms =
		(uint32_t)CONFIG_ZLRS_APP_CONFIG_TIMEOUT_S * 1000U;
	uint32_t last_activity_ms = k_uptime_get_32();
	bool exit_requested = false;

	cfg_link = control;
	cfg_failsafe_out = failsafe;

	if (port_active(&data_pair) == NULL) {
		LOG_WRN("no data port on this board: config mode unavailable");
		return -1;
	}

	/* 这里没有超帧要抢时间，逻辑越直白越好 */
	zlrs_cfg_parser_init(&cfg_parser);

	LOG_INF("config mode: %u s idle timeout, exit on EXIT command",
		(unsigned int)CONFIG_ZLRS_APP_CONFIG_TIMEOUT_S);

	/* 会话期间关狗：写短语要 100000 轮 PBKDF2（几十秒），会被 8 秒看门狗复位。
	 * 配置是有人值守的操作，退出时再武装回来。 */
	zlrs_watchdog_disarm();

	while (!exit_requested &&
	       (uint32_t)(k_uptime_get_32() - last_activity_ms) < timeout_ms) {
		uint8_t chunk[CFG_RX_CHUNK];
		bool any = false;
		zlrs_led_input_t led_in;

		zlrs_watchdog_feed();

		/* 白灯慢闪 = 不是正常工作状态 */
		memset(&led_in, 0, sizeof(led_in));
		led_in.config_mode = true;
		led_in.configured = cfg_link->configured;
		led_in.paired = cfg_link->paired;
		led_in.role_tx = IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX);
		led_in.link_state = cfg_link->state;
		led_in.lq_percent = cfg_link->stats.lq_percent;
		led_in.uptime_ms = k_uptime_get_32();
		zlrs_led_port_update(&led_in, led_in.uptime_ms);

		/* 每轮重新选端点：配置途中插上 USB 也能切过去 */
		const uint16_t got = zlrs_data_port_read(chunk, sizeof(chunk));

		/* 收发都走端口层，别用 uart_poll_in/out：数据口的 ISR 已经在搬字节，
		 * poll_in 只会看到空缓冲；poll_out 也不会真的发出去。 */
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

			(void)zlrs_data_port_write(cfg_tx_line, len);
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

/* 配置协议被裁掉：应用照旧调用，这里直接说"没有配置模式" */
int zlrs_config_mode_run(zlrs_link_t *control, uint8_t *failsafe)
{
	(void)control;
	(void)failsafe;

	return -1;
}

#endif /* CONFIG_ZLRS_CONFIG */

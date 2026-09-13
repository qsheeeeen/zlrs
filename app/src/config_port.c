/*
 * 配置模式的落地：数据 UART 收配置帧 → 分发 → 回响应。
 *
 * 配置模式期间不跑超帧、不做透传：按键时机把两种用途分开（doc/02 第 10 节）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "config_port.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <string.h>

#include "store.h"
#include "zlrs/config/config.h"
#include "zlrs/link/link.h"

LOG_MODULE_REGISTER(zlrs_config_port, CONFIG_APP_LOG_LEVEL);

#define RX_CHUNK 32U
#define EXIT_POLL_MS 50U

static const struct device *const uart = DEVICE_DT_GET(DT_ALIAS(zlrs_data_uart));

static zlrs_cfg_parser_t parser;
static uint8_t tx_line[ZLRS_CFG_FRAME_MAX_LEN];
static zlrs_link_t *link; /* 配置模式作用的链路（控制链路） */
static uint8_t *failsafe_out;

/* ---- backend：把命令落到 NVS 与链路层 ------------------------------------ */

static int cfg_save_root_key(void *ctx, const uint8_t key[ZLRS_KEY_LEN])
{
	(void)ctx;

	if (zlrs_store_save_root_key(key) != 0) {
		return ZLRS_CFG_E_STORE;
	}

	/* 立刻生效，不必等重启 */
	zlrs_link_set_root_key(link, key, k_uptime_get_32());

	LOG_INF("写入新的根密钥");

	return ZLRS_CFG_OK;
}

static int cfg_read_status(void *ctx, zlrs_cfg_status_t *out)
{
	(void)ctx;

	out->version = ZLRS_CFG_VERSION;
	out->role = link->cfg.role;
	out->state = link->state;
	out->flags = 0U;
	if (link->configured) {
		out->flags |= ZLRS_CFG_FLAG_KEY;
	}
	if (link->paired) {
		out->flags |= ZLRS_CFG_FLAG_PAIRED;
		memcpy(out->peer_id, link->peer_id, ZLRS_DEV_ID_LEN);
	}
	if (link->session_valid) {
		out->flags |= ZLRS_CFG_FLAG_SESSION;
	}
	memcpy(out->dev_id, link->dev_id, ZLRS_DEV_ID_LEN);
	out->lq_percent = link->stats.lq_percent;

	return ZLRS_CFG_OK;
}

static int cfg_save_failsafe(void *ctx, const uint8_t channels[ZLRS_CFG_FAILSAFE_LEN])
{
	(void)ctx;

	if (zlrs_store_save_failsafe(channels) != 0) {
		return ZLRS_CFG_E_STORE;
	}
	if (failsafe_out != NULL) {
		memcpy(failsafe_out, channels, ZLRS_CFG_FAILSAFE_LEN);
	}

	LOG_INF("写入失控值");

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

	LOG_INF("数据口波特率改为 %u（下次启动生效）", (unsigned int)baud);

	return ZLRS_CFG_OK;
}

static int cfg_pair(void *ctx)
{
	(void)ctx;

	zlrs_link_press(link, k_uptime_get_32());
	LOG_INF("配置工具触发配对");

	return ZLRS_CFG_OK;
}

static int cfg_unbind(void *ctx)
{
	(void)ctx;

	zlrs_link_unbind(link, k_uptime_get_32());
	(void)zlrs_store_clear_peer();
	LOG_INF("配置工具触发解绑");

	return ZLRS_CFG_OK;
}

static const zlrs_cfg_backend_t backend = {
	.save_root_key = cfg_save_root_key,
	.read_status = cfg_read_status,
	.save_failsafe = cfg_save_failsafe,
	.save_baud = cfg_save_baud,
	.pair = cfg_pair,
	.unbind = cfg_unbind,
};

/* ---- 会话 ---------------------------------------------------------------- */

int zlrs_config_mode_run(zlrs_link_t *control, uint8_t *failsafe)
{
	const uint32_t timeout_ms =
		(uint32_t)CONFIG_ZLRS_APP_CONFIG_TIMEOUT_S * 1000U;
	uint32_t last_activity_ms = k_uptime_get_32();
	bool exit_requested = false;

	link = control;
	failsafe_out = failsafe;

	if (!device_is_ready(uart)) {
		LOG_ERR("数据口不可用，配置模式起不来");
		return -1;
	}

	/* 配置模式直接用轮询收发：这里没有超帧要抢时间，逻辑越直白越好 */
	zlrs_cfg_parser_init(&parser);

	LOG_INF("配置模式：%u 秒无操作超时，收到 EXIT 立即退出",
		(unsigned int)CONFIG_ZLRS_APP_CONFIG_TIMEOUT_S);

	while (!exit_requested &&
	       (uint32_t)(k_uptime_get_32() - last_activity_ms) < timeout_ms) {
		uint8_t chunk[RX_CHUNK];
		bool any = false;

		for (size_t i = 0U; i < sizeof(chunk); i++) {
			if (uart_poll_in(uart, &chunk[i]) != 0) {
				break;
			}
			any = true;

			const int result = zlrs_cfg_feed(&parser, chunk[i]);

			if (result != 1) {
				continue;
			}

			last_activity_ms = k_uptime_get_32();

			if (parser.cmd == ZLRS_CFG_CMD_EXIT) {
				exit_requested = true;
			}

			const uint8_t len = zlrs_cfg_dispatch(&backend, parser.cmd,
							      ZLRS_CFG_PAYLOAD(&parser),
							      parser.payload_len, tx_line,
							      sizeof(tx_line));

			for (uint8_t b = 0U; b < len; b++) {
				uart_poll_out(uart, tx_line[b]);
			}
		}

		if (!any) {
			k_msleep(EXIT_POLL_MS);
		}
	}

	LOG_INF("配置模式结束");

	return exit_requested ? 0 : 1;
}

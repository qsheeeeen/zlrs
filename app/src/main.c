/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * zlrs 应用入口：跑文档里的超帧，把控制链路与数据链路接上 LR2021 与串口。
 *
 *   - 射频：USP/RAC（app/src/rac_port.c）
 *   - 串口：数据 UART 透传（app/src/serial_port.c）
 *   - 密钥与配对记录：NVS（app/src/store.c）
 *
 * 初始化顺序见 docs/01-system.md 第 7 节，线程模型见 docs/04-threading.md。
 */

#include <zephyr/app_version.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include <smtc_rac_api.h>
#include <smtc_sw_platform_helper.h>

#include "rac_port.h"
#include "serial_port.h"
#include "store.h"

#include "zlrs/link/link.h"

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

#define FRAME_PERIOD_US (1000000U / ZLRS_FRAME_RATE_HZ)
#define SLOT_BUDGET_MS 10U
#define SERIAL_CHUNK_LEN 64U
#define REPORT_PERIOD_MS 1000U
#define PRESS_MIN_MS 30U

static zlrs_link_t link_control;
static zlrs_link_t link_data;

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_NODELABEL(usr_btn), gpios);
static bool button_down;
static uint32_t button_since_ms;

static uint8_t rc_payload[ZLRS_RC_PAYLOAD_LEN];
static uint32_t last_report_ms;
static struct {
	uint32_t rc_rx;
	uint32_t tlm_rx;
	uint32_t data_rx;
} counters;

/* ---- 链路回调（链路线程里执行） ----------------------------------------- */

static void on_rc(void *ctx, const uint8_t *payload, uint16_t len)
{
	(void)ctx;
	(void)payload;
	(void)len;

	/* CRSF 输出端口是下一步；这里只统计，避免 150 Hz 刷日志 */
	counters.rc_rx++;
}

static void on_tlm(void *ctx, const uint8_t *payload, uint16_t len)
{
	(void)ctx;
	(void)payload;
	(void)len;

	counters.tlm_rx++;
}

static void on_data(void *ctx, const uint8_t *data, uint16_t len)
{
	(void)ctx;

	counters.data_rx += zlrs_serial_write(data, len);
}

static void on_state(void *ctx, uint8_t state)
{
	const char *name = "?";
	const uint8_t kind = (uint8_t)(uintptr_t)ctx;

	switch (state) {
	case ZLRS_STATE_UNCONFIGURED:
		name = "unconfigured";
		break;
	case ZLRS_STATE_UNPAIRED:
		name = "unpaired";
		break;
	case ZLRS_STATE_PAIRING:
		name = "pairing";
		break;
	case ZLRS_STATE_SEARCHING:
		name = "searching";
		break;
	case ZLRS_STATE_CONNECTED:
		name = "connected";
		break;
	default:
		break;
	}

	LOG_INF("%s链路 -> %s", (kind == ZLRS_LINK_CONTROL) ? "控制" : "数据", name);
}

/* ---- 链路搭建 ------------------------------------------------------------ */

static void build_links(void)
{
	zlrs_link_cfg_t ctl_cfg = {
		.kind = ZLRS_LINK_CONTROL,
		.role = IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX) ? ZLRS_ROLE_TX : ZLRS_ROLE_RX,
		.domain = CONFIG_ZLRS_APP_DOMAIN,
		.sync_slot = ZLRS_SYNC_SLOT_CONTROL,
		.profile = ZLRS_PROFILE_P1,
		.rate_bps = CONFIG_ZLRS_APP_CONTROL_RATE_BPS,
		.tx_power_dbm = CONFIG_ZLRS_APP_TX_POWER_DBM,
		.rx_timeout_ms = 2U,
		.beacon_period_ms = 1000U,
		.peer_timeout_ms = IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX) ? 250U : 100U,
		.pair_period_ms = 100U,
		.pair_window_ms = 30000U,
		.data_mtu = 0U,
	};
	zlrs_link_cfg_t data_cfg = ctl_cfg;
	const zlrs_link_events_t ctl_ev = {
		.on_rc = on_rc,
		.on_tlm = on_tlm,
		.on_state = on_state,
		.ctx = (void *)(uintptr_t)ZLRS_LINK_CONTROL,
	};
	const zlrs_link_events_t data_ev = {
		.on_data = on_data,
		.on_state = on_state,
		.ctx = (void *)(uintptr_t)ZLRS_LINK_DATA,
	};

	data_cfg.kind = ZLRS_LINK_DATA;
	data_cfg.domain = ZLRS_DOMAIN_ISM2400;
	data_cfg.sync_slot = ZLRS_SYNC_SLOT_DATA;
	data_cfg.rate_bps = CONFIG_ZLRS_APP_DATA_RATE_BPS;
	data_cfg.peer_timeout_ms = 0U; /* 数据链路不做失联判定 */
	data_cfg.data_mtu = CONFIG_ZLRS_APP_DATA_MTU;

	zlrs_link_init(&link_control, &ctl_cfg, zlrs_rac_control_ops(), &ctl_ev);
	zlrs_link_init(&link_data, &data_cfg, zlrs_rac_data_ops(), &data_ev);
}

static void load_identity(void)
{
	uint8_t root_key[ZLRS_KEY_LEN];
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	bool have_root_key = false;
	const uint32_t now_ms = zlrs_rac_port_now_ms();

	/* 根密钥：编译期给的优先（台架用），否则读 NVS */
	if (strlen(CONFIG_ZLRS_APP_ROOT_KEY_HEX) > 0U) {
		if (zlrs_store_parse_hex_key(CONFIG_ZLRS_APP_ROOT_KEY_HEX, root_key) == 0) {
			have_root_key = true;
		} else {
			LOG_ERR("CONFIG_ZLRS_APP_ROOT_KEY_HEX 不是 64 个十六进制字符");
		}
	}
	if (!have_root_key && zlrs_store_load_root_key(root_key) == (int)ZLRS_KEY_LEN) {
		have_root_key = true;
	}

	/* 设备标识：首次启动生成并存 NVS，见 docs/02-security.md 第 2 节 */
	if (zlrs_store_load_dev_id(dev_id) != (int)ZLRS_DEV_ID_LEN) {
		const uint32_t rnd = sys_rand32_get();

		memcpy(dev_id, &rnd, sizeof(dev_id));
		(void)zlrs_store_save_dev_id(dev_id);
	}
	zlrs_link_set_dev_id(&link_control, dev_id);
	zlrs_link_set_dev_id(&link_data, dev_id);

	if (!have_root_key) {
		LOG_WRN("没有根密钥：不参与收发（等配置工具写入，见 docs/02-security.md）");
		return;
	}

	zlrs_link_set_root_key(&link_control, root_key, now_ms);
	zlrs_link_set_root_key(&link_data, root_key, now_ms);

	if (zlrs_store_load_peer(peer_id) == (int)ZLRS_DEV_ID_LEN) {
		zlrs_link_set_peer(&link_data, peer_id, now_ms);
		LOG_INF("已恢复配对记录");
	}
}

/* ---- 超帧 ---------------------------------------------------------------- */

static void run_slot(uint8_t slot, uint32_t budget_ms)
{
	zlrs_link_t *link = (slot == ZLRS_SLOT_DATA_A || slot == ZLRS_SLOT_DATA_B)
				    ? &link_data
				    : &link_control;
	const zlrs_radio_req_t *req = zlrs_link_slot(link, slot, zlrs_rac_port_now_ms());

	if (req == NULL) {
		return;
	}

	if (link->radio.submit(link->radio.ctx, req) != 0 ||
	    zlrs_rac_port_wait(budget_ms) != 0) {
		/* 射频没在预算内回来：放弃事务并给链路层收尾，免得卡住 */
		zlrs_rac_port_abort();
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, zlrs_rac_port_now_ms());
		return;
	}

	zlrs_rac_port_drain();
}

static void pump_serial(void)
{
	uint8_t chunk[SERIAL_CHUNK_LEN];
	const uint16_t got = zlrs_serial_read(chunk, sizeof(chunk));

	if (got > 0U) {
		(void)zlrs_link_write(&link_data, chunk, got); /* 队列满就丢尾巴 */
	}
}

static void adopt_pairing(void)
{
	if (!link_control.paired || link_data.paired) {
		return;
	}

	/* 控制链路配对成功后，把对端 dev_id 同步给数据链路并落盘 */
	zlrs_link_set_peer(&link_data, link_control.peer_id, zlrs_rac_port_now_ms());
	(void)zlrs_store_save_peer(link_control.peer_id);

	LOG_INF("配对完成，对端 %02x%02x%02x%02x", link_control.peer_id[0],
		link_control.peer_id[1], link_control.peer_id[2], link_control.peer_id[3]);
}

static void unbind(void)
{
	zlrs_link_unbind(&link_control, zlrs_rac_port_now_ms());
	zlrs_link_unbind(&link_data, zlrs_rac_port_now_ms());
	(void)zlrs_store_clear_peer();

	LOG_INF("已解绑");
}

static void handle_button(uint32_t now_ms)
{
	const int pressed = gpio_pin_get_dt(&button);

	if (pressed > 0 && !button_down) {
		button_down = true;
		button_since_ms = now_ms;
		return;
	}

	if (pressed == 0 && button_down) {
		const uint32_t held_ms = now_ms - button_since_ms;

		button_down = false;

		if (held_ms >= CONFIG_ZLRS_APP_UNBIND_HOLD_MS) {
			unbind();
		} else if (held_ms >= PRESS_MIN_MS) {
			LOG_INF("进入配对（按住 %u ms）", held_ms);
			zlrs_link_press(&link_control, now_ms);
		}
	}
}

static void fill_rc_payload(void)
{
	/* 16 通道 × 11 位打包；还没有 CRSF 输入端口，先发中立值 */
	uint32_t acc = 0U;
	uint8_t bits = 0U;
	uint8_t *out = rc_payload;

	for (size_t i = 0; i < 16U; i++) {
		const uint32_t channel = (i == 0U) ? 172U : 992U; /* 油门最低，其余中位 */

		acc |= channel << bits;
		bits = (uint8_t)(bits + 11U);
		while (bits >= 8U) {
			*out++ = (uint8_t)(acc & 0xFFU);
			acc >>= 8;
			bits = (uint8_t)(bits - 8U);
		}
	}
}

static void report(uint32_t now_ms)
{
	if ((uint32_t)(now_ms - last_report_ms) < REPORT_PERIOD_MS) {
		return;
	}
	last_report_ms = now_ms;

	LOG_INF("%s 控制=%u 遥测=%u 透传=%u 串口丢=%u",
		IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX) ? "TX" : "RX", counters.rc_rx,
		counters.tlm_rx, counters.data_rx, zlrs_serial_dropped());
}

int main(void)
{
	LOG_INF("zlrs %s", APP_VERSION_STRING);

	/* 等 USP/RAC 线程就绪，再初始化 RAC（见 docs/01-system.md 第 7 节） */
	SMTC_SW_PLATFORM_INIT();
	SMTC_SW_PLATFORM_VOID(smtc_rac_init());

	(void)zlrs_store_init();
	build_links();
	load_identity();
	(void)zlrs_serial_init();
	fill_rc_payload();
	zlrs_link_set_rc(&link_control, rc_payload, sizeof(rc_payload));

	if (zlrs_rac_port_init(&link_control, &link_data) != 0) {
		LOG_ERR("RAC 初始化失败");
		return 0;
	}

	if (!gpio_is_ready_dt(&button) || gpio_pin_configure_dt(&button, GPIO_INPUT) != 0) {
		LOG_WRN("按键不可用");
	} else if (gpio_pin_get_dt(&button) > 0) {
		LOG_WRN("开机时按键被按住：配置模式尚未实现（见 docs/01-system.md 2.1）");
	}

	LOG_INF("进入超帧循环：%u Hz，控制 %s / 数据 2.4G", ZLRS_FRAME_RATE_HZ,
		IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX) ? "TX" : "RX");

	for (;;) {
		const int64_t start_us = k_uptime_get() * 1000;
		const uint32_t now_ms = zlrs_rac_port_now_ms();

		/*
		 * 四个时隙共享一个超帧的预算：某个时隙（比如空接收窗口）
		 * 卡住时不能把整帧拖长，剩下的时隙按剩余的预算走。
		 */
		for (size_t i = 0; i < 4U; i++) {
			const uint8_t slots[4] = { ZLRS_SLOT_CTL_DOWN, ZLRS_SLOT_CTL_UP,
						   ZLRS_SLOT_DATA_A, ZLRS_SLOT_DATA_B };
			const int64_t used_us = k_uptime_get() * 1000 - start_us;
			int64_t remain_us = (int64_t)FRAME_PERIOD_US - used_us;
			uint32_t budget_ms = 1U;

			if (remain_us > (int64_t)SLOT_BUDGET_MS * 1000) {
				budget_ms = SLOT_BUDGET_MS;
			} else if (remain_us > 1000) {
				budget_ms = (uint32_t)(remain_us / 1000);
			}

			run_slot(slots[i], budget_ms);
		}

		zlrs_link_tick(&link_control, now_ms);
		zlrs_link_tick(&link_data, now_ms);

		adopt_pairing();
		pump_serial();
		handle_button(now_ms);
		report(now_ms);

		const int64_t elapsed_us = k_uptime_get() * 1000 - start_us;
		const int64_t remain_us = (int64_t)FRAME_PERIOD_US - elapsed_us;

		if (remain_us > 0) {
			k_usleep((int32_t)remain_us);
		}
	}

	return 0;
}

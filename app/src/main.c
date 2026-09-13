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
 * 线程模型与初始化顺序见 doc/01-system.md 第 7 节。
 */

#include <zephyr/app_version.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include <smtc_rac_api.h>
#include <smtc_sw_platform_helper.h>

#include "crsf_port.h"
#include "rac_port.h"
#include "serial_port.h"
#include "store.h"

#include "zlrs/crsf/crsf.h"
#include "zlrs/link/link.h"
#include "zlrs/link/rate.h"
#include "zlrs/proto/tlm.h"

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

#define FRAME_PERIOD_US (1000000U / ZLRS_FRAME_RATE_HZ)
#define SLOT_BUDGET_MS 10U
#define SERIAL_CHUNK_LEN 64U
#define REPORT_PERIOD_MS 1000U
#define PRESS_MIN_MS 30U

static zlrs_link_t link_control;
static zlrs_link_t link_data;

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(zlrs_button), gpios);
static bool button_down;
static uint32_t button_since_ms;

/* CRSF 端口的收发（定义在下面，链路回调里要用） */
static void crsf_send_rc(const uint8_t *payload, uint8_t len);
static void crsf_send_link_stats(const zlrs_tlm_t *tlm);

static uint8_t rc_payload[ZLRS_RC_PAYLOAD_LEN];
static uint8_t failsafe_payload[ZLRS_CRSF_RC_PAYLOAD_LEN];
static uint32_t last_rc_input_ms;
static uint32_t last_tlm_up_ms;
static bool rc_input_seen;
static uint32_t crsf_bad_input;
static uint32_t last_report_ms;

/*
 * 速率自适应的档位表（从快到慢）。阈值要等实测误包率曲线回来再标定，
 * 见 doc/05-roadmap.md M4——这里给的是占位值，框架本身与阈值无关。
 */
#if IS_ENABLED(CONFIG_ZLRS_APP_RATE_ADAPT)
static const zlrs_rate_tier_t rate_tiers[] = {
	{ .rate_bps = 2600000U, .min_lq_to_stay = 85U },
	{ .rate_bps = 1300000U, .min_lq_to_stay = 60U },
	{ .rate_bps = 650000U, .min_lq_to_stay = 35U },
	{ .rate_bps = 325000U, .min_lq_to_stay = 0U },
};
static zlrs_rate_ctl_t rate_ctl;
#endif
static struct {
	uint32_t rc_rx;
	uint32_t tlm_rx;
	uint32_t data_rx;
} counters;

/* ---- 链路回调（链路线程里执行） ----------------------------------------- */

static void on_rc(void *ctx, const uint8_t *payload, uint16_t len)
{
	(void)ctx;

	counters.rc_rx++;
	/* RX 侧：把空口通道数据重建成 CRSF 帧发给飞控 */
	crsf_send_rc(payload, (uint8_t)len);
}

static void on_tlm(void *ctx, const uint8_t *payload, uint16_t len)
{
	zlrs_tlm_t tlm;

	(void)ctx;

	counters.tlm_rx++;
	/* TX 侧：把 RX 的上行质量转成给手台看的链路统计帧 */
	if (zlrs_tlm_decode(payload, len, &tlm)) {
		crsf_send_link_stats(&tlm);
	}
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
	data_cfg.arq = IS_ENABLED(CONFIG_ZLRS_APP_DATA_ARQ) ? 1U : 0U;
	data_cfg.peer_timeout_ms = 0U; /* 数据链路不做失联判定 */
	data_cfg.data_mtu = CONFIG_ZLRS_APP_DATA_MTU;

	zlrs_link_init(&link_control, &ctl_cfg, zlrs_rac_control_ops(), &ctl_ev);
	zlrs_link_init(&link_data, &data_cfg, zlrs_rac_data_ops(), &data_ev);

#if IS_ENABLED(CONFIG_ZLRS_APP_RATE_ADAPT)
	{
		uint8_t start = 0U;

		for (uint8_t i = 0U; i < ARRAY_SIZE(rate_tiers); i++) {
			if (rate_tiers[i].rate_bps == ctl_cfg.rate_bps) {
				start = i;
			}
		}
		zlrs_rate_init(&rate_ctl, rate_tiers, (uint8_t)ARRAY_SIZE(rate_tiers), start,
			       0U);
	}
#endif
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

	/* 设备标识：首次启动生成并存 NVS，见 doc/02-security.md 第 2 节 */
	if (zlrs_store_load_dev_id(dev_id) != (int)ZLRS_DEV_ID_LEN) {
		const uint32_t rnd = sys_rand32_get();

		memcpy(dev_id, &rnd, sizeof(dev_id));
		(void)zlrs_store_save_dev_id(dev_id);
	}
	zlrs_link_set_dev_id(&link_control, dev_id);
	zlrs_link_set_dev_id(&link_data, dev_id);

	if (!have_root_key) {
		LOG_WRN("没有根密钥：不参与收发（等配置工具写入，见 doc/02-security.md）");
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
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, zlrs_rac_port_now_ms());
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

/*
 * 失控值：默认是"油门最低、其余中位"，与 CRSF 端口断开时手台给的值一致。
 * 后面配置协议可以改写它（见 doc/01-system.md 2.1 的配置模式）。
 */
static void failsafe_init(void)
{
	uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT];

	zlrs_crsf_channels_default(channels);
	zlrs_crsf_channels_pack(failsafe_payload, channels);
	memcpy(rc_payload, failsafe_payload, sizeof(rc_payload));
}

/* TX：手台发来的 RC 帧 → 空口 RC 载荷 */
static void crsf_handle_input(uint8_t type, const uint8_t *payload, uint8_t len)
{
	if (type != ZLRS_CRSF_TYPE_RC_CHANNELS) {
		return; /* 手台/飞控的其它帧本设计不消费 */
	}
	if (len != ZLRS_CRSF_RC_PAYLOAD_LEN) {
		crsf_bad_input++;
		return;
	}

	zlrs_link_set_rc(&link_control, payload, len);
	last_rc_input_ms = zlrs_rac_port_now_ms();
	rc_input_seen = true;
}

/*
 * TX：给手台看的链路统计（0x14）。
 * 上行数字来自 RX 的遥测回报，下行数字是 TX 自己测的——两边各自只报自己知道的。
 */
static void crsf_send_link_stats(const zlrs_tlm_t *tlm)
{
	zlrs_crsf_link_stats_t stats;
	uint8_t payload[ZLRS_CRSF_LINK_STATS_LEN];

	memset(&stats, 0, sizeof(stats));
	stats.uplink_rssi_ant1 = tlm->rssi_neg_dbm;
	stats.uplink_rssi_ant2 = tlm->rssi_neg_dbm;
	stats.uplink_lq = tlm->lq_percent;
	stats.uplink_snr_db = tlm->snr_db;
	stats.active_antenna = 0U;
	stats.rf_mode = tlm->rf_mode;
	stats.uplink_tx_power = tlm->tx_power;
	stats.downlink_rssi = zlrs_crsf_rssi_from_dbm(link_control.stats.last_rssi);
	stats.downlink_lq = link_control.stats.lq_percent;
	stats.downlink_snr_db = link_control.stats.last_snr;

	zlrs_crsf_link_stats_encode(payload, &stats);
	(void)zlrs_crsf_port_write(ZLRS_CRSF_TYPE_LINK_STATISTICS, payload,
				   sizeof(payload));
}

/* RX：把收到的 RC 载荷重建成 CRSF 帧发给飞控 */
static void crsf_send_rc(const uint8_t *payload, uint8_t len)
{
	if (len != ZLRS_CRSF_RC_PAYLOAD_LEN) {
		return;
	}

	(void)zlrs_crsf_port_write(ZLRS_CRSF_TYPE_RC_CHANNELS, payload, len);
}

/* RX：把本侧测到的上行质量回报给 TX */
static void crsf_report_tlm(uint32_t now_ms)
{
	zlrs_tlm_t tlm;
	uint8_t payload[ZLRS_TLM_LEN];

	if ((uint32_t)(now_ms - last_tlm_up_ms) < CONFIG_ZLRS_APP_TLM_PERIOD_MS) {
		return;
	}
	last_tlm_up_ms = now_ms;

	memset(&tlm, 0, sizeof(tlm));
	tlm.lq_percent = link_control.stats.lq_percent;
	tlm.rssi_neg_dbm = zlrs_crsf_rssi_from_dbm(link_control.stats.last_rssi);
	tlm.snr_db = link_control.stats.last_snr;
	tlm.rf_mode = zlrs_crsf_rf_mode(link_control.cfg.rate_bps);
	tlm.tx_power = zlrs_crsf_tx_power_index(link_control.cfg.tx_power_dbm);
	tlm.state = link_control.state;
	if (link_control.paired) {
		tlm.flags |= ZLRS_TLM_FLAG_PAIRED;
	}
	if (link_control.session_valid) {
		tlm.flags |= ZLRS_TLM_FLAG_SESSION;
	}

	zlrs_tlm_encode(payload, &tlm);
	zlrs_link_set_tlm(&link_control, payload, sizeof(payload));
}

/* 主循环里的 CRSF 任务：收帧、TX 侧超时转失控、RX 侧输出与上报 */
static void pump_crsf(uint32_t now_ms)
{
	uint8_t type = 0U;
	uint8_t payload[ZLRS_CRSF_PAYLOAD_MAX_LEN];
	uint8_t len;

	if (!zlrs_crsf_port_ready()) {
		return;
	}

	while ((len = zlrs_crsf_port_read(&type, payload, sizeof(payload))) > 0U) {
		crsf_handle_input(type, payload, len);
	}

	if (IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX)) {
		/* 手台拔线/关机：TX 立刻改发预设失控值，不让对端继续跑旧杆量 */
		if (rc_input_seen &&
		    (uint32_t)(now_ms - last_rc_input_ms) >
			    CONFIG_ZLRS_APP_CRSF_TIMEOUT_MS) {
			rc_input_seen = false;
			zlrs_link_set_rc(&link_control, failsafe_payload,
					 sizeof(failsafe_payload));
			LOG_WRN("CRSF 输入超时：转失控值");
		}
	} else {
		/* RX：链路没连上就持续输出失控值，飞控才不会失控 */
		if (link_control.state != ZLRS_STATE_CONNECTED) {
			crsf_send_rc(failsafe_payload, sizeof(failsafe_payload));
		}
		crsf_report_tlm(now_ms);
	}
}

static void report(uint32_t now_ms)
{
	if ((uint32_t)(now_ms - last_report_ms) < REPORT_PERIOD_MS) {
		return;
	}
	last_report_ms = now_ms;

#if IS_ENABLED(CONFIG_ZLRS_APP_RATE_ADAPT)
	{
		uint8_t next = 0U;

		if (zlrs_rate_update(&rate_ctl, link_control.stats.lq_percent, now_ms, &next)) {
			LOG_INF("速率档 -> %u bps（LQ %u%%）",
				(unsigned int)rate_tiers[next].rate_bps,
				link_control.stats.lq_percent);
			zlrs_link_set_rate(&link_control, rate_tiers[next].rate_bps);
		}
	}
#endif

	LOG_INF("%s 控制=%u 遥测=%u 透传=%u 串口丢=%u",
		IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX) ? "TX" : "RX", counters.rc_rx,
		counters.tlm_rx, counters.data_rx, zlrs_serial_dropped());
	LOG_INF("CRSF 坏帧=%u 端口丢=%u 空口积压丢=%u LQ=%u%%",
		crsf_bad_input, zlrs_crsf_port_rx_dropped(),
		link_data.stats.data_drop_bytes, link_control.stats.lq_percent);
}

int main(void)
{
	LOG_INF("zlrs %s", APP_VERSION_STRING);

	/* 等 USP/RAC 线程就绪，再初始化 RAC（见 doc/01-system.md 第 7 节） */
	SMTC_SW_PLATFORM_INIT();
	SMTC_SW_PLATFORM_VOID(smtc_rac_init());

	(void)zlrs_store_init();
	build_links();
	load_identity();
	(void)zlrs_serial_init();
	failsafe_init();
	zlrs_link_set_rc(&link_control, rc_payload, sizeof(rc_payload));
	(void)zlrs_crsf_port_init();

	if (zlrs_rac_port_init(&link_control, &link_data) != 0) {
		LOG_ERR("RAC 初始化失败");
		return 0;
	}

	if (!gpio_is_ready_dt(&button) || gpio_pin_configure_dt(&button, GPIO_INPUT) != 0) {
		LOG_WRN("按键不可用");
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
		pump_crsf(now_ms);
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

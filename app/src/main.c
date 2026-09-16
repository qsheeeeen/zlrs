/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * zlrs 应用入口：跑文档里的超帧，把控制链路与数据链路接上 LR2021 与串口。
 *
 *   - 射频：USP/RAC（app/src/rac_port.c）
 *   - 端口：CRSF、数据口透传与配置模式（app/src/ports.c）
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

#include <string.h>

#include <psa/crypto.h>

#include <smtc_rac_api.h>
#include <smtc_sw_platform_helper.h>

#include "ports.h"
#include "board.h"
#include "led_port.h"
#include "rac_port.h"
#include "store.h"

#include "zlrs/crsf/crsf.h"
#include "zlrs/link/link.h"
#include "zlrs/link/power.h"
#include "zlrs/link/rate.h"
#include "zlrs/region/region.h"
#include "zlrs/proto/tlm.h"

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

/* 控制帧的时间计划：启动时按 Kconfig 与地区占空比算出来，见 plan_init() */
static uint32_t control_rate_hz;
static uint32_t frame_period_us;
static uint32_t slot_budget_us;
static uint8_t tlm_every_n;
static uint8_t data_every_n;
static uint8_t resync_every_n;

/* 控制帧的空中时间粗估：22 B RC + 3 B 头 + 4 B 标签 + 2 B CRC */
#define CTL_FRAME_LEN 31U
/* 遥测帧：8 B 载荷 + 头与标签 */
#define CTL_TLM_LEN 15U
/*
 * 上板调试：两条链路可以各自单独跑。控制链路承载 RC 与配对，数据链路承载
 * 透传——先各自单独跑通，再合起来看分时（见 doc/01-system.md 2.3）。
 */
#define ZLRS_CONTROL_ENABLED IS_ENABLED(CONFIG_ZLRS_APP_LINK_CONTROL)
#define ZLRS_DATA_ENABLED IS_ENABLED(CONFIG_ZLRS_APP_LINK_DATA)
#define ZLRS_CRSF_ENABLED IS_ENABLED(CONFIG_ZLRS_APP_CRSF_PORT)

#define SERIAL_CHUNK_LEN 64U
#define REPORT_PERIOD_MS 1000U

static zlrs_link_t link_control;
static zlrs_link_t link_data;

/* 两条链路各自的调制档：启动时按 Kconfig 与地区定下来，见 plan_init() */
static zlrs_mod_cfg_t control_mod;
static zlrs_mod_cfg_t data_mod;

/*
 * 频段角色（doc/01-system.md 3.2）：默认控制链路占 sub-G、数据链路占 2.4G；
 * 竞速模式对调。法规门（功率上限、速率上限、占空比）+ sub-G 那条链路才吃。
 */
#if IS_ENABLED(CONFIG_ZLRS_APP_BAND_ROLE_SUBG_DATA)
#define CONTROL_ON_SUBG 0
#else
#define CONTROL_ON_SUBG 1
#endif

/* 地区：决定频段、功率上限与占空比记账 */
static const zlrs_region_t *region;
static zlrs_duty_t duty;


static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(zlrs_button), gpios);
static bool button_down;
static bool button_fired; /* 这一次按住已经触发过配对 */
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
static uint32_t led_drop_seen;
static uint32_t led_drop_ms;

/*
 * 速率自适应的档位表（从快到慢）。阈值要等实测误包率曲线回来再标定，
 * 见 doc/05-roadmap.md M4——这里给的是占位值，框架本身与阈值无关。
 */
#if IS_ENABLED(CONFIG_ZLRS_APP_RATE_ADAPT)
static const zlrs_rate_tier_t rate_tiers[] = {
	{ .mod = { .modulation = ZLRS_MOD_FLRC, .rate_bps = 2600000U },
	  .min_lq_to_stay = 85U },
	{ .mod = { .modulation = ZLRS_MOD_FLRC, .rate_bps = 1300000U },
	  .min_lq_to_stay = 60U },
	{ .mod = { .modulation = ZLRS_MOD_FLRC, .rate_bps = 650000U },
	  .min_lq_to_stay = 35U },
	{ .mod = { .modulation = ZLRS_MOD_FLRC, .rate_bps = 325000U },
	  .min_lq_to_stay = 0U },
};
static zlrs_rate_ctl_t rate_ctl;
#endif

/* 动态功率：TX 按 RX 回报的 RSSI/LQ 升降自己的发射功率 */
#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
static zlrs_power_ctl_t power_ctl;
/* 最新一次遥测里的上行质量；判据每秒评估一次，所以先存下来 */
static struct {
	int16_t rssi_dbm;
	uint8_t lq;
	bool fresh;
} tlm_latest;
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

#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
		/* 只存下来：判据在 1 Hz 的 report() 里评估，见 doc/03-protocol.md 4.6 */
		tlm_latest.rssi_dbm = -(int16_t)tlm.rssi_neg_dbm;
		tlm_latest.lq = tlm.lq_percent;
		tlm_latest.fresh = true;
#endif
	}
}

static void on_data(void *ctx, const uint8_t *data, uint16_t len)
{
	(void)ctx;

	counters.data_rx += zlrs_data_port_write(data, len);
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

	LOG_INF("%s link -> %s", (kind == ZLRS_LINK_CONTROL) ? "control" : "data", name);
}

/* RX：配对时从 TX 收到失控值，存 NVS 并立刻用于输出 */
static void on_failsafe(void *ctx, const uint8_t *payload, uint16_t len)
{
	(void)ctx;

	if (len != sizeof(failsafe_payload)) {
		return;
	}

	memcpy(failsafe_payload, payload, sizeof(failsafe_payload));
	(void)zlrs_store_save_failsafe(failsafe_payload);
	LOG_INF("failsafe values received from TX");
}

/*
 * 控制帧的时间计划：帧率档（Kconfig）× 地区占空比上限。
 * 500 Hz 档只有在不限占空比的地区才跑得动；EU868 的 1% 只能撑几十帧/秒，
 * 这时压到上限并告警——法规优先，但要让用户看见发生了什么。
 */
static void plan_init(void)
{
	zlrs_control_plan_t plan;
	/* 法规门只作用在 sub-G 那条链路上：竞速模式下它是数据链路 */
	const zlrs_region_t *ctl_region = CONTROL_ON_SUBG ? region : NULL;
	const zlrs_region_t *data_region = CONTROL_ON_SUBG ? NULL : region;
	zlrs_plan_req_t req = {
		.region = ctl_region,
		.frame_len = CTL_FRAME_LEN,
		.tlm_len = CTL_TLM_LEN,
	};

	/*
	 * 调制按链路各自选（doc/03-protocol.md 4.1.1）：窄带地区放不下宽带
	 * FLRC 时控制链路改跑 LoRa 远距档，数据链路始终留在 2.4G 跑 FLRC。
	 */
#if IS_ENABLED(CONFIG_ZLRS_APP_BAND_ROLE_SUBG_DATA)
	/* 竞速模式：控制链路独占 2.4G，速率/帧率用它自己那套配置 */
	control_mod = zlrs_mod_flrc(CONFIG_ZLRS_APP_RACING_CONTROL_RATE_BPS);
	req.want_hz = CONFIG_ZLRS_APP_RACING_CONTROL_RATE_HZ;
#elif IS_ENABLED(CONFIG_ZLRS_APP_CONTROL_MODULATION_LORA)
	control_mod = zlrs_region_clamp_mod(
		ctl_region, zlrs_mod_lora(CONFIG_ZLRS_APP_CONTROL_LORA_SF,
					  CONFIG_ZLRS_APP_CONTROL_LORA_BW_HZ,
					  CONFIG_ZLRS_APP_CONTROL_LORA_CR,
					  CONFIG_ZLRS_APP_CONTROL_LORA_PREAMBLE));
	req.want_hz = CONFIG_ZLRS_APP_LORA_CONTROL_RATE_HZ;
#else
	control_mod = zlrs_region_clamp_mod(
		ctl_region,
		zlrs_mod_flrc(zlrs_region_clamp_rate(ctl_region,
						     CONFIG_ZLRS_APP_CONTROL_RATE_BPS)));
	req.want_hz = CONFIG_ZLRS_APP_CONTROL_RATE_HZ;
#endif
	data_mod = zlrs_mod_flrc(
		zlrs_region_clamp_rate(data_region, CONFIG_ZLRS_APP_DATA_RATE_BPS));
	req.mod = &control_mod;

	/*
	 * 超帧周期由最慢的那条链路决定：数据链路那两个窗口按"每 data_every_n
	 * 帧跑一次"摊到每一帧里，计划内部迭代到自洽（见 lib/region/region.c）。
	 */
	req.other_mod = ZLRS_DATA_ENABLED ? &data_mod : NULL;
	req.other_frame_len = (uint16_t)(CONFIG_ZLRS_APP_DATA_MTU + 7U);
	zlrs_region_control_plan(&req, &plan);

	control_rate_hz = plan.rate_hz;
	frame_period_us = plan.frame_period_us;
	slot_budget_us = plan.slot_budget_us;
	tlm_every_n = plan.tlm_every_n;
	data_every_n = plan.data_every_n;

	if (plan.clamped_by_duty) {
		LOG_WRN("region %s duty %u%%: control rate %u Hz -> %u Hz",
			region->name, region->duty_percent, (unsigned int)req.want_hz,
			(unsigned int)plan.rate_hz);
	}
	if (plan.clamped_by_airtime) {
		LOG_WRN("airtime (mod %u): control rate %u Hz -> %u Hz",
			(unsigned int)control_mod.modulation, (unsigned int)req.want_hz,
			(unsigned int)plan.rate_hz);
	}

	/*
	 * 连接态周期 BEACON：按帧数换算（见 doc/03-protocol.md 5.1）。
	 * 帧周期长到 10 ms 以上时 1000 ms 装得下 100 帧以内，uint8 够用；
	 * 更短的帧周期由 MAX/MIN 压到 255 帧以内。
	 */
	if (CONFIG_ZLRS_APP_RESYNC_PERIOD_MS > 0) {
		const uint32_t frames = ((uint32_t)CONFIG_ZLRS_APP_RESYNC_PERIOD_MS * 1000U) /
					frame_period_us;

		resync_every_n = (uint8_t)MIN(MAX(frames, 1U), 255U);
	} else {
		resync_every_n = 0U;
	}

	LOG_INF("control plan: %u Hz (frame %u us, slot budget %u us), tlm 1/%u, data 1/%u",
		(unsigned int)control_rate_hz, (unsigned int)frame_period_us,
		(unsigned int)slot_budget_us, tlm_every_n, data_every_n);
	LOG_INF("modulation: control %u (sf %u bw %u), data %u (%u bps)",
		(unsigned int)control_mod.modulation, control_mod.sf,
		(unsigned int)control_mod.bw_hz, (unsigned int)data_mod.modulation,
		(unsigned int)data_mod.rate_bps);
	LOG_INF("connected-state beacon every %u frame(s) (resync, doc/03 5.1)",
		(unsigned int)resync_every_n);
}

/* ---- 链路搭建 ------------------------------------------------------------ */

static void build_links(void)
{
	zlrs_link_cfg_t ctl_cfg = {
		.kind = ZLRS_LINK_CONTROL,
		.role = IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX) ? ZLRS_ROLE_TX : ZLRS_ROLE_RX,
		/* 频段角色可配：远距模式控制链路占 sub-G，竞速模式对调（见 01 第 3.2 节） */
		.domain = CONTROL_ON_SUBG ? region->domain : ZLRS_DOMAIN_ISM2400,
		.sync_slot = ZLRS_SYNC_SLOT_CONTROL,
		.profile = IS_ENABLED(CONFIG_ZLRS_APP_SECURITY_PROFILE_P0) ? ZLRS_PROFILE_P0
									    : ZLRS_PROFILE_P1,
		/*
		 * 上板调试档：这三项各自可以关（见 doc/01-system.md 2.3）。
		 * 关掉跳频 → 收发时序与跳频表分开看；关掉握手 → 少一段钥匙派生；
		 * 关掉遥测 → 控制链路只剩下行 RC。
		 */
		.hopping = IS_ENABLED(CONFIG_ZLRS_APP_FHSS) ? 1U : 0U,
		.handshake = IS_ENABLED(CONFIG_ZLRS_APP_SESSION_HANDSHAKE) ? 1U : 0U,
		.telemetry = IS_ENABLED(CONFIG_ZLRS_APP_TELEMETRY) ? 1U : 0U,
		/* 调制档：窄频段地区走 LoRa 远距档，宽带地区走 FLRC（见 plan_init） */
		.mod = control_mod,
		.tx_power_dbm = CONTROL_ON_SUBG
					? MIN(CONFIG_ZLRS_APP_TX_POWER_DBM,
					      region->max_tx_power_dbm)
					: CONFIG_ZLRS_APP_TX_POWER_DBM,
		.rx_timeout_ms = 2U,
		.beacon_period_ms = 1000U,
		/*
		 * 遥测是 TX 侧的活证：关掉遥测就没东西可听，失联判定得一起关，
		 * 否则每一帧都会把 TX 拉回 SEARCHING 重建会话。
		 */
		.peer_timeout_ms = (IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX) &&
				    IS_ENABLED(CONFIG_ZLRS_APP_TELEMETRY))
					   ? 250U
					   : (IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX) ? 0U : 100U),
		.pair_period_ms = 100U,
		.pair_window_ms = 30000U,
		.data_mtu = 0U,
		/* 节奏稀疏化：见 plan_init()，两端同固件所以一致 */
		.tlm_every_n = tlm_every_n,
		.data_every_n = 1U, /* 控制链路不用这个字段 */
		/* 连接态周期 BEACON：跳频索引重同步 + 对端重启后自动重连 */
		.resync_every_n = resync_every_n,
	};
	zlrs_link_cfg_t data_cfg = ctl_cfg;
	const zlrs_link_events_t ctl_ev = {
		.on_rc = on_rc,
		.on_tlm = on_tlm,
		.on_state = on_state,
		.on_failsafe = on_failsafe,
		.ctx = (void *)(uintptr_t)ZLRS_LINK_CONTROL,
	};
	const zlrs_link_events_t data_ev = {
		.on_data = on_data,
		.on_state = on_state,
		.ctx = (void *)(uintptr_t)ZLRS_LINK_DATA,
	};

	data_cfg.kind = ZLRS_LINK_DATA;
	data_cfg.domain = CONTROL_ON_SUBG ? ZLRS_DOMAIN_ISM2400 : region->domain;
	data_cfg.sync_slot = ZLRS_SYNC_SLOT_DATA;
	data_cfg.mod = data_mod;
	data_cfg.tx_power_dbm =
		CONTROL_ON_SUBG ? CONFIG_ZLRS_APP_TX_POWER_DBM
				: MIN(CONFIG_ZLRS_APP_TX_POWER_DBM,
				      region->max_tx_power_dbm);
	data_cfg.arq = IS_ENABLED(CONFIG_ZLRS_APP_DATA_ARQ) ? 1U : 0U;
	/*
	 * ARQ 重传超时由档位派生：一帧的空中时间加一次往返余量，至少留 30 ms
	 * （FLRC 的亚毫秒帧用的就是那个默认值）。
	 */
	data_cfg.arq_timeout_ms = (uint16_t)MAX(
		ZLRS_ARQ_TIMEOUT_MS,
		2U * zlrs_mod_airtime_ms(&data_mod, CONFIG_ZLRS_APP_DATA_MTU + 7U));
	/*
	 * 数据链路尽力而为：空闲不发空帧，所以不判失联——判了会在空闲时反复重建
	 * 会话。代价是对端重启后要等这一侧重启或重配，见 doc/03-protocol.md 3.6。
	 */
	data_cfg.peer_timeout_ms = 0U;
	data_cfg.data_mtu = CONFIG_ZLRS_APP_DATA_MTU;
	/* 数据链路的重建可以慢一点：BEACON 2 s，见 doc/03-protocol.md 3.6 */
	data_cfg.beacon_period_ms = 2000U;
	data_cfg.tlm_every_n = 1U; /* 数据链路没有遥测时隙 */
	data_cfg.data_every_n = data_every_n;

	zlrs_link_init(&link_control, &ctl_cfg, zlrs_rac_control_ops(), &ctl_ev);
	zlrs_link_init(&link_data, &data_cfg, zlrs_rac_data_ops(), &data_ev);

#if IS_ENABLED(CONFIG_ZLRS_APP_RATE_ADAPT)
	{
		uint8_t start = 0U;

		for (uint8_t i = 0U; i < ARRAY_SIZE(rate_tiers); i++) {
			if (rate_tiers[i].mod.rate_bps == ctl_cfg.mod.rate_bps) {
				start = i;
			}
		}
		zlrs_rate_init(&rate_ctl, rate_tiers, (uint8_t)ARRAY_SIZE(rate_tiers), start,
			       0U);
	}
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
	/* 上限取"配置功率与地区上限的较小值"，下限是配置项 */
	zlrs_power_init(&power_ctl, MIN(CONFIG_ZLRS_APP_TX_POWER_DBM, region->max_tx_power_dbm),
			CONFIG_ZLRS_APP_DYNAMIC_POWER_FLOOR_DBM, 0U);
#endif
}

static void load_identity(void)
{
	uint8_t root_key[ZLRS_KEY_LEN];
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	zlrs_store_pairing_t pairing;
	bool have_root_key = false;
	const uint32_t now_ms = zlrs_rac_port_now_ms();

	/* 根密钥：编译期给的优先（台架用），否则读 NVS */
	if (strlen(CONFIG_ZLRS_APP_ROOT_KEY_HEX) > 0U) {
		if (zlrs_store_parse_hex_key(CONFIG_ZLRS_APP_ROOT_KEY_HEX, root_key) == 0) {
			have_root_key = true;
		} else {
			LOG_ERR("CONFIG_ZLRS_APP_ROOT_KEY_HEX is not 64 hex characters");
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
		LOG_WRN("no root key: staying off air until one is written (doc/02-security.md)");
		return;
	}

	zlrs_link_set_root_key(&link_control, root_key, now_ms);
	zlrs_link_set_root_key(&link_data, root_key, now_ms);

	/* 配对记录：对端 dev_id + 配对时的两个 nonce，两条链路都要装（见 02 第 3 节） */
	if (zlrs_store_load_pairing(&pairing) == (int)sizeof(pairing)) {
		zlrs_link_set_peer(&link_control, pairing.peer_id, pairing.pair_salt, now_ms);
		zlrs_link_set_peer(&link_data, pairing.peer_id, pairing.pair_salt, now_ms);
		LOG_INF("pairing record restored");
	}
}

/* ---- 超帧 ---------------------------------------------------------------- */

static void run_slot(uint8_t slot)
{
	const bool data_slot = (slot == ZLRS_SLOT_DATA_A || slot == ZLRS_SLOT_DATA_B);
	zlrs_link_t *link = data_slot ? &link_data : &link_control;
	const uint32_t now_ms = zlrs_rac_port_now_ms();
	/* 法规门（占空比）只作用在 sub-G 那条链路上 */
	const bool on_subg = data_slot ? (CONTROL_ON_SUBG == 0) : (CONTROL_ON_SUBG != 0);
	const zlrs_radio_req_t *req;

	if (data_slot ? !ZLRS_DATA_ENABLED : !ZLRS_CONTROL_ENABLED) {
		return; /* 这条链路这次没启用 */
	}

	req = zlrs_link_slot(link, slot, now_ms);

	if (req == NULL) {
		return;
	}

	/*
	 * 地区占空比（EU868 是 1%）：额度用完就把这个时隙让出去。
	 * 让出去的时隙按"射频失败"收尾，链路层会把数据放回队列下轮再试。
	 * 只有 sub-G 那条链路受这条约束（竞速模式下就是数据链路）。
	 */
	if (req->is_tx && on_subg &&
	    !zlrs_duty_reserve(&duty, zlrs_airtime_mod_ms(&req->mod, req->size), now_ms)) {
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, now_ms);
		return;
	}

	if (link->radio.submit(link->radio.ctx, req) != 0 ||
	    zlrs_rac_port_wait_us(slot_budget_us) != 0) {
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
	uint16_t got;

	if (!ZLRS_DATA_ENABLED) {
		return; /* 没有数据链路：串口不往上送 */
	}

	got = zlrs_data_port_read(chunk, sizeof(chunk));

	if (got > 0U) {
		(void)zlrs_link_write(&link_data, chunk, got); /* 队列满就丢尾巴 */
	}
}

static void adopt_pairing(void)
{
	zlrs_store_pairing_t pairing;

	/* 配对记录由控制链路产生、数据链路采用；少一条就不用同步 */
	if (!ZLRS_CONTROL_ENABLED || !ZLRS_DATA_ENABLED) {
		return;
	}

	if (!link_control.paired) {
		return;
	}
	/* 换绑或同对设备重新配对后，dev_id 或配对 nonce 会变，数据链路要重同步 */
	if (link_data.paired &&
	    memcmp(link_data.peer_id, link_control.peer_id, ZLRS_DEV_ID_LEN) == 0 &&
	    memcmp(link_data.pair_salt, link_control.pair_salt, ZLRS_PAIR_SALT_LEN) == 0) {
		return;
	}

	/* 控制链路配对成功后，把配对记录同步给数据链路并落盘 */
	zlrs_link_set_peer(&link_data, link_control.peer_id, link_control.pair_salt,
			   zlrs_rac_port_now_ms());
	memcpy(pairing.peer_id, link_control.peer_id, ZLRS_DEV_ID_LEN);
	memcpy(pairing.pair_salt, link_control.pair_salt, ZLRS_PAIR_SALT_LEN);
	(void)zlrs_store_save_pairing(&pairing);

	LOG_INF("paired with %02x%02x%02x%02x", link_control.peer_id[0],
		link_control.peer_id[1], link_control.peer_id[2], link_control.peer_id[3]);
}

/*
 * 按键只有一个动作：长按（按住到阈值）触发配对，见 doc/06-ui.md。
 * 没有短按，也没有按键解绑——要换绑就直接长按重新配对，新记录会顶掉旧的。
 */
static void handle_button(uint32_t now_ms)
{
	const int pressed = gpio_pin_get_dt(&button);

	/* 上板调试可以关掉按键配对：只剩开机进配置模式那一种用法 */
	if (!IS_ENABLED(CONFIG_ZLRS_APP_PAIRING_BUTTON) || !ZLRS_CONTROL_ENABLED) {
		return;
	}

	if (pressed > 0 && !button_down) {
		button_down = true;
		button_since_ms = now_ms;
		button_fired = false;
		return;
	}

	if (pressed > 0 && button_down && !button_fired &&
	    (uint32_t)(now_ms - button_since_ms) >= CONFIG_ZLRS_APP_PAIR_HOLD_MS) {
		button_fired = true;
		LOG_INF("pairing (button held %u ms)",
			(unsigned int)(now_ms - button_since_ms));
		zlrs_link_press(&link_control, now_ms);
		return;
	}

	if (pressed == 0 && button_down) {
		button_down = false;
	}
}

/*
 * 失控值：默认是"油门最低、其余中位"，与 CRSF 端口断开时手台给的值一致。
 * 后面配置协议可以改写它（见 doc/01-system.md 2.1 的配置模式）。
 */
static void failsafe_init(void)
{
	uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT];

	if (zlrs_store_load_failsafe(failsafe_payload) != (int)sizeof(failsafe_payload)) {
		/* 没配过就用默认值：油门最低、其余中位 */
		zlrs_crsf_channels_default(channels);
		zlrs_crsf_channels_pack(failsafe_payload, channels);
	}
	memcpy(rc_payload, failsafe_payload, sizeof(rc_payload));

	/* TX 侧：这份值会随下一次配对下发给 RX */
	zlrs_link_set_failsafe(&link_control, failsafe_payload, sizeof(failsafe_payload));
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

	if (!ZLRS_CRSF_ENABLED) {
		return;
	}

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
	if (!ZLRS_CRSF_ENABLED || len != ZLRS_CRSF_RC_PAYLOAD_LEN) {
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
	tlm.rf_mode = zlrs_crsf_rf_mode_mod(&link_control.cfg.mod);
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

	/* 上板调试可以关掉 CRSF 端口：TX 就只发配置好的失控值 */
	if (!ZLRS_CRSF_ENABLED || !ZLRS_CONTROL_ENABLED) {
		return;
	}

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
			LOG_WRN("CRSF input timeout: sending failsafe values");
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
			LOG_INF("rate tier -> %u bps (LQ %u%%)",
				(unsigned int)rate_tiers[next].mod.rate_bps,
				link_control.stats.lq_percent);
			zlrs_link_set_mod(&link_control, &rate_tiers[next].mod);
		}
	}
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
	{
		int8_t next = 0;
		const bool fresh = tlm_latest.fresh;

		tlm_latest.fresh = false;

		/*
		 * 每秒评估一次：这一秒收到过遥测就用它的 RSSI/LQ 判质量，
		 * 否则走 valid=false 的超时路径（回到上限）。
		 */
		if (zlrs_power_update(&power_ctl, fresh, tlm_latest.rssi_dbm,
				      tlm_latest.lq, now_ms, &next)) {
			zlrs_link_set_power(&link_control, next);
			LOG_INF("tx power -> %d dBm (rssi %d dBm, lq %u%%)", next,
				tlm_latest.rssi_dbm, tlm_latest.lq);
		}
	}
#endif

	LOG_INF("%s ctl=%u tlm=%u data=%u serial_drop=%u",
		IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX) ? "TX" : "RX", counters.rc_rx,
		counters.tlm_rx, counters.data_rx, zlrs_data_port_dropped());
	LOG_INF("crsf_bad=%u crsf_drop=%u air_drop=%u lq=%u%%",
		crsf_bad_input, zlrs_crsf_port_rx_dropped(),
		link_data.stats.data_drop_bytes, link_control.stats.lq_percent);
	LOG_INF("region %s: max %d dBm, duty denied %u",
		(region != NULL) ? region->name : "?", region->max_tx_power_dbm,
		duty.denied);
#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
	LOG_INF("tx power %d dBm (up %u / down %u / fallback %u)",
		zlrs_power_current_dbm(&power_ctl), power_ctl.raises, power_ctl.lowers,
		power_ctl.fallbacks);
#endif
}

/*
 * 状态灯：把链路状态翻译成 lib/led 的输入，见 doc/06-ui.md。
 * 每 20 Hz 刷一次（zlrs_led_port_update 内部限速）。
 */
/*
 * 配置不合法（比如地区与本机 SKU 不匹配）时的安全态：不进超帧、不发射频，
 * 只亮红灯让人看见。法规相关的错误宁可停机，也不能"凑合发"。
 */
static void fault_loop(void)
{
	zlrs_led_input_t in;

	memset(&in, 0, sizeof(in));
	in.failsafe = true; /* 红灯快闪 */
	in.link_state = ZLRS_STATE_SEARCHING;
	in.role_tx = IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX);
	in.uptime_ms = 60000U; /* 越过开机自检窗口，立刻进告警 */

	for (;;) {
		zlrs_led_port_update(&in, k_uptime_get_32());
		k_msleep(50);
	}
}

/*
 * 密码学后端初始化。PSA provider 必须在第一次导入钥匙之前初始化；失败就不参与
 * 收发——KDF 与每包 tag 都靠它，闭着眼上空中没有任何意义。
 */
static void crypto_init(void)
{
	const psa_status_t st = psa_crypto_init();

	if (st != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init failed (%d)", (int)st);
		fault_loop();
	}
}

static void led_tick(uint32_t now_ms)
{
	zlrs_led_input_t in;
	const uint32_t drops = link_data.stats.data_drop_bytes;

	if (!IS_ENABLED(CONFIG_ZLRS_APP_STATUS_LED)) {
		return;
	}

	if (drops != led_drop_seen) {
		led_drop_seen = drops;
		led_drop_ms = now_ms;
	}

	memset(&in, 0, sizeof(in));
	in.configured = link_control.configured;
	in.paired = link_control.paired;
	in.role_tx = IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX);
	in.link_state = link_control.state;
	in.lq_percent = link_control.stats.lq_percent;
	in.uptime_ms = now_ms;
	in.drop_recent = (drops > 0U) && (uint32_t)(now_ms - led_drop_ms) < 1000U;
	/* TX 没有手台输入时就是在发失控值；RX 未连接时在往飞控输出失控值 */
	in.failsafe = IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX)
			      ? !rc_input_seen
			      : (link_control.state != ZLRS_STATE_CONNECTED);

	zlrs_led_port_update(&in, now_ms);
}

int main(void)
{
	LOG_INF("zlrs %s", APP_VERSION_STRING);

	/* 上板调试档：这些开关关掉以后链路不再是生产形态，开机就喊一声 */
	if (!IS_ENABLED(CONFIG_ZLRS_APP_SECURITY_PROFILE_P1)) {
		LOG_WRN("bring-up: security profile P0, frames carry no tag");
	}
	if (!IS_ENABLED(CONFIG_ZLRS_APP_FHSS)) {
		LOG_WRN("bring-up: hopping off, every frame stays on the sync channel");
	}
	if (!IS_ENABLED(CONFIG_ZLRS_APP_SESSION_HANDSHAKE)) {
		LOG_WRN("bring-up: session handshake off, pairing keys are session keys");
	}
	if (!IS_ENABLED(CONFIG_ZLRS_APP_TELEMETRY)) {
		LOG_WRN("bring-up: telemetry off, control link is RC-only");
	}
	if (!ZLRS_CONTROL_ENABLED) {
		LOG_WRN("bring-up: control link off, only the data link runs "
			"(pairing needs the control link)");
	}
	if (!ZLRS_DATA_ENABLED) {
		LOG_WRN("bring-up: data link off, no serial passthrough");
	}
	if (!ZLRS_CRSF_ENABLED) {
		LOG_WRN("bring-up: CRSF port off, TX sends the configured failsafe values");
	}
	if (!IS_ENABLED(CONFIG_ZLRS_APP_CONFIG_MODE)) {
		LOG_WRN("bring-up: config mode off, this build cannot be configured");
	}
	if (!IS_ENABLED(CONFIG_ZLRS_APP_PAIRING_BUTTON)) {
		LOG_WRN("bring-up: pairing button off");
	}
	if (!IS_ENABLED(CONFIG_ZLRS_APP_STATUS_LED)) {
		LOG_WRN("bring-up: status led off");
	}

	/* 地区配置层：频段、功率上限与占空比都在这里定 */
	region = zlrs_region_get(CONFIG_ZLRS_APP_REGION);
	if (region == NULL) {
		LOG_ERR("unknown region %d", CONFIG_ZLRS_APP_REGION);
		return 0;
	}
	{
		const int rc = zlrs_region_validate(region, ZLRS_SUBG_BAND,
						    CONFIG_ZLRS_APP_TX_POWER_DBM);

		/* -5：地区要的 sub-G 硬件跟这块板子不是一回事 → 不发射频 */
		if (rc == -5) {
			LOG_ERR("region %s needs another sub-G sku (board band %d)",
				region->name, ZLRS_SUBG_BAND);
			fault_loop();
		}
		if (rc == -2) {
			LOG_WRN("region %s caps tx power at %d dBm (configured %d)",
				region->name, region->max_tx_power_dbm,
				CONFIG_ZLRS_APP_TX_POWER_DBM);
		} else if (rc != 0) {
			LOG_WRN("region check failed (%d): channel plan out of band", rc);
		}
	}
	zlrs_duty_init(&duty, region, 0U);
	if (IS_ENABLED(CONFIG_ZLRS_APP_STATUS_LED)) {
		(void)zlrs_led_port_init();
	}

	/* 等 USP/RAC 线程就绪，再初始化 RAC（见 doc/01-system.md 第 7 节） */
	SMTC_SW_PLATFORM_INIT();
	SMTC_SW_PLATFORM_VOID(smtc_rac_init());

	(void)zlrs_store_init();
	plan_init();
	build_links();
	crypto_init();
	load_identity();
	failsafe_init();

	/*
	 * 配置模式：开机时按键按住就进，见 doc/01-system.md 2.1。
	 * 跑完直接继续正常启动，不用重启——写进去的密钥与失控值当场就生效。
	 */
	if (IS_ENABLED(CONFIG_ZLRS_APP_CONFIG_MODE) && gpio_is_ready_dt(&button) &&
	    gpio_pin_configure_dt(&button, GPIO_INPUT) == 0) {
		k_msleep(20); /* 上电去抖 */
		if (gpio_pin_get_dt(&button) > 0) {
			LOG_WRN("button held at boot: entering config mode");
			(void)zlrs_config_mode_run(&link_control, failsafe_payload);
			memcpy(rc_payload, failsafe_payload, sizeof(rc_payload));
			zlrs_link_set_rc(&link_control, rc_payload, sizeof(rc_payload));
		}
	}

	{
		uint32_t baud = 0U;

		if (zlrs_store_load_baud(&baud) != (int)sizeof(baud)) {
			baud = 0U;
		}
		(void)zlrs_data_port_init(baud);
	}

	zlrs_link_set_rc(&link_control, rc_payload, sizeof(rc_payload));
	if (ZLRS_CRSF_ENABLED) {
		(void)zlrs_crsf_port_init();
	}

	if (zlrs_rac_port_init(&link_control, &link_data) != 0) {
		LOG_ERR("RAC init failed");
		return 0;
	}

	if (!gpio_is_ready_dt(&button) || gpio_pin_configure_dt(&button, GPIO_INPUT) != 0) {
		LOG_WRN("button unavailable");
	}

	LOG_INF("entering superframe loop: %u Hz, %s / %s (%s)",
		(unsigned int)control_rate_hz,
		ZLRS_CONTROL_ENABLED ? "control on" : "control off",
		ZLRS_DATA_ENABLED ? "data on" : "data off",
		IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX) ? "TX" : "RX");

	for (;;) {
		const int64_t start_us = k_uptime_get() * 1000;
		const uint32_t now_ms = zlrs_rac_port_now_ms();

		/*
		 * 四个时隙平分一帧（slot_budget_us）：某个时隙卡住也不会把整帧拖长——
		 * 超预算的事务由 run_slot 放弃并收尾，下一帧照常走。
		 * 遥测与数据窗口的稀疏化在链路层内部按 tlm_every_n / data_every_n 做。
		 */
		run_slot(ZLRS_SLOT_CTL_DOWN);
		run_slot(ZLRS_SLOT_CTL_UP);
		run_slot(ZLRS_SLOT_DATA_A);
		run_slot(ZLRS_SLOT_DATA_B);

		if (ZLRS_CONTROL_ENABLED) {
			zlrs_link_tick(&link_control, now_ms);
		}
		if (ZLRS_DATA_ENABLED) {
			zlrs_link_tick(&link_data, now_ms);
		}

		adopt_pairing();
		pump_serial();
		pump_crsf(now_ms);
		handle_button(now_ms);
		led_tick(now_ms);
		report(now_ms);

		const int64_t elapsed_us = k_uptime_get() * 1000 - start_us;
		const int64_t remain_us = (int64_t)frame_period_us - elapsed_us;

		if (remain_us > 0) {
			k_usleep((int32_t)remain_us);
		}
	}

	return 0;
}

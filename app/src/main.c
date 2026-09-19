/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * zlrs 应用入口：跑超帧，把两条链路接上射频与串口。线程模型与初始化顺序见
 * doc/07-architecture.md 第 6 节。
 */

#include <zephyr/app_version.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/reboot.h>

#include <string.h>

#include <psa/crypto.h>

#if IS_ENABLED(CONFIG_ZLRS_APP_UPDATE_TRIGGER)
#include <zephyr/retention/bootmode.h>
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_MCUBOOT_CONFIRM)
#include <zephyr/dfu/mcuboot.h>
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_SHELL)
#include <zephyr/shell/shell.h>
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_RADIO)
#include <smtc_rac_api.h>
#include <smtc_sw_platform_helper.h>
#endif

#include "ports.h"
#include "board.h"
#include "led_port.h"
#include "radio_port.h"
#include "store.h"
#include "watchdog.h"

#include "zlrs/crsf/crsf.h"
#include "zlrs/config/config.h"
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
/* 数据帧载荷上限：跟着帧格式与速率档算出来（不是配置项） */
static uint16_t data_mtu;

/* 控制帧的空中时间粗估：22 B RC + 3 B 头 + 4 B 标签 + 2 B CRC */
#define CTL_FRAME_LEN 31U
/* 遥测帧：8 B 载荷 + 头与标签 */
#define CTL_TLM_LEN 15U
/*
 * 上板调试：两条链路可以各自单独跑。控制链路承载 RC 与配对，数据链路承载
 * 透传——先各自单独跑通，再合起来看分时（见 doc/12-debug.md 第 3 节）。
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
 * 频段角色（doc/07-architecture.md 第 2 节）：默认控制链路占 sub-G、数据链路占 2.4G；
 * 竞速模式对调。法规门（功率上限、速率上限、占空比）+ sub-G 那条链路才吃。
 */
#if IS_ENABLED(CONFIG_ZLRS_APP_BAND_ROLE_SUBG_DATA)
#define CONTROL_ON_SUBG 0
#else
#define CONTROL_ON_SUBG 1
#endif

/* 地区：决定频段、功率上限与占空比记账 */
static const zlrs_region_t *region;
static uint8_t region_id;
static int8_t tx_power_dbm;
static zlrs_duty_t duty;


static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(zlrs_button), gpios);
static bool button_down;
static bool button_fired; /* 这一次按住已经触发过配对 */
static uint32_t button_since_ms;

/* CRSF 端口的收发（定义在下面，链路回调里要用） */
static void crsf_send_rc(const uint8_t *payload, uint8_t len);
static void crsf_send_link_stats(const zlrs_tlm_t *tlm);

#if IS_ENABLED(CONFIG_ZLRS_APP_ENTROPY_TRNG)
/* 熵源自检（定义在 main 之前），shell 命令与开机自检都用它 */
static int entropy_selftest(void);
#endif

static uint8_t rc_payload[ZLRS_RC_PAYLOAD_LEN];
static uint8_t failsafe_payload[ZLRS_CRSF_RC_PAYLOAD_LEN];
static uint32_t last_rc_input_ms;
static uint32_t last_tlm_up_ms;
static bool rc_input_seen;
static uint32_t crsf_bad_input;
static uint32_t last_report_ms;
static uint32_t led_drop_seen;
/* 连上过才算"失控"：从没连上的设备该显示未配置/未配对/搜索，而不是红灯 */
static bool ever_connected;
static uint32_t led_drop_ms;

/*
 * 速率自适应的档位表（从快到慢）。阈值要等实测误包率曲线回来再标定，
 * 见 doc/13-roadmap.md——这里给的是占位值，框架本身与阈值无关。
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
		/* 只存下来：判据在 1 Hz 的 report() 里评估，见 doc/09-protocol.md 4.5 */
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
 * 控制帧的时间计划：帧率档（Kconfig）× 地区占空比上限。超了就压到上限并
 * 告警——法规优先，但要让用户看见。
 */
/*
 * 搜索态的 SYNC 周期按调制档定（doc/09-protocol.md 3.6）：
 * 速率越低、一帧空中时间越长，间隔相应放宽到 100–600 ms。
 */
static uint16_t sync_period_for_mod(const zlrs_mod_cfg_t *mod)
{
	if (mod->modulation != ZLRS_MOD_FLRC) {
		return 600U; /* LoRa：一帧几十毫秒，再密就没意义 */
	}
	if (mod->rate_bps >= 2600000U) {
		return 100U;
	}
	if (mod->rate_bps >= 1300000U) {
		return 150U;
	}
	if (mod->rate_bps >= 650000U) {
		return 300U;
	}

	return 600U;
}

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
	 * 调制按链路各自选（doc/07-architecture.md 第 3 节）：窄带地区放不下宽带
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
	 * MTU 跟着帧格式与速率档走（doc/09-protocol.md 4.3）：取"数据窗口单向
	 * 预算"（四分之一帧周期）内装得下的最大载荷，再受帧格式载荷上限约束。
	 */
	{
		const uint32_t budget_us =
			(1000000U / (uint32_t)CONFIG_ZLRS_APP_CONTROL_RATE_HZ) / 4U;
		uint16_t len = (uint16_t)(ZLRS_PAYLOAD_MAX_LEN - 2U);

		while (len > 16U &&
		       zlrs_mod_airtime_us(&data_mod, (uint16_t)(len + 7U)) > budget_us) {
			len = (uint16_t)(len - 8U);
		}

		data_mtu = len;
	}

	/*
	 * 超帧周期由最慢的那条链路决定：数据链路那两个窗口按"每 data_every_n
	 * 帧跑一次"摊到每一帧里，计划内部迭代到自洽（见 lib/region/region.c）。
	 */
	req.other_mod = ZLRS_DATA_ENABLED ? &data_mod : NULL;
	req.other_frame_len = (uint16_t)(data_mtu + 7U);
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

	/* 连接态周期 SYNC 按帧数换算（见 doc/09-protocol.md 5.1）：1000 ms 在
	 * 常见帧周期下装得进 uint8，更短的帧周期由 MAX 压到 255 帧以内 */
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
	LOG_INF("connected-state sync every %u frame(s) (resync, doc/09-protocol.md 5.1)",
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
		.second_domain = CONTROL_ON_SUBG ? ZLRS_DOMAIN_ISM2400 : region->domain,
		.sync_slot = ZLRS_SYNC_SLOT_CONTROL,
		.profile = IS_ENABLED(CONFIG_ZLRS_APP_SECURITY_PROFILE_P0) ? ZLRS_PROFILE_P0
									    : ZLRS_PROFILE_P1,
		/* 上板调试档，各自可关，见 doc/12-debug.md 第 3 节 */
		.hopping = IS_ENABLED(CONFIG_ZLRS_APP_FHSS) ? 1U : 0U,
		.handshake = IS_ENABLED(CONFIG_ZLRS_APP_SESSION_HANDSHAKE) ? 1U : 0U,
		.telemetry = IS_ENABLED(CONFIG_ZLRS_APP_TELEMETRY) ? 1U : 0U,
		/* 调制档：窄频段地区走 LoRa 远距档，宽带地区走 FLRC（见 plan_init） */
		.mod = control_mod,
		.tx_power_dbm = CONTROL_ON_SUBG
					? MIN(tx_power_dbm,
					      region->max_tx_power_dbm)
					: tx_power_dbm,
		.rx_timeout_ms = 2U,
		.sync_period_ms = sync_period_for_mod(&control_mod),
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
		/* 连接态周期 SYNC：跳频索引重同步 + 对端重启后自动重连 */
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
	data_cfg.second_domain = CONTROL_ON_SUBG ? region->domain : ZLRS_DOMAIN_ISM2400;
	data_cfg.sync_slot = ZLRS_SYNC_SLOT_DATA;
	data_cfg.mod = data_mod;
	data_cfg.tx_power_dbm =
		CONTROL_ON_SUBG ? tx_power_dbm
				: MIN(tx_power_dbm,
				      region->max_tx_power_dbm);
	data_cfg.arq = IS_ENABLED(CONFIG_ZLRS_APP_DATA_ARQ) ? 1U : 0U;
	/*
	 * ARQ 重传超时由档位派生：一帧的空中时间加一次往返余量，至少留 30 ms
	 * （FLRC 的亚毫秒帧用的就是那个默认值）。
	 */
	data_cfg.arq_timeout_ms = (uint16_t)MAX(
		ZLRS_ARQ_TIMEOUT_MS,
		2U * zlrs_mod_airtime_ms(&data_mod, data_mtu + 7U));
	/*
	 * 数据链路尽力而为：空闲不发空帧，所以不判失联——判了会在空闲时反复重建
	 * 会话。代价是对端重启后要等这一侧重启或重配，见 doc/09-protocol.md 3.7。
	 */
	data_cfg.peer_timeout_ms = 0U;
	data_cfg.data_mtu = data_mtu;
	data_cfg.sync_period_ms = sync_period_for_mod(&data_mod);
	data_cfg.tlm_every_n = 1U; /* 数据链路没有遥测时隙 */
	data_cfg.data_every_n = data_every_n;

	zlrs_link_init(&link_control, &ctl_cfg, zlrs_radio_control_ops(), &ctl_ev);
	zlrs_link_init(&link_data, &data_cfg, zlrs_radio_data_ops(), &data_ev);

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
	zlrs_power_init(&power_ctl, MIN(tx_power_dbm, region->max_tx_power_dbm),
			CONFIG_ZLRS_APP_DYNAMIC_POWER_FLOOR_DBM, 0U);
#endif
}

static void load_identity(void)
{
	uint8_t root_key[ZLRS_KEY_LEN];
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	zlrs_store_pairing_t pairing;
	bool have_root_key = false;
	const uint32_t now_ms = zlrs_radio_port_now_ms();

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

	/* 设备标识：首次启动生成并存 NVS，见 doc/10-security.md 第 2 节 */
	if (zlrs_store_load_dev_id(dev_id) != (int)ZLRS_DEV_ID_LEN) {
		const uint32_t rnd = sys_rand32_get();

		memcpy(dev_id, &rnd, sizeof(dev_id));
		(void)zlrs_store_save_dev_id(dev_id);
	}
	zlrs_link_set_dev_id(&link_control, dev_id);
	zlrs_link_set_dev_id(&link_data, dev_id);

	if (!have_root_key) {
		LOG_WRN("no root key: staying off air until one is written (doc/10-security.md)");
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
	const uint32_t now_ms = zlrs_radio_port_now_ms();
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

	/* 地区占空比额度用完就让出时隙（按射频失败收尾，链路层下轮再试）；
	 * 只约束 sub-G 那条链路。 */
	if (req->is_tx && on_subg &&
	    !zlrs_duty_reserve(&duty, zlrs_airtime_mod_ms(&req->mod, req->size), now_ms)) {
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, now_ms);
		return;
	}

	if (link->radio.submit(link->radio.ctx, req) != 0 ||
	    zlrs_radio_port_wait_us(slot_budget_us) != 0) {
		/* 射频没在预算内回来：放弃事务并给链路层收尾，免得卡住 */
		zlrs_radio_port_abort();
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, zlrs_radio_port_now_ms());
		return;
	}

	zlrs_radio_port_drain();
}

#if IS_ENABLED(CONFIG_ZLRS_APP_UPDATE_TRIGGER)
/*
 * 重启进 MCUboot 的串口恢复：置 boot mode 标志（retained SRAM，重启不丢），
 * MCUboot 读到就停在恢复里等签名镜像。入口只有配置协议的"进恢复"命令与
 * （调试构建的）shell，见 doc/10-security.md 第 10 节。
 */
static void enter_serial_recovery(void)
{
	LOG_WRN("update trigger: rebooting into MCUboot serial recovery");

	/* RP2350 的看门狗在 SYSRESETREQ 之后可能还开着，留给 MCUboot 会打断它的
	 * 串口恢复；bootloader 侧另有一道护栏（lib/bootguard）。 */
	zlrs_watchdog_disarm();

	if (bootmode_set(BOOT_MODE_TYPE_BOOTLOADER) != 0) {
		LOG_ERR("boot mode write failed; MCUboot will boot the app again");
	}

	/* 让日志先出串口，再重启 */
	k_msleep(50);
	sys_reboot(SYS_REBOOT_COLD);
}
#endif

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
			   zlrs_radio_port_now_ms());
	memcpy(pairing.peer_id, link_control.peer_id, ZLRS_DEV_ID_LEN);
	memcpy(pairing.pair_salt, link_control.pair_salt, ZLRS_PAIR_SALT_LEN);
	(void)zlrs_store_save_pairing(&pairing);

	LOG_INF("paired with %02x%02x%02x%02x", link_control.peer_id[0],
		link_control.peer_id[1], link_control.peer_id[2], link_control.peer_id[3]);
}

/*
 * 按键只有一个动作：长按（按住到阈值）触发配对，见 doc/11-ux.md。
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
 * 后面配置协议可以改写它（见 doc/08-firmware.md 第 2 节的配置模式）。
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
	last_rc_input_ms = zlrs_radio_port_now_ms();
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

#if IS_ENABLED(CONFIG_ZLRS_APP_SUB_BAND_MIX)
	/*
	 * 子带混合自适应（doc/09-protocol.md 5.1）：LQ 掉了就把时隙往 sub-G 挪
	 * （顺序由链路层定：遥测 → 数传 → RC），LQ 好了再挪回来。上限受地区
	 * 占空比约束（zlrs_region_mix_level），每秒最多动一档。
	 */
	{
		static uint32_t mix_last_ms;
		static uint8_t mix_level;
		uint8_t want = mix_level;
		const uint32_t window_ms = (uint32_t)(4U * (frame_period_us / 1000U));
		const uint32_t airtime_us =
			zlrs_mod_airtime_us(&control_mod, CTL_FRAME_LEN);
		const uint8_t cap = zlrs_region_mix_level(CONTROL_ON_SUBG ? region : NULL,
							  window_ms, airtime_us);

		if (link_control.stats.lq_percent < 70U && mix_level < cap) {
			want = (uint8_t)(mix_level + 1U);
		} else if (link_control.stats.lq_percent > 90U && mix_level > 0U) {
			want = (uint8_t)(mix_level - 1U);
		}

		if (want != mix_level && (uint32_t)(now_ms - mix_last_ms) >= 2000U) {
			mix_level = want;
			mix_last_ms = now_ms;
			LOG_INF("sub-band mix -> %u (LQ %u%%, region cap %u)",
				mix_level, link_control.stats.lq_percent, cap);
			zlrs_link_set_mix(&link_control, mix_level);
			zlrs_link_set_mix(&link_data, mix_level);
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

	/*
	 * 每秒四条状态日志：上板调试开着（一眼看出链路在干什么），量产关掉。
	 * 注意这只影响"打不打日志"——速率档与动态功率的每秒评估在上面，跟它无关。
	 */
	if (!IS_ENABLED(CONFIG_ZLRS_APP_STATS_LOG)) {
		return;
	}

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
 * 状态灯：把链路状态翻译成 lib/led 的输入，见 doc/11-ux.md。
 * 每 20 Hz 刷一次（zlrs_led_port_update 内部限速）。
 */
#if IS_ENABLED(CONFIG_ZLRS_APP_CONFIG_MODE)
static void enter_config_mode(void);
#endif

/*
 * 致命错误（地区与 SKU 不匹配、熵源自检不过、PSA 起不来……）时的状态：
 * **报警但不锁死**——红灯 + 配置口继续在线，主机仍然能进配置模式、触发串口
 * 恢复、把设备刷回来。法规/安全问题宁可不停发，但要保证"永远刷得动"，
 * 否则一次坏镜像就得拆机按 BOOTSEL。
 */
static void fault_loop(void)
{
	zlrs_led_input_t in;

	memset(&in, 0, sizeof(in));
	in.failsafe = true; /* 红灯快闪 */
	in.link_state = ZLRS_STATE_SEARCHING;
	in.role_tx = IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX);
	in.uptime_ms = 60000U; /* 越过开机自检窗口，立刻进告警 */

	LOG_ERR("fault state: not transmitting; config port stays available for flashing");

	for (;;) {
		zlrs_watchdog_feed();
		zlrs_led_port_update(&in, k_uptime_get_32());

#if IS_ENABLED(CONFIG_ZLRS_APP_CONFIG_MODE)
		/* 配置口活着：进配置模式、`recovery` 命令、重新刷机都还能走 */
		if (zlrs_config_port_poll_enter()) {
			enter_config_mode();
		}
#endif

		k_msleep(20);
	}
}

/*
 * PSA provider 必须在第一次导入钥匙之前初始化；失败就别收发——KDF 与每包
 * tag 都靠它，闭着眼上空中没有意义。
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
	/* 发射端没有遥控器输入时就是在发失控值；接收端未连接时在往飞控输出失控值 */
	if (link_control.state == ZLRS_STATE_CONNECTED) {
		ever_connected = true;
	}
	/*
	 * 失控保护只在"连上过"之后才有意义：发射端连上过、但从遥控器收不到通道数据
	 * （在发失控值），或接收端连上过又掉线（在往飞控输出失控值）。从没连上过的
	 * 设备按普通链路状态显示——未配置紫、未配对黄、搜索青，别用红灯盖掉该做的动作。
	 */
	in.failsafe = ever_connected &&
		      (IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX)
			       ? !rc_input_seen
			       : (link_control.state != ZLRS_STATE_CONNECTED));

	zlrs_led_port_update(&in, now_ms);
}

/*
 * 镜像确认：初始化跑完就告诉 MCUboot "这一版是好的"——test/revert 的判据只有
 * 这一位，没跑到这里的镜像复位后会被回滚（见 doc/08-firmware.md 第 6 节）。
 * 判据是"初始化跑完"，要观察窗口就用 ZLRS_APP_MCUBOOT_CONFIRM_MS。
 */
static void confirm_image(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_ZLRS_APP_MCUBOOT_CONFIRM)
	static bool confirmed;

	if (confirmed || now_ms < (uint32_t)CONFIG_ZLRS_APP_MCUBOOT_CONFIRM_MS) {
		return;
	}

	confirmed = true;

	if (boot_write_img_confirmed() == 0) {
		LOG_INF("image confirmed: this build stays after a reset");
	} else {
		LOG_ERR("image confirm failed; MCUboot will revert on next reset");
	}
#else
	ARG_UNUSED(now_ms);
#endif
}

#if IS_ENABLED(CONFIG_ZLRS_APP_CONFIG_MODE)
/* shell 入口只置这个标志：会话要跑在主循环里（超帧停、数据口不被透传抢），
 * 见主循环里的处理 */
static atomic_t config_request;

/* 跑一段配置会话（EXIT 或空闲超时结束）后继续正常模式。三个入口共用 */
static void enter_config_mode(void)
{
	LOG_WRN("entering config mode");

	/* 0 = EXIT，1 = 超时，-1 = 没有可用数据口：日志里要能分辨 */
	{
		const int rc = zlrs_config_mode_run(&link_control, failsafe_payload);

		if (rc != 0) {
			LOG_WRN("config mode ended without EXIT (rc %d)", rc);
		}
	}
	memcpy(rc_payload, failsafe_payload, sizeof(rc_payload));
	zlrs_link_set_rc(&link_control, rc_payload, sizeof(rc_payload));

	LOG_INF("config mode done, back to normal");
}

/* ---- 配置协议要动的应用侧动作 ------------------------------------------ */

static int cfg_op_save_region(uint8_t id)
{
	const zlrs_region_t *const want = zlrs_region_get(id);

	if (want == NULL) {
		return ZLRS_CFG_E_PARAM;
	}

	/* 地区要的 sub-G 硬件必须与板子一致（-5 = SKU 不匹配），不匹配直接拒绝 */
	if (zlrs_region_validate(want, ZLRS_SUBG_BAND, tx_power_dbm) == -5) {
		return ZLRS_CFG_E_PARAM;
	}

	if (zlrs_store_save_region(id) != 0) {
		return ZLRS_CFG_E_STORE;
	}

	LOG_INF("region set to %s (takes effect on next boot)", want->name);

	return ZLRS_CFG_OK;
}

static int cfg_op_save_power(int8_t dbm)
{
	if (region == NULL || dbm > region->max_tx_power_dbm) {
		return ZLRS_CFG_E_PARAM;
	}

	if (zlrs_store_save_power(dbm) != 0) {
		return ZLRS_CFG_E_STORE;
	}

	LOG_INF("tx power set to %d dBm (takes effect on next boot)", dbm);

	return ZLRS_CFG_OK;
}

static int cfg_op_erase_pair(void)
{
	if (zlrs_store_erase_pairing() != 0) {
		return ZLRS_CFG_E_STORE;
	}

	LOG_INF("pairing erased (takes effect on next boot)");

	return ZLRS_CFG_OK;
}

static int cfg_op_factory_reset(void)
{
	if (zlrs_store_erase_all() != 0) {
		return ZLRS_CFG_E_STORE;
	}

	LOG_WRN("factory reset: NVS cleared (takes effect on next boot)");

	return ZLRS_CFG_OK;
}

#if IS_ENABLED(CONFIG_ZLRS_APP_UPDATE_TRIGGER)
static int cfg_op_recovery(void)
{
	enter_serial_recovery(); /* 不会返回 */

	return ZLRS_CFG_OK;
}
#endif

static uint8_t cfg_op_region(void)
{
	return region_id;
}

static int8_t cfg_op_power(void)
{
	return tx_power_dbm;
}

static const zlrs_config_ops_t cfg_ops = {
	.save_region = cfg_op_save_region,
	.save_power = cfg_op_save_power,
	.erase_pair = cfg_op_erase_pair,
	.factory_reset = cfg_op_factory_reset,
#if IS_ENABLED(CONFIG_ZLRS_APP_UPDATE_TRIGGER)
	.enter_recovery = cfg_op_recovery,
#endif
	.region = cfg_op_region,
	.power = cfg_op_power,
};
#endif /* CONFIG_ZLRS_APP_CONFIG_MODE */

#if IS_ENABLED(CONFIG_ZLRS_APP_SHELL)
static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "role   : %s", IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX) ? "TX" : "RX");
	shell_print(sh, "sub-G  : %s (band %d)", ZLRS_SUBG_BAND == ZLRS_BAND_SUBG_LF ? "LF" : "HF",
		    ZLRS_SUBG_BAND);
	shell_print(sh, "region : %s (max %d dBm)",
		    (region != NULL) ? region->name : "?", region->max_tx_power_dbm);
	shell_print(sh, "control: state %d, lq %u%%, paired %d",
		    link_control.state, link_control.stats.lq_percent, link_control.paired);
	shell_print(sh, "data   : state %d, drop %u B", link_data.state,
		    link_data.stats.data_drop_bytes);
	shell_print(sh, "radio  : %s", IS_ENABLED(CONFIG_ZLRS_APP_RADIO) ? "on" : "off (bring-up)");

	return 0;
}

#if IS_ENABLED(CONFIG_ZLRS_APP_CONFIG_MODE)
static int cmd_config(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	atomic_set(&config_request, 1);
	shell_print(sh, "已请求配置模式：数据口改跑配置协议，EXIT 回来");

	return 0;
}
#endif

/*
 * 数据口两块端点的状态：上板调试用。端点二选一在这个板上被坑过两次——
 * 先看 DTR（usbipd 上送不到）、后加"收到过主机字节"。这条命令一次把判据摊开。
 */
static int cmd_ports(const struct shell *sh, size_t argc, char **argv)
{
	zlrs_port_state_t states[2];
	const size_t count = zlrs_data_port_state(states, ARRAY_SIZE(states));

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (count == 0U) {
		shell_print(sh, "数据口没有可用端点");
		return 0;
	}

	for (size_t i = 0U; i < count; i++) {
		shell_print(sh, "%-16s ready=%d active=%d rx_total=%u rx_dropped=%u",
			    states[i].name, states[i].ready, states[i].active,
			    states[i].rx_total, states[i].rx_dropped);
	}

	return 0;
}

#if IS_ENABLED(CONFIG_ZLRS_APP_UPDATE_TRIGGER)
static int cmd_recovery(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "重启进 MCUboot 串口恢复");
	enter_serial_recovery(); /* 不会返回 */

	return 0;
}
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_ENTROPY_TRNG)
static int cmd_entropy(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (entropy_selftest() != 0) {
		shell_print(sh, "entropy: FAIL");
		return -1;
	}

	shell_print(sh, "entropy: ok");

	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	zlrs_cmds,
	SHELL_CMD(status, NULL, "角色 / SKU / 地区 / 链路状态", cmd_status),
	SHELL_CMD(ports, NULL, "数据口两块端点的状态", cmd_ports),
#if IS_ENABLED(CONFIG_ZLRS_APP_CONFIG_MODE)
	SHELL_CMD(config, NULL, "进配置模式（EXIT 回正常）", cmd_config),
#endif
#if IS_ENABLED(CONFIG_ZLRS_APP_UPDATE_TRIGGER)
	SHELL_CMD(recovery, NULL, "重启进 MCUboot 串口恢复", cmd_recovery),
#endif
#if IS_ENABLED(CONFIG_ZLRS_APP_ENTROPY_TRNG)
	SHELL_CMD(entropy, NULL, "熵源自检（TRNG 读两块 32 字节）", cmd_entropy),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(zlrs, &zlrs_cmds, "zlrs 调试命令", NULL);
#endif /* CONFIG_ZLRS_APP_SHELL */

#if IS_ENABLED(CONFIG_ZLRS_APP_ENTROPY_TRNG)
/*
 * 熵源自检：连续读两块 32 字节，必须成功且互不相同。
 * 失败就报错——**不做软件兜底**：硬件熵源坏了就是坏了，宁可停机也不用假熵，
 * 否则密钥、设备标识、配对全都建立在"看起来能跑"的随机数上。
 */
static int entropy_selftest(void)
{
	const struct device *const dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
	uint8_t a[32];
	uint8_t b[32];

	if (!device_is_ready(dev)) {
		LOG_ERR("entropy device not ready");
		return -1;
	}

	if (entropy_get_entropy(dev, a, sizeof(a)) != 0 ||
	    entropy_get_entropy(dev, b, sizeof(b)) != 0) {
		LOG_ERR("entropy self-test failed: TRNG read error");
		return -1;
	}

	if (memcmp(a, b, sizeof(a)) == 0) {
		LOG_ERR("entropy self-test failed: two identical blocks");
		return -1;
	}

	LOG_INF("entropy self-test ok");

	return 0;
}
#endif /* CONFIG_ZLRS_APP_ENTROPY_TRNG */

int main(void)
{
	/* 上板调试档：USB 已就绪，先等主机打开控制台端口，否则接下来的 panic
	 * 日志会打在没人打开的口上。生产构建为 0 */
	if (CONFIG_ZLRS_APP_BOOT_DELAY_MS > 0) {
		k_msleep(CONFIG_ZLRS_APP_BOOT_DELAY_MS);
	}

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
	if (!IS_ENABLED(CONFIG_ZLRS_APP_RADIO)) {
		LOG_WRN("bring-up: radio off, no USP/RAC and no LR2021 traffic");
	}

	/*
	 * NVS 先起来：地区、功率、密钥、配对、失控值、波特率都从它读。挂了不至于
	 * 不能跑，但必须说清楚后果：读不出来也存不下去，设备会停在"未配置"
	 * （不发射频）。store 自己也会打一条错误。
	 */
	if (zlrs_store_init() != 0) {
		LOG_ERR("store unavailable: keys, pairing and failsafe will not persist");
	}

#if IS_ENABLED(CONFIG_ZLRS_APP_CONFIG_MODE)
	/*
	 * 配置口尽早起来：后面任何一步判定致命（熵源、地区/SKU、PSA）都会进
	 * fault_loop，那条路径要靠配置口保持"刷得动"。
	 */
	(void)zlrs_config_port_init();
	zlrs_config_set_ops(&cfg_ops);
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_ENTROPY_TRNG)
	/* 熵源坏了就别往下走：后面的密钥生成与设备标识全依赖它 */
	if (entropy_selftest() != 0) {
		fault_loop();
	}
#endif

	/*
	 * 地区与功率上限是运行期参数（配置协议可写）：NVS 里有就用它，没有就用
	 * 编译期默认值。写入的取值在配置回调里校验（见 cfg_op_save_region）。
	 */
	region_id = CONFIG_ZLRS_APP_REGION;
	tx_power_dbm = CONFIG_ZLRS_APP_TX_POWER_DBM;
	{
		uint8_t stored_region;
		int8_t stored_power;

		if (zlrs_store_load_region(&stored_region) == (int)sizeof(stored_region)) {
			region_id = stored_region;
		}
		if (zlrs_store_load_power(&stored_power) == (int)sizeof(stored_power)) {
			tx_power_dbm = stored_power;
		}
	}
	region = zlrs_region_get(region_id);
	if (region == NULL) {
		LOG_ERR("unknown region %d", region_id);
		return 0;
	}
	/* 上板第一眼要看的两件事：这块板子是哪个 SKU、跑的是哪个地区 */
	LOG_INF("board: sub-G %s (band %d), region %s",
		ZLRS_SUBG_BAND == ZLRS_BAND_SUBG_LF ? "LF" : "HF", ZLRS_SUBG_BAND,
		region->name);
	{
		const int rc = zlrs_region_validate(region, ZLRS_SUBG_BAND, tx_power_dbm);

		/* -5：地区要的 sub-G 硬件跟这块板子不是一回事 → 不发射频 */
		if (rc == -5) {
			LOG_ERR("region %s needs another sub-G sku (board band %d)",
				region->name, ZLRS_SUBG_BAND);
			fault_loop();
		}
		if (rc == -2) {
			LOG_WRN("region %s caps tx power at %d dBm (configured %d)",
				region->name, region->max_tx_power_dbm, tx_power_dbm);
		} else if (rc != 0) {
			LOG_WRN("region check failed (%d): channel plan out of band", rc);
		}
	}
	zlrs_duty_init(&duty, region, 0U);
	if (IS_ENABLED(CONFIG_ZLRS_APP_STATUS_LED)) {
		(void)zlrs_led_port_init();
	}

	/* 等 USP/RAC 线程就绪，再初始化 RAC（见 doc/07-architecture.md 第 6 节） */
#if IS_ENABLED(CONFIG_ZLRS_APP_RADIO)
	SMTC_SW_PLATFORM_INIT();
	SMTC_SW_PLATFORM_VOID(smtc_rac_init());
#endif

	plan_init();
	build_links();
	crypto_init();
	load_identity();
	failsafe_init();

	/* 数据口必须先于配置模式初始化：按键入口就在下面，会话直接读数据口 */
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

	if (zlrs_radio_port_init(&link_control, &link_data) != 0) {
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

	/*
	 * 初始化到这里算跑完了（端口、NVS、链路、USB 都起来了）：确认镜像，
	 * 让 MCUboot 认这一版。
	 */
	confirm_image(zlrs_radio_port_now_ms());

	for (;;) {
		const int64_t start_us = k_uptime_get() * 1000;
		const uint32_t now_ms = zlrs_radio_port_now_ms();

		zlrs_watchdog_feed();
		confirm_image(now_ms);

#if IS_ENABLED(CONFIG_ZLRS_APP_CONFIG_MODE)
		/* 进配置模式的三个入口都收敛到这里：配置口命令、shell、调试魔数。
		 * 会话期间超帧暂停、射频停、数据口字节丢弃。 */
		if (zlrs_config_port_poll_enter() || atomic_cas(&config_request, 1, 0)) {
			enter_config_mode();
		}
#endif

		/* 四个时隙平分一帧：超预算的事务由 run_slot 放弃并收尾，不拖长整帧 */
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

/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * zlrs application entry point: runs the superframe and wires both links to the radio
 * and the serial ports. Threading model and init order: doc/07-architecture.md 6.
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

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/retention/bootmode.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_SHELL)
#include <zephyr/shell/shell.h>
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_RADIO)
#include <smtc_rac_api.h>
#include <smtc_sw_platform_helper.h>
#endif

#include "ports.h"
#include "caps.h"
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

/* Control frame plan: derived at boot from Kconfig and the region duty cap, see plan_init() */
static uint32_t control_rate_hz;
static uint32_t frame_period_us;
static uint32_t slot_budget_us;
static uint8_t tlm_every_n;
static uint8_t data_every_n;
static uint8_t resync_every_n;
/* Data payload cap: derived from the frame format and rate tier, not a Kconfig knob */
static uint16_t data_mtu;

/* Rough control frame airtime: 22 B RC + 3 B header + 4 B tag + 2 B CRC */
#define CTL_FRAME_LEN        31U
/* Telemetry frame: 8 B payload plus header and tag */
#define CTL_TLM_LEN          15U
/*
 * Bench bring-up: the two links can each run alone. Control carries RC and pairing,
 * data carries the passthrough; prove them one at a time before combining (doc/12-debug.md 3).
 */
#define ZLRS_CONTROL_ENABLED IS_ENABLED(CONFIG_ZLRS_APP_LINK_CONTROL)
#define ZLRS_DATA_ENABLED    IS_ENABLED(CONFIG_ZLRS_APP_LINK_DATA)
/* The CRSF port is part of the control link, and its presence is a devicetree fact: if the
 * board has the port at all, the control link uses it. */
#define ZLRS_CRSF_ENABLED (ZLRS_CONTROL_ENABLED && ZLRS_PORT_CTRL_FIXED)
/* Same shape for the config port: the CDC in devicetree is the whole answer */
#define ZLRS_CONFIG_ENABLED ZLRS_PORT_CONFIG

#define SERIAL_CHUNK_LEN 64U
#define REPORT_PERIOD_MS 1000U

static zlrs_link_t link_control;
static zlrs_link_t link_data;

/* Per-link modulation: fixed at boot from Kconfig and the region, see plan_init() */
static zlrs_mod_cfg_t control_mod;
static zlrs_mod_cfg_t data_mod;

/*
 * Band role (doc/07-architecture.md 2): control owns sub-G by default, data owns 2.4G;
 * racing mode swaps them. The regulatory gate (power, rate, duty) applies to the sub-G link.
 */
#if IS_ENABLED(CONFIG_ZLRS_APP_BAND_ROLE_SUBG_DATA)
#define CONTROL_ON_SUBG 0
#else
#define CONTROL_ON_SUBG 1
#endif

/* Region: picks the band, the power ceiling and the duty accounting */
static const zlrs_region_t *region;
static uint8_t region_id;
static int8_t tx_power_dbm;
static zlrs_duty_t duty;

/*
 * Role and security profile: runtime settings with a compile-time default (set-role /
 * set-profile, doc/organization.md 4). Loaded once at boot, before the links are built.
 */
static bool role_tx;
static uint8_t active_profile;

/* The profile a build starts on, and which ones it may run: P1 is the production profile, P0
 * carries no tag at all so only a debug image may select it, P2 is not implemented yet. */
#define ZLRS_PROFILE_DEFAULT                                                                       \
	(IS_ENABLED(CONFIG_ZLRS_APP_SECURITY_PROFILE_P0) ? ZLRS_PROFILE_P0 : ZLRS_PROFILE_P1)

static bool profile_allowed(uint8_t profile)
{
	if (profile == ZLRS_PROFILE_P1) {
		return true;
	}
	if (profile == ZLRS_PROFILE_P0) {
		return IS_ENABLED(CONFIG_ZLRS_APP_DEBUG);
	}

	return false;
}

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(zlrs_button), gpios);
static bool button_down;
static bool button_fired; /* pairing already fired for this press */
static uint32_t button_since_ms;

/* CRSF port I/O (defined below, called from the link callbacks) */
static void crsf_send_rc(const uint8_t *payload, uint8_t len);
static void crsf_send_link_stats(const zlrs_tlm_t *tlm);

#if IS_ENABLED(CONFIG_ZLRS_RP2350_TRNG)
/* Entropy self-test (defined above main); used by the boot check and the shell command */
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
/* Failsafe means nothing before a first connection: show unconfigured/unpaired/searching, not red */
static bool ever_connected;
static uint32_t led_drop_ms;

/*
 * Rate adaptation tiers, fastest first. Thresholds wait for measured PER curves,
 * see doc/13-roadmap.md; the numbers here are placeholders, the mechanism ignores them.
 */
#if IS_ENABLED(CONFIG_ZLRS_APP_RATE_ADAPT)
static const zlrs_rate_tier_t rate_tiers[] = {
	{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 2600000U}, .min_lq_to_stay = 85U},
	{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 1300000U}, .min_lq_to_stay = 60U},
	{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 650000U}, .min_lq_to_stay = 35U},
	{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 325000U}, .min_lq_to_stay = 0U},
};
static zlrs_rate_ctl_t rate_ctl;
#endif

/* Dynamic power: the TX follows the RSSI/LQ the RX reports back */
#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
static zlrs_power_ctl_t power_ctl;
/* Uplink quality from the latest telemetry; the verdict runs at 1 Hz, so stash it here */
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

/* ---- link callbacks (run on the link thread) ------------------------------ */

static void on_rc(void *ctx, const uint8_t *payload, uint16_t len)
{
	(void)ctx;

	counters.rc_rx++;
	/* RX: rebuild the air-side channel payload into a CRSF frame for the flight controller */
	crsf_send_rc(payload, (uint8_t)len);
}

static void on_tlm(void *ctx, const uint8_t *payload, uint16_t len)
{
	zlrs_tlm_t tlm;

	(void)ctx;

	counters.tlm_rx++;
	/* TX: turn the RX uplink quality into the link-stats frame the handset reads */
	if (zlrs_tlm_decode(payload, len, &tlm)) {
		crsf_send_link_stats(&tlm);

#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
		/* Just stash it: the verdict runs in report() at 1 Hz, see doc/09-protocol.md 4.5 */
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

/* RX: failsafe values arrive from the TX during pairing; store them and use them right away */
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
 * Control frame plan: rate tier (Kconfig) x region duty cap. Over the cap we clamp to it and
 * warn: the regulation wins, but the user gets to see it.
 */
/*
 * SEARCHING sync period follows the modulation (doc/09-protocol.md 3.6):
 * the slower the rate the longer a frame takes, so the interval widens to 100-600 ms.
 */
static uint16_t sync_period_for_mod(const zlrs_mod_cfg_t *mod)
{
	if (mod->modulation != ZLRS_MOD_FLRC) {
		return 600U; /* LoRa: a frame is tens of ms, a tighter cadence buys nothing */
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
	/* The regulatory gate follows the sub-G link, which is the data link in racing mode */
	const zlrs_region_t *ctl_region = CONTROL_ON_SUBG ? region : NULL;
	const zlrs_region_t *data_region = CONTROL_ON_SUBG ? NULL : region;
	zlrs_plan_req_t req = {
		.region = ctl_region,
		.frame_len = CTL_FRAME_LEN,
		.tlm_len = CTL_TLM_LEN,
	};

	/*
	 * Modulation is per link (doc/07-architecture.md 3): where the band cannot fit wide
	 * FLRC the control link falls back to the LoRa tier; data always stays on 2.4G FLRC.
	 */
#if IS_ENABLED(CONFIG_ZLRS_APP_BAND_ROLE_SUBG_DATA)
	/* Racing mode: control owns 2.4G and uses its own rate/frame-rate knobs */
	control_mod = zlrs_mod_flrc(CONFIG_ZLRS_APP_RACING_CONTROL_RATE_BPS);
	req.want_hz = CONFIG_ZLRS_APP_RACING_CONTROL_RATE_HZ;
#elif IS_ENABLED(CONFIG_ZLRS_APP_CONTROL_MODULATION_LORA)
	control_mod = zlrs_region_clamp_mod(ctl_region,
					    zlrs_mod_lora(CONFIG_ZLRS_APP_CONTROL_LORA_SF,
							  CONFIG_ZLRS_APP_CONTROL_LORA_BW_HZ,
							  CONFIG_ZLRS_APP_CONTROL_LORA_CR,
							  CONFIG_ZLRS_APP_CONTROL_LORA_PREAMBLE));
	req.want_hz = CONFIG_ZLRS_APP_LORA_CONTROL_RATE_HZ;
#else
	control_mod = zlrs_region_clamp_mod(ctl_region,
					    zlrs_mod_flrc(zlrs_region_clamp_rate(
						    ctl_region, CONFIG_ZLRS_APP_CONTROL_RATE_BPS)));
	req.want_hz = CONFIG_ZLRS_APP_CONTROL_RATE_HZ;
#endif
	data_mod =
		zlrs_mod_flrc(zlrs_region_clamp_rate(data_region, CONFIG_ZLRS_APP_DATA_RATE_BPS));
	req.mod = &control_mod;

	/*
	 * MTU follows the frame format and rate tier (doc/09-protocol.md 4.3): the largest payload
	 * that fits the one-way data window budget (a quarter frame), capped by the format.
	 */
	{
		/*
		 * Budget the data window against the frame rate this build actually asks for: the
		 * rate knob differs per mode (racing, LoRa, plain FLRC), and only req.want_hz knows.
		 */
		const uint32_t budget_us = (1000000U / req.want_hz) / 4U;
		uint16_t len = (uint16_t)(ZLRS_PAYLOAD_MAX_LEN - 2U);

		while (len > 16U &&
		       zlrs_mod_airtime_us(&data_mod, (uint16_t)(len + 7U)) > budget_us) {
			len = (uint16_t)(len - 8U);
		}

		data_mtu = len;
	}

	/*
	 * The superframe period is set by the slower link: the two data windows run once per
	 * data_every_n frames, amortised over each frame; core/region iterates the plan to agreement.
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
		LOG_WRN("region %s duty %u%%: control rate %u Hz -> %u Hz", region->name,
			region->duty_percent, (unsigned int)req.want_hz,
			(unsigned int)plan.rate_hz);
	}
	if (plan.clamped_by_airtime) {
		LOG_WRN("airtime (mod %u): control rate %u Hz -> %u Hz",
			(unsigned int)control_mod.modulation, (unsigned int)req.want_hz,
			(unsigned int)plan.rate_hz);
	}

	/* CONNECTED sync period converted to frames (doc/09-protocol.md 5.1): 1000 ms fits an
	 * uint8 at common frame periods, shorter periods get clamped to 255 frames. */
	if (CONFIG_ZLRS_APP_RESYNC_PERIOD_MS > 0) {
		const uint32_t frames =
			((uint32_t)CONFIG_ZLRS_APP_RESYNC_PERIOD_MS * 1000U) / frame_period_us;

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

/* ---- link setup ---------------------------------------------------------- */

static void build_links(void)
{
	zlrs_link_cfg_t ctl_cfg = {
		.kind = ZLRS_LINK_CONTROL,
		.role = role_tx ? ZLRS_ROLE_TX : ZLRS_ROLE_RX,
		/* Band role: control on sub-G by default, swapped in racing mode (doc/01-product.md 3.2) */
		.domain = CONTROL_ON_SUBG ? region->domain : ZLRS_DOMAIN_ISM2400,
		.second_domain = CONTROL_ON_SUBG ? ZLRS_DOMAIN_ISM2400 : region->domain,
		.sync_slot = ZLRS_SYNC_SLOT_CONTROL,
		.profile = active_profile,
		/* Bench switches, each can be turned off, see doc/12-debug.md 3 */
		.hopping = IS_ENABLED(CONFIG_ZLRS_APP_FHSS) ? 1U : 0U,
		.handshake = IS_ENABLED(CONFIG_ZLRS_APP_SESSION_HANDSHAKE) ? 1U : 0U,
		.telemetry = IS_ENABLED(CONFIG_ZLRS_APP_TELEMETRY) ? 1U : 0U,
		/* Modulation: narrow-band regions get the LoRa tier, wide ones FLRC (see plan_init) */
		.mod = control_mod,
		.tx_power_dbm = CONTROL_ON_SUBG ? MIN(tx_power_dbm, region->max_tx_power_dbm)
						: tx_power_dbm,
		.rx_timeout_ms = 2U,
		.sync_period_ms = sync_period_for_mod(&control_mod),
		/*
		 * Telemetry is the TX-side liveness proof: with it off there is nothing to hear, so the
		 * peer-loss check has to go too, or every frame drags the TX back into SEARCHING.
		 */
		.peer_timeout_ms = (role_tx &&
				    IS_ENABLED(CONFIG_ZLRS_APP_TELEMETRY))
					   ? 250U
					   : (role_tx ? 0U : 100U),
		.pair_period_ms = 100U,
		.pair_window_ms = 30000U,
		.data_mtu = 0U,
		/* Cadence thinning: see plan_init(); both ends run the same firmware so they agree */
		.tlm_every_n = tlm_every_n,
		.data_every_n = 1U, /* unused on the control link */
		/* Periodic sync while connected: re-align the hop index, auto-reconnect after a peer reboot */
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
		CONTROL_ON_SUBG ? tx_power_dbm : MIN(tx_power_dbm, region->max_tx_power_dbm);
	data_cfg.arq = IS_ENABLED(CONFIG_ZLRS_APP_DATA_ARQ) ? 1U : 0U;
	/*
	 * ARQ retransmit timeout derives from the tier: one airtime plus a round trip, floor 30 ms
	 * (which is what the sub-millisecond FLRC frames end up using).
	 */
	data_cfg.arq_timeout_ms = (uint16_t)MAX(ZLRS_ARQ_TIMEOUT_MS,
						2U * zlrs_mod_airtime_ms(&data_mod, data_mtu + 7U));
	/*
	 * The data link is best-effort: it sends nothing when idle, so it has no peer-loss check,
	 * which would otherwise rebuild the session over and over while idle (doc/09-protocol.md 3.7).
	 */
	data_cfg.peer_timeout_ms = 0U;
	data_cfg.data_mtu = data_mtu;
	data_cfg.sync_period_ms = sync_period_for_mod(&data_mod);
	data_cfg.tlm_every_n = 1U; /* no telemetry slot on the data link */
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
		zlrs_rate_init(&rate_ctl, rate_tiers, (uint8_t)ARRAY_SIZE(rate_tiers), start, 0U);
	}
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
	/* Ceiling is min(configured power, region cap); the floor is a Kconfig knob */
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

	/* Root key: NVS only, written once with the config tool (set-phrase / set-key-hex) */
	if (zlrs_store_load_root_key(root_key) == (int)ZLRS_KEY_LEN) {
		have_root_key = true;
	}

	/* Device id: generated on first boot and stored in NVS, see doc/10-security.md 2 */
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

	/* Pairing record: peer dev_id plus the two pairing nonces, loaded into both links */
	if (zlrs_store_load_pairing(&pairing) == (int)sizeof(pairing)) {
		zlrs_link_set_peer(&link_control, pairing.peer_id, pairing.pair_salt, now_ms);
		zlrs_link_set_peer(&link_data, pairing.peer_id, pairing.pair_salt, now_ms);
		LOG_INF("pairing record restored");
	}
}

/* ---- superframe ---------------------------------------------------------- */

/* Tick while pumping the RAC engine: fine enough not to stretch a slot, coarse enough not to spin */
#define ZLRS_RADIO_PUMP_STEP_US 200U

static void run_slot(uint8_t slot)
{
	const bool data_slot = (slot == ZLRS_SLOT_DATA_A || slot == ZLRS_SLOT_DATA_B);
	zlrs_link_t *link = data_slot ? &link_data : &link_control;
	const uint32_t now_ms = zlrs_radio_port_now_ms();
	/* The duty-cycle gate only applies to whichever link is on sub-G */
	const bool on_subg = data_slot ? (CONTROL_ON_SUBG == 0) : (CONTROL_ON_SUBG != 0);
	const zlrs_radio_req_t *req;

	if (data_slot ? !ZLRS_DATA_ENABLED : !ZLRS_CONTROL_ENABLED) {
		return; /* this link is not enabled in this build */
	}

	req = zlrs_link_slot(link, slot, now_ms);

	if (req == NULL) {
		return;
	}

	/* Out of duty budget: give the slot up (finish as a radio failure, the link retries next
	 * round). Only constrains the sub-G link. */
	if (req->is_tx && on_subg &&
	    !zlrs_duty_reserve(&duty, zlrs_airtime_mod_ms(&req->mod, req->size), now_ms)) {
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, now_ms);
		return;
	}

	if (link->radio.submit(link->radio.ctx, req) != 0) {
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, zlrs_radio_port_now_ms());
		return;
	}

	/*
	 * Pump the RAC engine while waiting for the transaction to finish. The vendor's
	 * immediate_radio_access sample is exactly "run_engine + wait for event": without it the
	 * planner never keys the radio and the transaction sits in RP_STATUS_TASK_INIT forever.
	 * The done_sem timeout doubles as the tick; the whole wait counts against the slot budget.
	 */
	{
		uint32_t waited_us = 0U;
		bool done = false;

		while (waited_us < slot_budget_us) {
#if IS_ENABLED(CONFIG_ZLRS_APP_RADIO)
			/* Only the RAC backend needs pumping; the null backend completes inline. */
			SMTC_SW_PLATFORM_VOID(smtc_rac_run_engine());
#endif

			if (zlrs_radio_port_wait_us(ZLRS_RADIO_PUMP_STEP_US) == 0) {
				done = true;
				break;
			}

			waited_us += ZLRS_RADIO_PUMP_STEP_US;
		}

		if (!done) {
			/* Not back within budget: abort and finish the link transaction so nothing stalls */
			zlrs_radio_port_abort();
			zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0,
					   zlrs_radio_port_now_ms());
			return;
		}
	}

	zlrs_radio_port_drain();
}

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
/*
 * Reboot into MCUboot serial recovery: set the boot-mode flag (retained SRAM, survives a reset),
 * which makes MCUboot wait for a signed image. The only entries are the config protocol's
 * recovery command and the debug shell, see doc/10-security.md 10.
 */
static void enter_serial_recovery(void)
{
	LOG_WRN("update trigger: rebooting into MCUboot serial recovery");

	/* RP2350's watchdog may still be armed after SYSRESETREQ; leaving it for MCUboot would
	 * cut its serial recovery short (subsys/bootguard is the bootloader-side guard). */
	zlrs_watchdog_disarm();

	if (bootmode_set(BOOT_MODE_TYPE_BOOTLOADER) != 0) {
		LOG_ERR("boot mode write failed; MCUboot will boot the app again");
	}

	/* Let the log reach the UART before resetting */
	k_msleep(50);
	sys_reboot(SYS_REBOOT_COLD);
}
#endif

static void pump_serial(void)
{
	uint8_t chunk[SERIAL_CHUNK_LEN];
	uint16_t got;

	if (!ZLRS_DATA_ENABLED) {
		return; /* no data link: nothing to forward from the UART */
	}

	got = zlrs_data_port_read(chunk, sizeof(chunk));

	if (got > 0U) {
		(void)zlrs_link_write(&link_data, chunk, got); /* queue full: drop the tail */
	}
}

static void adopt_pairing(void)
{
	zlrs_store_pairing_t pairing;

	/* Control produces the pairing record, data adopts it; skip when either link is off */
	if (!ZLRS_CONTROL_ENABLED || !ZLRS_DATA_ENABLED) {
		return;
	}

	if (!link_control.paired) {
		return;
	}
	/* Re-pairing changes the dev_id or the nonces, so the data link has to re-sync */
	if (link_data.paired &&
	    memcmp(link_data.peer_id, link_control.peer_id, ZLRS_DEV_ID_LEN) == 0 &&
	    memcmp(link_data.pair_salt, link_control.pair_salt, ZLRS_PAIR_SALT_LEN) == 0) {
		return;
	}

	/* After pairing succeeds on the control link, push the record to the data link and store it */
	zlrs_link_set_peer(&link_data, link_control.peer_id, link_control.pair_salt,
			   zlrs_radio_port_now_ms());
	memcpy(pairing.peer_id, link_control.peer_id, ZLRS_DEV_ID_LEN);
	memcpy(pairing.pair_salt, link_control.pair_salt, ZLRS_PAIR_SALT_LEN);
	(void)zlrs_store_save_pairing(&pairing);

	LOG_INF("paired with %02x%02x%02x%02x", link_control.peer_id[0], link_control.peer_id[1],
		link_control.peer_id[2], link_control.peer_id[3]);
}

/*
 * The button has exactly one action: hold it to the threshold to start pairing (doc/11-ux.md).
 * No short press and no unpair gesture: hold again to re-pair, the new record replaces the old.
 */
static void handle_button(uint32_t now_ms)
{
	const int pressed = gpio_pin_get_dt(&button);

	/* Bench builds can drop button pairing; the button then does nothing at all */
	if (!ZLRS_HAS_BUTTON || !ZLRS_CONTROL_ENABLED) {
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
		LOG_INF("pairing (button held %u ms)", (unsigned int)(now_ms - button_since_ms));
		zlrs_link_press(&link_control, now_ms);
		return;
	}

	if (pressed == 0 && button_down) {
		button_down = false;
	}
}

/*
 * Failsafe values default to throttle low and everything else centred, which is what a
 * handset sends with its link down. The config protocol can overwrite them (08-firmware 3).
 */
static void failsafe_init(void)
{
	uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT];

	if (zlrs_store_load_failsafe(failsafe_payload) != (int)sizeof(failsafe_payload)) {
		/* Never configured: use the default (throttle low, the rest centred) */
		zlrs_crsf_channels_default(channels);
		zlrs_crsf_channels_pack(failsafe_payload, channels);
	}
	memcpy(rc_payload, failsafe_payload, sizeof(rc_payload));

	/* TX side: these values travel to the RX with the next pairing */
	zlrs_link_set_failsafe(&link_control, failsafe_payload, sizeof(failsafe_payload));
}

/* TX: RC frames from the handset become air-side RC payloads */
static void crsf_handle_input(uint8_t type, const uint8_t *payload, uint8_t len)
{
	if (type != ZLRS_CRSF_TYPE_RC_CHANNELS) {
		return; /* other handset/flight-controller frames are not consumed by design */
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
 * TX: link statistics for the handset (0x14).
 * Uplink numbers come from RX telemetry, downlink ones are measured here — each side reports
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
	(void)zlrs_crsf_port_write(ZLRS_CRSF_TYPE_LINK_STATISTICS, payload, sizeof(payload));
}

/* RX: rebuild the received RC payload into a CRSF frame for the flight controller */
static void crsf_send_rc(const uint8_t *payload, uint8_t len)
{
	if (!ZLRS_CRSF_ENABLED || len != ZLRS_CRSF_RC_PAYLOAD_LEN) {
		return;
	}

	(void)zlrs_crsf_port_write(ZLRS_CRSF_TYPE_RC_CHANNELS, payload, len);
}

/* RX: report the uplink quality measured on this side back to the TX */
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

/* CRSF task in the main loop: read frames, TX failsafe timeout, RX output and reporting */
static void pump_crsf(uint32_t now_ms)
{
	uint8_t type = 0U;
	uint8_t payload[ZLRS_CRSF_PAYLOAD_MAX_LEN];
	uint8_t len;

	/* Bench builds can drop the CRSF port: the TX then sends only the configured failsafe values */
	if (!ZLRS_CRSF_ENABLED || !ZLRS_CONTROL_ENABLED) {
		return;
	}

	if (!zlrs_crsf_port_ready()) {
		return;
	}

	while ((len = zlrs_crsf_port_read(&type, payload, sizeof(payload))) > 0U) {
		crsf_handle_input(type, payload, len);
	}

	if (role_tx) {
		/* Handset unplugged or powered off: switch to the preset failsafe values at once */
		if (rc_input_seen &&
		    (uint32_t)(now_ms - last_rc_input_ms) > CONFIG_ZLRS_APP_CRSF_TIMEOUT_MS) {
			rc_input_seen = false;
			zlrs_link_set_rc(&link_control, failsafe_payload, sizeof(failsafe_payload));
			LOG_WRN("CRSF input timeout: sending failsafe values");
		}
	} else {
		/* RX: keep emitting failsafe values while unconnected, so the flight controller stays safe */
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
	 * Sub-band mixing (doc/09-protocol.md 5.1): when LQ drops, move slots to sub-G (the link
	 * layer picks the order: telemetry, then data, then RC) and move them back when LQ recovers.
	 * The region duty cap (zlrs_region_mix_level) bounds it; at most one step per second.
	 */
	{
		static uint32_t mix_last_ms;
		static uint8_t mix_level;
		uint8_t want = mix_level;
		const uint32_t window_ms = (uint32_t)(4U * (frame_period_us / 1000U));
		const uint32_t airtime_us = zlrs_mod_airtime_us(&control_mod, CTL_FRAME_LEN);
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
			LOG_INF("sub-band mix -> %u (LQ %u%%, region cap %u)", mix_level,
				link_control.stats.lq_percent, cap);
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
		 * Evaluated once a second: if telemetry arrived, judge quality from its RSSI/LQ,
		 * otherwise take the valid=false timeout path, which goes back to the ceiling.
		 */
		if (zlrs_power_update(&power_ctl, fresh, tlm_latest.rssi_dbm, tlm_latest.lq, now_ms,
				      &next)) {
			zlrs_link_set_power(&link_control, next);
			LOG_INF("tx power -> %d dBm (rssi %d dBm, lq %u%%)", next,
				tlm_latest.rssi_dbm, tlm_latest.lq);
		}
	}
#endif

	/*
	 * The once-a-second status lines: on for bench work (one glance says what the link is doing),
	 * off in production. This only gates the logging; the 1 Hz rate and power verdicts are separate.
	 */
	if (!IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)) {
		return;
	}

	LOG_INF("%s ctl=%u tlm=%u data=%u serial_drop=%u",
		role_tx ? "TX" : "RX", counters.rc_rx, counters.tlm_rx,
		counters.data_rx, zlrs_data_port_dropped());
	/* Frames that really went out: only transactions that completed count (see core/link) */
	LOG_INF("air: ctl_tx=%u ctl_rx=%u data_tx=%u data_rx=%u fail=%u+%u",
		link_control.stats.tx_frames, link_control.stats.rx_frames,
		link_data.stats.tx_frames, link_data.stats.rx_frames, link_control.stats.radio_fail,
		link_data.stats.radio_fail);
	LOG_INF("crsf_bad=%u crsf_drop=%u air_drop=%u lq=%u%%", crsf_bad_input,
		zlrs_crsf_port_rx_dropped(), link_data.stats.data_drop_bytes,
		link_control.stats.lq_percent);
	LOG_INF("region %s: max %d dBm, duty denied %u", (region != NULL) ? region->name : "?",
		region->max_tx_power_dbm, duty.denied);
#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
	LOG_INF("tx power %d dBm (up %u / down %u / fallback %u)",
		zlrs_power_current_dbm(&power_ctl), power_ctl.raises, power_ctl.lowers,
		power_ctl.fallbacks);
#endif
}

/*
 * Status LED: translate the link state into core/led inputs, see doc/11-ux.md.
 * Refreshed at 20 Hz (zlrs_led_port_update rate-limits internally).
 */
#if ZLRS_CONFIG_ENABLED
static void enter_config_mode(void);
#endif

/*
 * State on a fatal error (region vs SKU mismatch, entropy self-test failure, PSA init failure):
 * alarm but stay reachable. Red LED, config port still up, so the host can enter config mode,
 * trigger serial recovery and reflash. A regulatory or security problem is worth staying off
 * the air for, but the device must stay flashable — otherwise one bad image means BOOTSEL.
 *
 * Exception: a test image (not yet confirmed by the host) resets on a fatal error so that
 * MCUboot rolls back — a flashable fault state is no reason to keep a broken build running.
 */
static void fault_loop(void)
{
	zlrs_led_input_t in;

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	if (!boot_is_img_confirmed()) {
		/* Give the host 2 s to read the log or step in, then reset to roll back */
		LOG_ERR("fault on unconfirmed image: rebooting to roll back");
		k_msleep(2000);
		sys_reboot(SYS_REBOOT_COLD);
	}
#endif

	memset(&in, 0, sizeof(in));
	in.failsafe = true; /* fast red blink */
	in.link_state = ZLRS_STATE_SEARCHING;
	in.role_tx = role_tx;
	in.uptime_ms = 60000U; /* past the boot self-test window, so the alarm shows at once */

	LOG_ERR("fault state: not transmitting; config port stays available for flashing");

	for (;;) {
		zlrs_watchdog_feed();
		zlrs_led_port_update(&in, k_uptime_get_32());

#if ZLRS_CONFIG_ENABLED
		/* Config port alive: config mode, the recovery command and reflashing all still work */
		if (zlrs_config_port_poll_enter()) {
			enter_config_mode();
		}
#endif

		k_msleep(20);
	}
}

/*
 * The PSA provider must be initialised before the first key import; on failure stay off the
 * air: the KDF and the per-frame tags depend on it, so transmitting blind is pointless.
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

	if (!ZLRS_HAS_LED) {
		return;
	}

	if (drops != led_drop_seen) {
		led_drop_seen = drops;
		led_drop_ms = now_ms;
	}

	memset(&in, 0, sizeof(in));
	in.configured = link_control.configured;
	in.paired = link_control.paired;
	in.role_tx = role_tx;
	in.link_state = link_control.state;
	in.lq_percent = link_control.stats.lq_percent;
	in.uptime_ms = now_ms;
	in.drop_recent = (drops > 0U) && (uint32_t)(now_ms - led_drop_ms) < 1000U;
	/* TX without RC input sends failsafe values; an unconnected RX outputs them to the FC */
	if (link_control.state == ZLRS_STATE_CONNECTED) {
		ever_connected = true;
	}
	/*
	 * Failsafe only means something after a first connection: the TX was connected and then lost
	 * the handset (sending failsafe), or the RX was connected and dropped (outputting failsafe).
	 * A device that never connected shows its normal state (purple/yellow/cyan), never red.
	 */
	in.failsafe = ever_connected && (role_tx
						 ? !rc_input_seen
						 : (link_control.state != ZLRS_STATE_CONNECTED));

	zlrs_led_port_update(&in, now_ms);
}

/*
 * Image confirmation: an upgrade counts as successful only when the host really connected,
 * sent the explicit confirm command and got an answer — that flag is the whole test/revert
 * verdict. Upgrades run with test semantics: if the host never confirms, or the image never
 * comes up at all, the reset makes MCUboot roll back. See doc/08-firmware.md 6.
 */
static void confirm_image(void)
{
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	static bool confirmed;

	if (confirmed) {
		return;
	}

	/* Already confirmed (an ordinary boot, not an upgrade): nothing to do */
	if (boot_is_img_confirmed()) {
		confirmed = true;
		return;
	}

	if (zlrs_config_image_confirmed()) {
		confirmed = true;

		if (boot_write_img_confirmed() == 0) {
			LOG_INF("image confirmed by host: this build stays after a reset");
		} else {
			LOG_ERR("image confirm failed; MCUboot will revert on next reset");
		}
	} else if (k_uptime_get_32() >= (uint32_t)CONFIG_ZLRS_APP_MCUBOOT_CONFIRM_TIMEOUT_MS) {
		/* Never confirmed by the host: reset and let MCUboot roll back instead of clinging on */
		LOG_ERR("upgrade not confirmed by host: rebooting to roll back");
		sys_reboot(SYS_REBOOT_COLD);
	}
#else
#endif
}

#if ZLRS_CONFIG_ENABLED
/* The shell entry only sets this flag: the session runs in the main loop (superframe stopped,
 * data port not stolen by the passthrough), see the main loop. */
static atomic_t config_request;

/* Run one config session (ends on EXIT or idle timeout), then resume normal mode. Shared. */
static void enter_config_mode(void)
{
	LOG_WRN("entering config mode");

	/* 0 = EXIT, 1 = timeout, -1 = no usable port: the log has to tell them apart */
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

/* ---- application-side actions the config protocol triggers ----------------- */

static int cfg_op_save_region(uint8_t id)
{
	const zlrs_region_t *const want = zlrs_region_get(id);

	if (want == NULL) {
		return ZLRS_CFG_E_PARAM;
	}

	/* The region's sub-G hardware must match the board (-5 = SKU mismatch); refuse otherwise */
	if (zlrs_region_validate(want, ZLRS_RF_DESIGN, tx_power_dbm) == -5) {
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

static int cfg_op_save_role(uint8_t role)
{
	if (role > ZLRS_ROLE_RX) {
		return ZLRS_CFG_E_PARAM;
	}
	if (zlrs_store_save_role(role) != 0) {
		return ZLRS_CFG_E_STORE;
	}

	LOG_INF("role set to %s (takes effect on next boot)", (role == ZLRS_ROLE_TX) ? "TX" : "RX");

	return ZLRS_CFG_OK;
}

static int cfg_op_save_profile(uint8_t profile)
{
	/* Refusing an unavailable profile here is the second gate: the boot path also falls back
	 * when a stored value cannot be run (P0 outside debug images, P2 not implemented) */
	if (!profile_allowed(profile)) {
		return ZLRS_CFG_E_PARAM;
	}
	if (zlrs_store_save_profile(profile) != 0) {
		return ZLRS_CFG_E_STORE;
	}

	LOG_INF("security profile set to P%u (takes effect on next boot)", (unsigned int)profile);

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

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
static int cfg_op_recovery(void)
{
	enter_serial_recovery(); /* does not return */

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
	.save_role = cfg_op_save_role,
	.save_profile = cfg_op_save_profile,
	.erase_pair = cfg_op_erase_pair,
	.factory_reset = cfg_op_factory_reset,
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	.enter_recovery = cfg_op_recovery,
#endif
	.region = cfg_op_region,
	.power = cfg_op_power,
};
#endif /* ZLRS_CONFIG_ENABLED */

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_SHELL)
static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "role   : %s", role_tx ? "TX" : "RX");
	shell_print(sh, "sub-G  : %s (band %d)", ZLRS_RF_BAND == ZLRS_BAND_SUBG_LF ? "LF" : "HF",
		    ZLRS_RF_BAND);
	shell_print(sh, "region : %s (max %d dBm)", (region != NULL) ? region->name : "?",
		    region->max_tx_power_dbm);
	shell_print(sh, "control: state %d, lq %u%%, paired %d", link_control.state,
		    link_control.stats.lq_percent, link_control.paired);
	shell_print(sh, "data   : state %d, drop %u B", link_data.state,
		    link_data.stats.data_drop_bytes);
	shell_print(sh, "radio  : %s", IS_ENABLED(CONFIG_ZLRS_APP_RADIO) ? "on" : "off (bring-up)");

	return 0;
}

#if ZLRS_CONFIG_ENABLED
static int cmd_config(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	atomic_set(&config_request, 1);
	shell_print(sh, "config mode requested: the config CDC speaks the protocol, EXIT returns");

	return 0;
}
#endif

/*
 * Data port endpoint state, for bench debugging. Picking an endpoint bit us twice on this
 * board: first DTR (never reaches us over usbipd), then "host bytes seen". This dumps both.
 */
static int cmd_ports(const struct shell *sh, size_t argc, char **argv)
{
	zlrs_port_state_t states[2];
	const size_t count = zlrs_data_port_state(states, ARRAY_SIZE(states));

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (count == 0U) {
		shell_print(sh, "data port has no usable endpoint");
		return 0;
	}

	for (size_t i = 0U; i < count; i++) {
		shell_print(sh, "%-16s ready=%d active=%d rx_total=%u rx_dropped=%u",
			    states[i].name, states[i].ready, states[i].active, states[i].rx_total,
			    states[i].rx_dropped);
	}

	return 0;
}

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
static int cmd_recovery(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "rebooting into MCUboot serial recovery");
	enter_serial_recovery(); /* does not return */

	return 0;
}
#endif

#if IS_ENABLED(CONFIG_ZLRS_RP2350_TRNG)
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

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
/*
 * Debug helper: dump the image state as MCUboot sees it. First thing to look at when an
 * upgrade or rollback misbehaves; confirmed=1 means MCUboot has pinned this build for good.
 */
static void mcuboot_dump_slot(const struct shell *sh, const char *name, uint8_t area_id)
{
	const struct flash_area *fa;
	static uint8_t sector[4096];

	if (flash_area_open(area_id, &fa) != 0) {
		shell_print(sh, "%s: cannot open flash area %u", name, area_id);
		return;
	}

	/*
	 * MCUboot's trailer lives in the last sector of the slot. Scan for non-0xFF bytes so that the
	 * positions and values show up, which makes magic / image_ok / copy_done obvious.
	 */
	/* State may not sit in the last sector: scan 16 sectors back, list the non-0xFF bytes */
	shell_print(sh, "%s: non-0xFF bytes in the last 64 KB (offset from the slot end):", name);

	uint8_t shown = 0U;

	for (uint32_t back = 1U; back <= 16U && shown < 12U; back++) {
		const uint32_t off = fa->fa_size - back * sizeof(sector);

		if (flash_area_read(fa, off, sector, sizeof(sector)) != 0) {
			shell_print(sh, "  read at 0x%05x failed", off);
			break;
		}

		for (size_t i = 0U; i < sizeof(sector) && shown < 12U; i++) {
			if (sector[i] != 0xFFU) {
				shell_print(sh, "  -0x%05x = 0x%02x", back * 4096U - i, sector[i]);
				shown++;
			}
		}
	}

	if (shown == 0U) {
		shell_print(sh, "  (no writes found in these 64 KB)");
	}

	flash_area_close(fa);
}

static int cmd_mcuboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "active slot : %u", boot_fetch_active_slot());
	/* VTOR points at the slot really running (cross-check against MCUboot's active slot) */
	shell_print(sh, "VTOR        : 0x%08x (slot0=0x%08x, slot1=0x%08x)",
		    (unsigned int)SCB->VTOR,
		    (unsigned int)(0x10000000 + PARTITION_OFFSET(slot0_partition)),
		    (unsigned int)(0x10000000 + PARTITION_OFFSET(slot1_partition)));
	shell_print(sh, "confirmed   : %d", (int)boot_is_img_confirmed());
	shell_print(sh, "swap type   : %d (0=none 1=test 2=revert 3=fail 4=permanent)",
		    (int)mcuboot_swap_type());
	shell_print(sh, "confirmed by host: %d", (int)zlrs_config_image_confirmed());
	mcuboot_dump_slot(sh, "slot0", PARTITION_ID(slot0_partition));
	mcuboot_dump_slot(sh, "slot1", PARTITION_ID(slot1_partition));

	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	zlrs_cmds, SHELL_CMD(status, NULL, "role / SKU / region / link state", cmd_status),
	SHELL_CMD(ports, NULL, "state of both data port endpoints", cmd_ports),
#if ZLRS_CONFIG_ENABLED
	SHELL_CMD(config, NULL, "enter config mode (EXIT returns)", cmd_config),
#endif
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	SHELL_CMD(recovery, NULL, "reboot into MCUboot serial recovery", cmd_recovery),
#endif
#if IS_ENABLED(CONFIG_ZLRS_RP2350_TRNG)
	SHELL_CMD(entropy, NULL, "entropy self-test (two 32-byte TRNG blocks)", cmd_entropy),
#endif
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	SHELL_CMD(mcuboot, NULL, "image state as MCUboot sees it (debug)", cmd_mcuboot),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(zlrs, &zlrs_cmds, "zlrs debug commands", NULL);
#endif /* CONFIG_ZLRS_APP_DEBUG_SHELL */

#if IS_ENABLED(CONFIG_ZLRS_RP2350_TRNG)
/*
 * Entropy self-test: read two 32-byte blocks in a row; both must succeed and differ.
 * On failure, report it — no software fallback. A broken hardware entropy source is broken;
 * keys, device id and pairing must not rest on random numbers that merely look fine.
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
#endif /* CONFIG_ZLRS_RP2350_TRNG */

int main(void)
{
	/* Debug image only (gated in Kconfig, so the symbol does not exist in production builds):
	 * USB is up, so wait for the host to open the console port — otherwise a panic log lands
	 * on a port nobody has open. */
#ifdef CONFIG_ZLRS_APP_DEBUG_BOOT_DELAY_MS
	if (CONFIG_ZLRS_APP_DEBUG_BOOT_DELAY_MS > 0) {
		k_msleep(CONFIG_ZLRS_APP_DEBUG_BOOT_DELAY_MS);
	}
#endif

	LOG_INF("zlrs %s", APP_VERSION_STRING);

	/* What the build is, and what this board has: ports and peripherals come from devicetree,
	 * so a missing one is reported without being an error (app/src/caps.h) */
	if (!ZLRS_CONTROL_ENABLED) {
		LOG_WRN("bring-up: control link off, only the data link runs "
			"(pairing needs the control link)");
	}
	if (!ZLRS_DATA_ENABLED) {
		LOG_WRN("bring-up: data link off, no serial passthrough");
	}
	if (!ZLRS_CRSF_ENABLED) {
		LOG_WRN("no CRSF port on this board, TX sends the configured failsafe values");
	}
	if (!ZLRS_CONFIG_ENABLED) {
		LOG_WRN("no config port on this board, parameters can only come from the defaults");
	}

	/*
	 * Bring NVS up first: region, power, keys, pairing, failsafe values and baud all come from
	 * it. A broken NVS is survivable, but the consequences are worth spelling out: nothing can
	/*
	 * Bring NVS up first: region, power, keys, pairing, failsafe values and baud all come from
	 * it. A broken NVS is survivable, but the consequences are worth spelling out: nothing can
	 * be read or written, so the device stays unconfigured and off the air. store logs its own error.
	 */
	if (zlrs_store_init() != 0) {
		LOG_ERR("store unavailable: keys, pairing and failsafe will not persist");
	}

#if ZLRS_CONFIG_ENABLED
	/*
	 * Bring the config port up early: any later fatal step (entropy, region/SKU, PSA) lands in
	 * fault_loop, and that path stays flashable only while the config port is alive.
	 */
	(void)zlrs_config_port_init();
	zlrs_config_set_ops(&cfg_ops);
#endif

#if IS_ENABLED(CONFIG_ZLRS_RP2350_TRNG)
	/* A dead entropy source stops the boot: key generation and the device id depend on it */
	if (entropy_selftest() != 0) {
		fault_loop();
	}
#endif

	/*
	 * Region and power ceiling are runtime parameters (the config protocol writes them): use the
	 * stored value when present, the board's own default otherwise (app/src/caps.h).
	 * cfg_op_save_region validates whatever it is asked to store.
	 */
	region_id = ZLRS_DEFAULT_REGION;
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
	/* The first two things to look at on the bench: which SKU this board is, which region it runs */
	LOG_INF("board: sub-G %s (band %d), region %s",
		ZLRS_RF_BAND == ZLRS_BAND_SUBG_LF ? "LF" : "HF", ZLRS_RF_BAND, region->name);
	/* What the build believes the hardware is, derived from devicetree (app/src/caps.h) */
	LOG_INF("caps: ports=%s%s%s usb=%c btn=%c led=%c wdt=%c rf=%s(%u-%u MHz)",
		ZLRS_PORT_CTRL_FIXED ? "control" : "", ZLRS_PORT_DATA_FIXED ? ",data" : "",
		ZLRS_PORT_CONFIG ? ",cfg" : "", ZLRS_HAS_USB ? 'y' : 'n',
		ZLRS_HAS_BUTTON ? 'y' : 'n', ZLRS_HAS_LED ? 'y' : 'n', ZLRS_HAS_WDT ? 'y' : 'n',
		ZLRS_RF_BAND == ZLRS_BAND_SUBG_LF ? "LF" : "HF", ZLRS_RF_LOW_HZ / 1000000U,
		ZLRS_RF_HIGH_HZ / 1000000U);

	/*
	 * Role and profile: stored value wins, build default otherwise, and a stored value this
	 * image cannot run falls back to the default rather than going on air with something else.
	 */
	role_tx = IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX);
	active_profile = ZLRS_PROFILE_DEFAULT;
	{
		uint8_t stored_role;
		uint8_t stored_profile;

		if (zlrs_store_load_role(&stored_role) == (int)sizeof(stored_role) &&
		    stored_role <= ZLRS_ROLE_RX) {
			role_tx = (stored_role == ZLRS_ROLE_TX);
		}
		if (zlrs_store_load_profile(&stored_profile) == (int)sizeof(stored_profile)) {
			if (profile_allowed(stored_profile)) {
				active_profile = stored_profile;
			} else {
				LOG_WRN("stored profile P%u is not available in this image, using P%u",
					(unsigned int)stored_profile, (unsigned int)ZLRS_PROFILE_DEFAULT);
			}
		}
	}
	LOG_INF("role %s, profile P%u", role_tx ? "TX" : "RX", (unsigned int)active_profile);

	/* Bench switches: with these off the link is no longer production-shaped, so say so at boot */
	if (active_profile != ZLRS_PROFILE_P1) {
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
	if (!IS_ENABLED(CONFIG_ZLRS_APP_RADIO)) {
		LOG_WRN("bring-up: radio off, no USP/RAC and no LR2021 traffic");
	}
	{
		const int rc = zlrs_region_validate(region, ZLRS_RF_DESIGN, tx_power_dbm);

		/* -5: the region wants different sub-G hardware than this board has, so stay off the air */
		if (rc == -5) {
			LOG_ERR("region %s needs another sub-G sku (board band %d)", region->name,
				ZLRS_RF_BAND);
			fault_loop();
		}
		if (rc == -2) {
			LOG_WRN("region %s caps tx power at %d dBm (configured %d)", region->name,
				region->max_tx_power_dbm, tx_power_dbm);
		} else if (rc != 0) {
			LOG_WRN("region check failed (%d): channel plan out of band", rc);
		}
	}
	zlrs_duty_init(&duty, region, 0U);
	if (ZLRS_HAS_LED) {
		(void)zlrs_led_port_init();
	}

	/* Wait for the USP/RAC thread, then init RAC (see doc/07-architecture.md 6) */
#if IS_ENABLED(CONFIG_ZLRS_APP_RADIO)
	SMTC_SW_PLATFORM_INIT();
	SMTC_SW_PLATFORM_VOID(smtc_rac_init());
#endif

	plan_init();
	build_links();
	crypto_init();
	load_identity();
	failsafe_init();

	/* The data port must come up before config mode: the session reads that port directly */
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

	LOG_INF("entering superframe loop: %u Hz, %s / %s (%s)", (unsigned int)control_rate_hz,
		ZLRS_CONTROL_ENABLED ? "control on" : "control off",
		ZLRS_DATA_ENABLED ? "data on" : "data off",
		role_tx ? "TX" : "RX");

	/*
	 * Init is done at this point (ports, NVS, links, USB). Image confirmation does not happen
	 * here: after an upgrade it waits for the host's explicit confirm (see confirm_image).
	 */

	for (;;) {
		const int64_t start_us = k_uptime_get() * 1000;

		zlrs_watchdog_feed();
		confirm_image();

#if ZLRS_CONFIG_ENABLED
		/* All config-mode entries converge here: config port command, shell, debug magic.
		 * During the session the superframe pauses, the radio stops, the data port drops bytes. */
		if (zlrs_config_port_poll_enter() || atomic_cas(&config_request, 1, 0)) {
			enter_config_mode();
		}
#endif

		/*
		 * The timestamp must be re-read after a config session. Anything stamped inside the session
		 * (a pairing triggered from the config port) is later than the "now" taken before it, so
		 * unsigned subtraction underflows and the pairing window looks long expired.
		 */
		const uint32_t now_ms = zlrs_radio_port_now_ms();

		/* Four slots share a frame: over-budget transactions are dropped, they do not stretch it */
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

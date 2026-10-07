/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * zlrs application entry point: runs the superframe and wires both links to the radio
 * and the serial ports. Threading model and init order: doc/06-architecture.md 6.
 */

#include <zephyr/app_version.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/reboot.h>

#include <string.h>

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
#include <stdarg.h>
#include <stdio.h>
#endif

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/retention/bootmode.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_SHELL)
#include <zephyr/shell/shell.h>
#endif

#ifdef PICO_RP2350
#include <pico/bootrom.h>
#endif

#include "mgmt/mode.h"
#include "link/crsf.h"
#include "link/data.h"
#include "link/power.h"
#include "link/rate.h"
#include "platform/caps.h"
#include "platform/recovery.h"
#include "platform/slot_timer.h"
#include "platform/watchdog.h"
#include "platform/radio.h"
#include "state/sys_bus.h"
#include "storage/log.h"
#include "storage/settings.h"
#include "zlrs/mgmt/mgmt.h"

#include "zlrs/crsf/crsf.h"
#include "zlrs/link/link.h"
#include "zlrs/region/region.h"
#include "zlrs/proto/tlm.h"

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

/* Control frame plan: derived at boot from Kconfig and the region duty cap, see plan_init() */
static uint32_t control_rate_hz;
static uint32_t frame_period_us;
static uint32_t slot_budget_us;
static uint32_t slot_prepare_us = CONFIG_ZLRS_APP_PREPARE_US;
static uint8_t tlm_every_n;
static uint8_t data_every_n;
static uint8_t resync_every_n;
/* Data payload cap: derived from the frame format and rate tier, not a Kconfig knob */
static uint16_t data_mtu;

/* Rough control frame airtime: 22 B RC + 3 B header + 4 B tag + 2 B CRC */
#define CTL_FRAME_LEN 31U
/* Telemetry frame: 8 B payload plus header and tag */
#define CTL_TLM_LEN   15U
/*
 * Both links can run alone. Mixed mode pairs on control; data24 pairs and transfers on data.
 * Build switches remove support; the saved operating mode selects which links run.
 */
static uint8_t operating_mode = ZLRS_MODE_MIXED;
#define ZLRS_DATA24          (operating_mode == ZLRS_MODE_DATA24)
#define ZLRS_CONTROL_ENABLED (IS_ENABLED(CONFIG_ZLRS_APP_LINK_CONTROL) && !ZLRS_DATA24)
#define ZLRS_DATA_ENABLED    IS_ENABLED(CONFIG_ZLRS_APP_LINK_DATA)
/* The CRSF port is part of the control link, and its presence is a devicetree fact: if the
 * board has the port at all, the control link uses it. */
#define ZLRS_CRSF_ENABLED    (ZLRS_CONTROL_ENABLED && ZLRS_PORT_CTRL_FIXED)
/* Same shape for the management port: the CDC in devicetree is the whole answer */
#define ZLRS_MGMT_ENABLED    ZLRS_PORT_MGMT

/* Copy batch only; the pump drains until its source or the link queue is empty. */
#define SERIAL_PUMP_CHUNK_LEN 256U
#define REPORT_PERIOD_MS 1000U

static zlrs_link_t link_control;
static zlrs_link_t link_data;
static zlrs_storage_settings_pairing_t saved_pairing;
static bool pairing_saved;

static zlrs_link_t *active_link(void)
{
	return ZLRS_CONTROL_ENABLED ? &link_control : &link_data;
}

/* Per-link modulation: fixed at boot from Kconfig and the region, see plan_init() */
static zlrs_mod_cfg_t control_mod;
static zlrs_mod_cfg_t data_mod;

/*
 * Band role (doc/06-architecture.md 2): control owns sub-G by default, data owns 2.4G;
 * racing mode swaps them. The regulatory gate (power, rate, duty) applies to the sub-G link.
 */
#if IS_ENABLED(CONFIG_ZLRS_APP_BAND_ROLE_SUBG_DATA)
#define CONTROL_ON_SUBG 0
#else
#define CONTROL_ON_SUBG 1
#endif

/* Fingerprint for the bench output: enough to compare two ends, not enough to leak the key */
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
static uint32_t fingerprint(const uint8_t *data, size_t len)
{
	uint32_t h = 2166136261U;

	for (size_t i = 0U; i < len; i++) {
		h = (h ^ data[i]) * 16777619U;
	}

	return h;
}
#endif

/* Region: picks the band, the power ceiling and the duty accounting */
static const zlrs_region_t *region;
static uint8_t region_id;
static int8_t tx_power_dbm;
static zlrs_duty_t duty;
/* Identity: read or generated once at boot, before anything that could use it (doc/09-security.md 1) */
static uint8_t device_id[ZLRS_DEV_ID_LEN];

/*
 * Role and security profile: runtime settings with a compile-time default (set-role /
 * set-profile, doc/07-firmware.md 3). Loaded once at boot, before the links are built.
 */
static bool role_tx;
static uint8_t active_profile;

/* The profile a build starts on, and which ones it may run: P1 is the production profile, P0
 * carries no tag at all so only a debug image may select it, P2 is not implemented yet. */
#define ZLRS_PROFILE_DEFAULT                                                                       \
	(IS_ENABLED(CONFIG_ZLRS_APP_SECURITY_PROFILE_P0) ? ZLRS_PROFILE_P0 : ZLRS_PROFILE_P1)

/* CRSF port I/O (defined below, called from the link callbacks) */
static void crsf_send_rc(const uint8_t *payload, uint8_t len);
static void crsf_send_link_stats(const zlrs_tlm_t *tlm);

static uint8_t rc_payload[ZLRS_RC_PAYLOAD_LEN];
static uint8_t failsafe_payload[ZLRS_CRSF_RC_PAYLOAD_LEN];
static uint32_t last_rc_input_ms;
static uint32_t last_tlm_up_ms;
static bool rc_input_seen;
static uint32_t crsf_bad_input;
static uint32_t last_report_ms;
/* Long presses already turned into a pairing request (app/src/ui/button.c) */
static uint32_t button_holds_seen;
/* Failsafe means nothing before a first connection: show unconfigured/unpaired/searching, not red */
static bool ever_connected;

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

		/* Just stash it: the power verdict runs in report() at 1 Hz (doc/08-protocol.md 4.5) */
		zlrs_link_power_telemetry(tlm.rssi_neg_dbm, tlm.lq_percent);
	}
}

static void on_data(void *ctx, const uint8_t *data, uint16_t len)
{
	(void)ctx;

	counters.data_rx += zlrs_link_data_write(data, len);
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
	(void)zlrs_storage_settings_save(ZLRS_SETTING_FAILSAFE, failsafe_payload,
					 sizeof(failsafe_payload));
	LOG_INF("failsafe values received from TX");
}

/*
 * Control frame plan: rate tier (Kconfig) x region duty cap. Over the cap we clamp to it and
 * warn: the regulation wins, but the user gets to see it.
 */
/*
 * SEARCHING sync period follows the modulation (doc/08-protocol.md 3.6):
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

static void plan_mixed_init(void)
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
	 * Modulation is per link (doc/06-architecture.md 3): where the band cannot fit wide
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

	/* Four equal hardware slots must fit control and the larger pairing packets. */
	const uint32_t control_window_us =
		zlrs_radio_airtime_us(&control_mod, MAX(CTL_FRAME_LEN, ZLRS_PAIR_PAYLOAD_LEN +
							       ZLRS_FRAME_HDR_LEN +
							       ZLRS_TAG_LEN)) +
		CONFIG_ZLRS_APP_PREPARE_US + CONFIG_ZLRS_APP_TX_GUARD_US + ZLRS_RADIO_STARTUP_US +
		100U;
	const uint32_t hardware_max_hz = MAX(1U, 1000000U / (4U * control_window_us));
	if (req.want_hz > hardware_max_hz) {
		LOG_WRN("hardware slot budget: control rate %u Hz -> %u Hz", req.want_hz,
			hardware_max_hz);
		req.want_hz = hardware_max_hz;
	}

	/*
	 * MTU follows the frame format and rate tier (doc/08-protocol.md 4.3): the largest payload
	 * that fits the one-way data window budget (a quarter frame), capped by the format.
	 */
	{
		/*
		 * Budget the data window against the frame rate this build actually asks for: the
		 * rate knob differs per mode (racing, LoRa, plain FLRC), and only req.want_hz knows.
		 */
		const uint32_t budget_us = (1000000U / req.want_hz) / 4U;
		const uint16_t frame_cap = data_mod.modulation == ZLRS_MOD_LORA
						   ? ZLRS_LORA_FRAME_MAX_LEN
						   : ZLRS_FRAME_MAX_LEN;
		uint16_t len = frame_cap - ZLRS_FRAME_HDR_LEN - ZLRS_TAG_LEN - 2U;

		while (len > 16U && zlrs_radio_airtime_us(&data_mod, (uint16_t)(len + 9U)) +
						    slot_prepare_us + CONFIG_ZLRS_APP_TX_GUARD_US +
						    ZLRS_RADIO_STARTUP_US + 100U >
					    budget_us) {
			len = (uint16_t)(len - 8U);
		}

		data_mtu = len;
	}

	/*
	 * The superframe period is set by the slower link: the two data windows run once per
	 * data_every_n frames, amortised over each frame; core/region iterates the plan to agreement.
	 */
	req.other_mod = ZLRS_DATA_ENABLED ? &data_mod : NULL;
	req.other_frame_len = (uint16_t)(data_mtu + 9U);
	zlrs_region_control_plan(&req, &plan);

	control_rate_hz = plan.rate_hz;
	frame_period_us = MAX(plan.frame_period_us, 4U * control_window_us);
	slot_budget_us = frame_period_us / 4U;
	tlm_every_n = plan.tlm_every_n;
	data_every_n = plan.data_every_n;
	LOG_INF("data plan: mtu=%u frame=%u us slot_prepare=%u us", data_mtu,
		frame_period_us, slot_prepare_us);

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
}

static void plan_init(void)
{
	if (ZLRS_DATA24) {
		/* The data frame, not the unused control plan, determines the slot. All four
		 * hardware windows form alternating TX/RX pairs; pairing also uses this modem. */
		data_mod = zlrs_mod_flrc(CONFIG_ZLRS_APP_DATA_RATE_BPS);
		control_mod = data_mod;
		slot_prepare_us = CONFIG_ZLRS_APP_DATA24_PREPARE_US;
		data_mtu = ZLRS_PAYLOAD_MAX_LEN - 2U;
		slot_budget_us = slot_prepare_us + CONFIG_ZLRS_APP_TX_GUARD_US +
				 ZLRS_RADIO_STARTUP_US +
				 zlrs_radio_airtime_us(&data_mod, ZLRS_FRAME_MAX_LEN) + 200U;
		/* The deadline is 100 us before preparation; leave another 100 us so the
		 * maximum frame finishes strictly before it, matching radio_prepare(). */
		frame_period_us = 4U * slot_budget_us;
		control_rate_hz = 1000000U / frame_period_us;
		tlm_every_n = 1U;
		data_every_n = 1U;
		LOG_INF("data24 plan: mtu=%u pair=%u us slot_prepare=%u us", data_mtu,
			slot_budget_us * 2U, slot_prepare_us);
	} else {
		plan_mixed_init();
	}

	/* CONNECTED sync period converted to frames (doc/08-protocol.md 5.1): 1000 ms fits an
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
	LOG_INF("connected-state sync every %u frame(s) (resync, doc/08-protocol.md 5.1)",
		(unsigned int)resync_every_n);
}

/* ---- link setup ---------------------------------------------------------- */

static uint32_t random_u32(void *ctx)
{
	ARG_UNUSED(ctx);
	return sys_rand32_get();
}

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
		/* Bench switches, each can be turned off, see doc/11-debug.md 3 */
		.hopping = IS_ENABLED(CONFIG_ZLRS_APP_FHSS) ? 1U : 0U,
		.handshake = IS_ENABLED(CONFIG_ZLRS_APP_SESSION_HANDSHAKE) ? 1U : 0U,
		.telemetry = IS_ENABLED(CONFIG_ZLRS_APP_TELEMETRY) ? 1U : 0U,
		/* Modulation: narrow-band regions get the LoRa tier, wide ones FLRC (see plan_init) */
		.mod = control_mod,
		.tx_power_dbm = CONTROL_ON_SUBG ? MIN(tx_power_dbm, region->max_tx_power_dbm)
						: tx_power_dbm,
		.rx_timeout_ms = 2U,
		/* Acquisition window: the pairing and search states have no shared slot phase yet */
		.acq_timeout_ms = 8U,
		.sync_period_ms = sync_period_for_mod(&control_mod),
		/*
		 * Telemetry is the TX-side liveness proof: with it off there is nothing to hear, so the
		 * peer-loss check has to go too, or every frame drags the TX back into SEARCHING.
		 */
		.peer_timeout_ms = (role_tx && IS_ENABLED(CONFIG_ZLRS_APP_TELEMETRY))
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
	data_cfg.domain = ZLRS_DATA24 || CONTROL_ON_SUBG ? ZLRS_DOMAIN_ISM2400 : region->domain;
	data_cfg.second_domain =
		ZLRS_DATA24 ? 0xFFU : (CONTROL_ON_SUBG ? region->domain : ZLRS_DOMAIN_ISM2400);
	data_cfg.sync_slot = ZLRS_SYNC_SLOT_DATA;
	data_cfg.mod = data_mod;
	data_cfg.tx_power_dbm = ZLRS_DATA24 || CONTROL_ON_SUBG
					? tx_power_dbm
					: MIN(tx_power_dbm, region->max_tx_power_dbm);
	data_cfg.arq = IS_ENABLED(CONFIG_ZLRS_APP_DATA_ARQ) ? 1U : 0U;
	/*
	 * An ACK may take two rounds, including the one-round delay from preparing during RX.
	 * Data24 has two A/B rounds per timer frame. Keep at least 10 ms for radio turnaround.
	 */
	const uint64_t arq_ack_period_us =
		(ZLRS_DATA24 ? (uint64_t)frame_period_us : 2ULL * frame_period_us) * data_every_n *
		CONFIG_ZLRS_APP_PACING_SCALE;
	const uint32_t arq_min_timeout_ms = ZLRS_DATA24 ? 10U : ZLRS_ARQ_TIMEOUT_MS;
	data_cfg.arq_timeout_ms = (uint16_t)MIN(
		UINT16_MAX, MAX(arq_min_timeout_ms, DIV_ROUND_UP(arq_ack_period_us, 1000U)));
	/*
	 * The data link is best-effort: it sends nothing when idle, so it has no peer-loss check,
	 * which would otherwise rebuild the session over and over while idle (doc/08-protocol.md 3.7).
	 */
	data_cfg.peer_timeout_ms = 0U;
	data_cfg.data_mtu = data_mtu;
	data_cfg.sync_period_ms = sync_period_for_mod(&data_mod);
	data_cfg.tlm_every_n = 1U; /* no telemetry slot on the data link */
	data_cfg.data_every_n = data_every_n;
	/* DATA_A occurs twice per timer frame in data24; preserve the wall-clock resync interval. */
	data_cfg.resync_every_n = ZLRS_DATA24
					  ? (uint8_t)MIN((uint16_t)resync_every_n * 2U, UINT8_MAX)
					  : resync_every_n;

	zlrs_link_init(&link_control, &ctl_cfg, random_u32, NULL, &ctl_ev);
	zlrs_link_init(&link_data, &data_cfg, random_u32, NULL, &data_ev);

	zlrs_link_rate_init(&ctl_cfg.mod);

	/* Ceiling is min(configured power, region cap); the floor is a Kconfig knob */
	zlrs_link_power_init(MIN(tx_power_dbm, region->max_tx_power_dbm),
			     CONFIG_ZLRS_APP_DYNAMIC_POWER_FLOOR_DBM);
}

static bool ensure_device_id(void)
{
	if (zlrs_storage_settings_load(ZLRS_SETTING_DEV_ID, device_id, sizeof(device_id)) ==
		    (int)ZLRS_DEV_ID_LEN &&
	    zlrs_storage_settings_dev_id_valid(device_id)) {
		LOG_INF("dev id %02x%02x%02x%02x (NVS)", device_id[0], device_id[1], device_id[2],
			device_id[3]);
		return true;
	}

	if (!zlrs_storage_settings_new_dev_id(device_id)) {
		return false;
	}

	if (zlrs_storage_settings_save(ZLRS_SETTING_DEV_ID, device_id, sizeof(device_id)) != 0) {
		LOG_WRN("dev id not persisted, it changes on the next boot");
	}
	LOG_INF("dev id %02x%02x%02x%02x (new)", device_id[0], device_id[1], device_id[2],
		device_id[3]);

	return true;
}

static void load_identity(void)
{
	uint8_t root_key[ZLRS_KEY_LEN];
	zlrs_storage_settings_pairing_t pairing;
	bool have_root_key = false;
	const uint32_t now_ms = zlrs_slot_timer_now_ms();

	zlrs_link_set_dev_id(&link_control, device_id);
	zlrs_link_set_dev_id(&link_data, device_id);

	/* Root key: NVS only, written once with the management tool (set-phrase / set-key-hex) */
	if (zlrs_storage_settings_load(ZLRS_SETTING_ROOT_KEY, root_key, sizeof(root_key)) ==
	    (int)ZLRS_KEY_LEN) {
		have_root_key = true;
	}

	if (!have_root_key) {
		LOG_WRN("no root key: staying off air until one is written (doc/09-security.md)");
		return;
	}

	zlrs_link_set_root_key(&link_control, root_key, now_ms);
	zlrs_link_set_root_key(&link_data, root_key, now_ms);

	/* Pairing record: peer dev_id plus the two pairing nonces, loaded into both links */
	if (zlrs_storage_settings_load(ZLRS_SETTING_PAIRING, &pairing, sizeof(pairing)) ==
	    (int)sizeof(pairing)) {
		zlrs_link_set_peer(&link_control, pairing.peer_id, pairing.pair_salt, now_ms);
		zlrs_link_set_peer(&link_data, pairing.peer_id, pairing.pair_salt, now_ms);
		saved_pairing = pairing;
		pairing_saved = true;
		LOG_INF("pairing record restored");
	}
}

/* ---- superframe ---------------------------------------------------------- */

static void pump_serial(void);

/* Wait against real elapsed time. Hardware alarms wake the thread at slot deadlines. */
static void radio_wait_done(uint32_t budget_us)
{
	const uint32_t deadline = zlrs_slot_timer_now_us() + budget_us;

	while ((int32_t)(deadline - zlrs_slot_timer_now_us()) > 0) {
		if (zlrs_radio_irq_pending()) {
			if (zlrs_radio_drain()) {
				return;
			}
		}
		/* Radio IRQ and hardware slot boundary both wake this wait. */
		const int32_t remain = (int32_t)(deadline - zlrs_slot_timer_now_us());

		if (remain > 0) {
			(void)zlrs_radio_wait_us((uint32_t)remain);
		}
	}
}

/* Debug measurements of the absolute frame clock, slot work and authenticated RX offsets. */
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
typedef struct {
	uint32_t frames;
	uint32_t frame_us_last; /* measured period of the last frame */
	uint32_t frame_us_min;
	uint32_t frame_us_max;
	uint32_t slot_us_last[4];
	uint32_t slot_us_max[4];
	uint32_t rx_slot;      /* slot the last frame landed in */
	uint32_t rx_offset_us; /* where in the frame it landed */
	uint32_t rx_offset_min;
	uint32_t rx_offset_max;
	uint32_t submit_us_last; /* the port's own submit path (radio configuration + enqueue) */
	uint32_t submit_us_max;
} bench_t;

static bench_t bench;
static uint32_t prev_frame_start_us;
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
static zlrs_radio_diag_t radio_diag;
#endif

/* TX owns the absolute clock; RX follows verified IRQ timestamps and scans when deaf. */
#define ZLRS_PHASE_DEADBAND_US   100
#define ZLRS_PHASE_SCAN_STEP_US  25U

static uint32_t frame_start_us;
static uint32_t slot_deadline_us;
static uint32_t slot_nominal_us;
static uint32_t skipped_frames;
static uint32_t missed_slots;
static uint32_t packet_build_us_last;
static uint32_t packet_build_us_max;
static uint32_t serial_pump_us_last;
static uint32_t protocol_build_us_last;
static uint32_t prepare_wait_skipped;
static uint32_t prepare_wait_late_us_max;
static uint32_t prepare_irq_late_us_max;
static uint32_t prepare_wake_us_max;
static uint32_t late_prepare_entry_us;
static uint32_t late_prepare_build_us;
static uint32_t late_prepare_spi_us;
static uint32_t slot_off_us[4]; /* where the slot's transaction really started in our frame */
static int32_t phase_adj_us;
static bool phase_adj_set;    /* one correction per frame: several receptions would fight */
static int32_t phase_last_us; /* what the last correction was, for the bench log */
static int32_t phase_err_us;  /* residual the last correction was computed from */
static uint8_t phase_samples;
static uint32_t phase_hits;     /* corrections since the last report */
static uint32_t last_rx_any_ms; /* any successful reception, either link: the scan trigger */

static uint32_t now_us(void)
{
	return zlrs_slot_timer_now_us();
}

/*
 * Init breadcrumbs. The main thread can be stuck in a place where the console, the shell and the
 * logging thread all still work, so the only way to see how far init got is memory the other
 * threads can read: no console I/O on this path (printk on a USB CDC console can block precisely
 * when the host is not draining it, which is what made the first attempt at this useless).
 */
static volatile uint8_t init_trace[24];
static volatile uint8_t init_trace_len;

static void trace_add(char c)
{
	if (init_trace_len < sizeof(init_trace)) {
		init_trace[init_trace_len++] = (uint8_t)c;
	}
}

#define TRACE(c) trace_add(c)

static void run_slot(uint8_t slot)
{
	/* An idle or early-completed predecessor must not open RX in an earlier slot.
	 * Both packet preparation and RX entry start at this slot's reserved window. */
	struct zlrs_slot_timer_wait timing;
	const uint32_t prepare_at_us = slot_nominal_us - slot_prepare_us;

	/* A completed RX leaves a safe opportunity to build our next TX immediately.
	 * RX entry still waits for its own window; searching/pairing retain that rule. */
	const bool early_tx = ZLRS_DATA24 && link_data.state == ZLRS_STATE_CONNECTED &&
			      (role_tx == ((slot & 1U) == 0U));

	if (early_tx) {
		timing = (struct zlrs_slot_timer_wait){.wake_us = now_us()};
	} else {
		zlrs_slot_timer_wait_until(prepare_at_us, &timing);
	}
	if (timing.armed) {
		prepare_irq_late_us_max =
			MAX(prepare_irq_late_us_max, timing.irq_us - prepare_at_us);
		prepare_wake_us_max = MAX(prepare_wake_us_max, timing.wake_us - timing.irq_us);
	} else {
		prepare_wait_skipped++;
		if ((int32_t)(timing.wake_us - prepare_at_us) > 0) {
			prepare_wait_late_us_max =
				MAX(prepare_wait_late_us_max, timing.wake_us - prepare_at_us);
		}
	}

	const bool data_slot =
		ZLRS_DATA24 || (slot == ZLRS_SLOT_DATA_A || slot == ZLRS_SLOT_DATA_B);
	const uint8_t link_slot =
		ZLRS_DATA24 ? ((slot & 1U) == 0U ? ZLRS_SLOT_DATA_A : ZLRS_SLOT_DATA_B) : slot;
	zlrs_link_t *link = data_slot ? &link_data : &link_control;
	const uint32_t slot_start_us = now_us();
	/* Slot decisions use the planned time even though construction happens before it. */
	const uint32_t now_ms = zlrs_slot_timer_uptime_ms(slot_nominal_us);
	const uint32_t hit_before = link->stats.slot_hit[link_slot];
	const uint32_t good_before = link->stats.rx_frames;
	/* The duty-cycle gate only applies to whichever link is on sub-G */
	const bool on_subg =
		!ZLRS_DATA24 && (data_slot ? (CONTROL_ON_SUBG == 0) : (CONTROL_ON_SUBG != 0));
	const zlrs_radio_req_t *req;

	if (data_slot ? !ZLRS_DATA_ENABLED : !ZLRS_CONTROL_ENABLED) {
		return;
	}
	/* Disabled slots stay idle; acquisition outside assigned slots corrupts phase/hop alignment. */
	if ((int32_t)(slot_deadline_us - now_us()) <= 100) {
		missed_slots++;
		zlrs_link_skip_slot(link, link_slot);
		return;
	}
	const uint32_t pump_at_us = now_us();

	if (data_slot) {
		pump_serial();
	}
	const uint32_t protocol_at_us = now_us();

	req = zlrs_link_slot(link, link_slot, now_ms);

	if (req == NULL) {
		return;
	}

	/* Advance the protocol even if the scheduled slot was missed; never send it late. */
	if ((int32_t)(slot_deadline_us - now_us()) <= 100) {
		missed_slots++;
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, now_ms);
		return;
	}
	zlrs_radio_slot(slot_nominal_us + CONFIG_ZLRS_APP_TX_GUARD_US, slot_deadline_us);

	if (req->is_tx) {
		serial_pump_us_last = protocol_at_us - pump_at_us;
		protocol_build_us_last = now_us() - protocol_at_us;
		packet_build_us_last = now_us() - slot_start_us;
		packet_build_us_max = MAX(packet_build_us_max, packet_build_us_last);
	}

	/* Out of duty budget: give the slot up (finish as a radio failure, the link retries next
	 * round). Only constrains the sub-G link. */
	if (req->is_tx && on_subg &&
	    !zlrs_duty_reserve(&duty, (zlrs_radio_airtime_us(&req->mod, req->size) + 999U) / 1000U,
			       now_ms)) {
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, now_ms);
		return;
	}

	const uint32_t prepare_started_us = now_us();

	if (zlrs_radio_prepare(link, req) != 0) {
		if (req->is_tx) {
			late_prepare_entry_us = slot_start_us - prepare_at_us;
			late_prepare_build_us = prepare_started_us - slot_start_us;
			late_prepare_spi_us = now_us() - prepare_started_us;
		}
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, zlrs_slot_timer_now_ms());
		return;
	}

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	/* How much of a slot goes into the port's own submit path (vs. the air) */
	bench.submit_us_last = now_us() - slot_start_us;
	if (bench.submit_us_last > bench.submit_us_max) {
		bench.submit_us_max = bench.submit_us_last;
	}
#endif

	/* Modem, authentication and FIFO prefix are ready. The timer issues SET_TX;
	 * the owner then appends a fallback FLRC packet's tail while it is on air. */
	const int launch_rc =
		req->is_tx ? zlrs_radio_launch_at(slot_nominal_us + CONFIG_ZLRS_APP_TX_GUARD_US)
			   : zlrs_radio_launch();

	if (launch_rc != 0) {
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, zlrs_slot_timer_now_ms());
		return;
	}

	slot_off_us[slot] = slot_nominal_us - frame_start_us;
	if (ZLRS_DATA24 && !req->is_tx && link->state == ZLRS_STATE_CONNECTED) {
		uint16_t size;
		const uint32_t next_us = slot_nominal_us + slot_budget_us;
		const uint32_t next_ms = zlrs_slot_timer_uptime_ms(next_us);

		pump_serial();
		const uint8_t *buf = zlrs_link_prepare_data(
			link, role_tx ? ZLRS_SLOT_DATA_A : ZLRS_SLOT_DATA_B, next_ms, &size);

		if (buf != NULL) {
			(void)zlrs_radio_preload_tx(buf, size);
		}
	}
	const int32_t remaining_us = (int32_t)(slot_deadline_us - now_us());

	if (remaining_us > 0) {
		radio_wait_done((uint32_t)remaining_us);
	}

	zlrs_radio_drain();

	/*
	 * The port's completion semaphore is shared by both links, so "the wait returned" is not the
	 * same as "this link's transaction finished": the other link's completion can wake this wait.
	 * What matters is whether the link still has a request outstanding — if it does, abort and
	 * finish it here. Skipping this left the request pending forever (the old code only aborted on a
	 * timeout, and the wait had "succeeded"), which is the wedge that made a link go deaf for good.
	 */
	if (link->req_pending) {
		zlrs_radio_abort();
		zlrs_link_complete(link, req->is_tx ? ZLRS_RADIO_FAIL : ZLRS_RADIO_OK, 0U, 0, 0,
				   zlrs_slot_timer_now_ms());
	}

	if (link->stats.slot_hit[link_slot] != hit_before && link->stats.rx_frames != good_before) {
		uint32_t done_us;
		uint32_t airtime_us;

		last_rx_any_ms = zlrs_slot_timer_now_ms();
		/* TX is the clock master. Only RX follows verified packets, timestamped in the GPIO ISR. */
		if (!role_tx && !phase_adj_set &&
		    (data_slot ? link == &link_data : link == &link_control) &&
		    zlrs_radio_rx_timing(&done_us, &airtime_us)) {
			/* Include SPI command transfer and synthesizer/PA startup in the phase origin. */
			const uint32_t expected = slot_nominal_us + CONFIG_ZLRS_APP_TX_GUARD_US +
						  ZLRS_RADIO_PHASE_STARTUP_US;
			const int32_t err = (int32_t)(done_us - airtime_us - expected);
			const int32_t limit = (int32_t)frame_period_us / 16;

			/* Once acquired, discard delayed IRQ outliers rather than move the clock. */
			if (phase_samples >= 8U && (err > limit || err < -limit)) {
				return;
			}
			phase_err_us = err;
			phase_adj_us =
				(err > ZLRS_PHASE_DEADBAND_US || err < -ZLRS_PHASE_DEADBAND_US)
					? CLAMP(phase_samples < 8U ? err : err / 4, -limit, limit)
					: 0;
			phase_last_us = phase_adj_us;
			phase_hits++;
			phase_samples = MIN(phase_samples + 1U, 8U);
			phase_adj_set = true;
		}
	}

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	{
		const uint32_t took_us = now_us() - slot_start_us;

		bench.slot_us_last[slot] = took_us;
		if (took_us > bench.slot_us_max[slot]) {
			bench.slot_us_max[slot] = took_us;
		}
		if (link->stats.slot_hit[link_slot] != hit_before) {
			const uint32_t offset_us = now_us() - frame_start_us;

			bench.rx_slot = slot;
			bench.rx_offset_us = offset_us;
			if (bench.rx_offset_max == 0U || offset_us > bench.rx_offset_max) {
				bench.rx_offset_max = offset_us;
			}
			if (bench.rx_offset_min == 0U || offset_us < bench.rx_offset_min) {
				bench.rx_offset_min = offset_us;
			}
		}
	}
#endif
}

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
/*
 * Reboot into MCUboot serial recovery: set the boot-mode flag (retained SRAM, survives a reset),
 * which makes MCUboot wait for a signed image. The only entries are the management protocol's
 * recovery command and the debug shell, see doc/09-security.md 9.
 */
#ifdef PICO_RP2350
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_SHELL)
/*
 * Bench: hand the board to the ROM's USB bootloader. The bootloader image is the one thing the
 * application cannot replace through MCUboot (it is what runs MCUboot), and the ROM entry saves
 * reaching for the BOOTSEL button every time: the board re-enumerates as a USB drive and the
 * .uf2 can be copied in.
 */
static int cmd_bootsel(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "rebooting into the RP2350 ROM bootloader (USB drive)");
	k_msleep(50);
	reset_usb_boot(0, 0); /* does not return */

	return 0;
}
#endif /* CONFIG_ZLRS_APP_DEBUG_SHELL */
#endif

#endif

static void pump_serial(void)
{
	uint8_t chunk[SERIAL_PUMP_CHUNK_LEN];
	uint16_t got;
	uint16_t room;

	if (!ZLRS_DATA_ENABLED) {
		return; /* no data link: nothing to forward from the UART */
	}

	/*
	 * The UART ISR only fills its RX ring. The radio thread owns link_data, so feed the link queue
	 * just before it selects a data-slot packet. Drain until either side has no room; the chunk is
	 * only a copy buffer, not a per-slot or per-frame throughput limit.
	 */
	for (;;) {
		room = zlrs_link_tx_space(&link_data);
		if (room == 0U) {
			return;
		}
		if (room > sizeof(chunk)) {
			room = (uint16_t)sizeof(chunk);
		}

		got = zlrs_link_data_read(chunk, room);
		if (got == 0U) {
			return;
		}

		/* room was measured immediately above in this single-owner thread. */
		if (zlrs_link_write(&link_data, chunk, got) != got) {
			return;
		}
	}
}

static void adopt_pairing(void)
{
	zlrs_link_t *const source = active_link();
	zlrs_storage_settings_pairing_t pairing;

	if (!source->paired) {
		return;
	}
	memcpy(pairing.peer_id, source->peer_id, ZLRS_DEV_ID_LEN);
	memcpy(pairing.pair_salt, source->pair_salt, ZLRS_PAIR_SALT_LEN);
	if (pairing_saved && memcmp(&pairing, &saved_pairing, sizeof(pairing)) == 0) {
		return;
	}

	if (ZLRS_CONTROL_ENABLED && ZLRS_DATA_ENABLED) {
		zlrs_link_set_peer(&link_data, source->peer_id, source->pair_salt,
				   zlrs_slot_timer_now_ms());
	}
	if (zlrs_storage_settings_save(ZLRS_SETTING_PAIRING, &pairing, sizeof(pairing)) != 0) {
		return; /* Retry persistence; do not report an unsaved record as saved. */
	}
	saved_pairing = pairing;
	pairing_saved = true;
	LOG_INF("paired with %02x%02x%02x%02x", source->peer_id[0], source->peer_id[1],
		source->peer_id[2], source->peer_id[3]);
}

/*
 * Failsafe values default to throttle low and everything else centred, which is what a
 * handset sends with its link down. The management protocol can overwrite them (08-firmware 3).
 */
static void failsafe_init(void)
{
	uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT];

	if (zlrs_storage_settings_load(ZLRS_SETTING_FAILSAFE, failsafe_payload,
				       sizeof(failsafe_payload)) != (int)sizeof(failsafe_payload)) {
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
	last_rc_input_ms = zlrs_slot_timer_now_ms();
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
	(void)zlrs_link_crsf_write(ZLRS_CRSF_TYPE_LINK_STATISTICS, payload, sizeof(payload));
}

/* RX: rebuild the received RC payload into a CRSF frame for the flight controller */
static void crsf_send_rc(const uint8_t *payload, uint8_t len)
{
	if (!ZLRS_CRSF_ENABLED || len != ZLRS_CRSF_RC_PAYLOAD_LEN) {
		return;
	}

	(void)zlrs_link_crsf_write(ZLRS_CRSF_TYPE_RC_CHANNELS, payload, len);
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

	if (!zlrs_link_crsf_ready()) {
		return;
	}

	while ((len = zlrs_link_crsf_read(&type, payload, sizeof(payload))) > 0U) {
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

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
/*
 * Debug reports use the most recent one-second window: receive windows, how many heard a frame, and
 * frames sent, all as a rate: that is the number a marginal antenna or a misaligned slot shows up
 * in, and it is what tells a receiver problem apart from a protocol one.
 */
typedef struct {
	uint32_t wins;
	uint32_t hits;
	uint32_t tx;
} rate_snap_t;

static rate_snap_t rate_snapshot(const zlrs_link_t *link)
{
	rate_snap_t s;

	s.wins = link->stats.slot_rx[0] + link->stats.slot_rx[1] + link->stats.slot_rx[2] +
		 link->stats.slot_rx[3];
	s.hits = link->stats.slot_hit[0] + link->stats.slot_hit[1] + link->stats.slot_hit[2] +
		 link->stats.slot_hit[3];
	s.tx = link->stats.tx_frames;

	return s;
}

/*
 * Most recent one-second window, kept up to date by report().
 */
static struct {
	uint32_t wins;
	uint32_t hits;
	uint32_t tx;
	uint32_t span_ms;
	uint8_t state;
} rate_last[2];

static void rate_update(uint8_t idx, const zlrs_link_t *link, const rate_snap_t *before,
			uint32_t span_ms, bool configured)
{
	const rate_snap_t now = rate_snapshot(link);
	const uint32_t wins = now.wins - before->wins;
	const uint32_t hits = now.hits - before->hits;
	const uint32_t tx = now.tx - before->tx;
	const uint32_t span = (span_ms != 0U) ? span_ms : 1U;

	rate_last[idx].wins = wins;
	rate_last[idx].hits = hits;
	rate_last[idx].tx = tx;
	rate_last[idx].span_ms = span;
	rate_last[idx].state = configured ? link->state : ZLRS_STATE_UNCONFIGURED;
}
#endif

static void report(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	/* Per-second link rates are read by the debug management report. */
	static rate_snap_t rate_before[2];
	static uint32_t rate_ms;
#endif

	if ((uint32_t)(now_ms - last_report_ms) < REPORT_PERIOD_MS) {
		return;
	}
	last_report_ms = now_ms;

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	rate_update(0U, &link_control, &rate_before[0],
		    (rate_ms != 0U) ? now_ms - rate_ms : REPORT_PERIOD_MS, ZLRS_CONTROL_ENABLED);
	rate_update(1U, &link_data, &rate_before[1],
		    (rate_ms != 0U) ? now_ms - rate_ms : REPORT_PERIOD_MS, ZLRS_DATA_ENABLED);
	rate_before[0] = rate_snapshot(&link_control);
	rate_before[1] = rate_snapshot(&link_data);
	rate_ms = now_ms;
#endif

	zlrs_link_rate_tick(&link_control, now_ms);

#if IS_ENABLED(CONFIG_ZLRS_APP_SUB_BAND_MIX)
	/*
 * Sub-band mixing (doc/08-protocol.md 5.1): when LQ drops, move slots to sub-G (the link
	 * layer picks the order: telemetry, then data, then RC) and move them back when LQ recovers.
	 * The region duty cap (zlrs_region_mix_level) bounds it; at most one step per second.
	 */
	{
		static uint32_t mix_last_ms;
		static uint8_t mix_level;
		uint8_t want = mix_level;
		const uint32_t window_ms = (uint32_t)(4U * (frame_period_us / 1000U));
		const uint32_t airtime_us = zlrs_mod_airtime_us(&control_mod, CTL_FRAME_LEN);
		const uint8_t cap = ZLRS_DATA24
					    ? 0U
					    : zlrs_region_mix_level(CONTROL_ON_SUBG ? region : NULL,
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

	zlrs_link_power_tick(&link_control, now_ms);

	/*
	 * The once-a-second status lines: bench only, logging only (the 1 Hz rate and the power verdicts
	 * are separate). A compile-time guard, because the counters it reads only exist when it is on.
	 */
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
	zlrs_radio_diag(&radio_diag);

	LOG_INF("%s ctl=%u tlm=%u data=%u serial_drop=%u host_drop=%u", role_tx ? "TX" : "RX",
		counters.rc_rx, counters.tlm_rx, counters.data_rx, zlrs_link_data_dropped(),
		zlrs_link_data_tx_dropped());
	/* Frames that really went out, per slot: only transactions that completed count (core/link) */
	LOG_INF("air: ctl_tx=%u ctl_rx=%u data_tx=%u data_rx=%u fail=%u+%u | slot tx %u/%u/%u/%u",
		link_control.stats.tx_frames, link_control.stats.rx_frames,
		link_data.stats.tx_frames, link_data.stats.rx_frames, link_control.stats.radio_fail,
		link_data.stats.radio_fail, link_control.stats.slot_tx[0],
		link_control.stats.slot_tx[1], link_control.stats.slot_tx[2],
		link_control.stats.slot_tx[3]);
	/* Why frames were dropped on the way in: no tag match, bad shape, or nothing heard at all */
	LOG_INF("rx : bad_tag=%u bad_frame=%u dup=%u ooo=%u syncs=%u resync=%u | rssi %d..%d",
		link_control.stats.rx_bad_tag, link_control.stats.rx_bad_frame,
		link_control.stats.rx_dup, link_control.stats.rx_out_of_order,
		link_control.stats.rx_syncs, link_control.stats.hop_resync,
		link_control.stats.rssi_min, link_control.stats.rssi_max);
	/*
	 * Listen windows per slot and how many of them heard a frame: a window that almost never
	 * hears anything is a phase problem, not a range problem (see the management link report).
	 */
	LOG_INF("win : ctl rx %u/%u/%u/%u got %u/%u/%u/%u | sync tx=%u rx=%u resp %u+%u cfm %u+%u",
		link_control.stats.slot_rx[0], link_control.stats.slot_rx[1],
		link_control.stats.slot_rx[2], link_control.stats.slot_rx[3],
		link_control.stats.slot_hit[0], link_control.stats.slot_hit[1],
		link_control.stats.slot_hit[2], link_control.stats.slot_hit[3],
		link_control.stats.tx_syncs, link_control.stats.rx_syncs,
		link_control.stats.sess_resp_tx, link_control.stats.sess_resp_rx,
		link_control.stats.sess_cfm_tx, link_control.stats.sess_cfm_rx);
	/*
	 * Superframe timing, cycle-counter measured: the frame both ends pace to, the length of each
	 * of our slots, and where in our frame the peer's frames landed. A peer slot that slides
	 * across the window range is the signature of two free-running frames.
	 */
	LOG_INF("tm : frame=%u us (%u..%u) slot=%u/%u/%u/%u max=%u/%u/%u/%u rx=slot%u+%u (%u..%u)"
		" txn=%u late_starts=%u lock=%u err=%d adj=%d submit=%u/%u us",
		bench.frame_us_last, bench.frame_us_min, bench.frame_us_max, bench.slot_us_last[0],
		bench.slot_us_last[1], bench.slot_us_last[2], bench.slot_us_last[3],
		bench.slot_us_max[0], bench.slot_us_max[1], bench.slot_us_max[2],
		bench.slot_us_max[3], bench.rx_slot, bench.rx_offset_us, bench.rx_offset_min,
		bench.rx_offset_max, radio_diag.transactions, radio_diag.late_starts, phase_hits,
		(int)phase_err_us, (int)phase_last_us, bench.submit_us_last, bench.submit_us_max);
	phase_hits = 0U;
	{
		LOG_INF("rd : stale_irq=%u(%08x) empty_irq=%u overruns=%u rx_win=%u tx=%u us rx=%u "
			"us",
			radio_diag.stale_irq, radio_diag.stale_irq_bits, radio_diag.empty_irq,
			radio_diag.overruns, radio_diag.rx_windows, radio_diag.tx_us_last,
			radio_diag.rx_us_last);
		/* Where a transaction's milliseconds go: the drain + RAC submit, and the wait after */
		LOG_INF("rd2: setup=%u us tx_done=%u setup max=%u us rx_packets=%u",
			radio_diag.submit_us_last, radio_diag.tx_done, radio_diag.submit_us_max,
			radio_diag.rx_packets);
	}
	/* Fingerprints (not the keys): comparing two ends needs to be possible from the log alone */
	LOG_INF("key: ctl pair=%08x word=%08x root=%08x a=%08x b=%08x hop=%u state=%u"
		" wait_cfm=%u cfm=%u/%u rssi=%d",
		fingerprint(link_control.pair_key, sizeof(link_control.pair_key)),
		fingerprint(link_control.sync_word, sizeof(link_control.sync_word)),
		fingerprint(link_control.root_key, sizeof(link_control.root_key)),
		link_control.pair_nonce, link_control.pair_nonce_b, link_control.fhss.index,
		link_control.state, link_control.pair_wait_cfm ? 1U : 0U,
		(unsigned int)link_control.pair_cfm_sent, link_control.pair_cfm_pending ? 1U : 0U,
		link_control.stats.last_rssi);
	/* Pairing handshake traffic: which of the three messages is the one going missing */
	LOG_INF("pair: ctl tx %u/%u/%u rx %u/%u/%u rej=%u", link_control.stats.pair_req_tx,
		link_control.stats.pair_resp_tx, link_control.stats.pair_cfm_tx,
		link_control.stats.pair_req_rx, link_control.stats.pair_resp_rx,
		link_control.stats.pair_cfm_rx, link_control.stats.pair_rejects);
	LOG_INF("crsf_bad=%u crsf_drop=%u air_drop=%u lq=%u%%", crsf_bad_input,
		zlrs_link_crsf_rx_dropped(), link_data.stats.data_drop_bytes,
		link_control.stats.lq_percent);
	LOG_INF("region %s: max %d dBm, duty denied %u", (region != NULL) ? region->name : "?",
		region->max_tx_power_dbm, duty.denied);
#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
	{
		int8_t dbm;
		uint32_t raises, lowers, fallbacks;

		zlrs_link_power_stats(&dbm, &raises, &lowers, &fallbacks);
		LOG_INF("tx power %d dBm (up %u / down %u / fallback %u)", dbm, raises, lowers,
			fallbacks);
	}
#endif

	/* Every line above covers one second: the next one gets a fresh timing window */
#endif /* CONFIG_ZLRS_APP_DEBUG_STATS_LOG */

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	/* Each management snapshot reports timing maxima from the preceding one-second interval. */
	phase_hits = 0U;
	bench.frame_us_min = 0U;
	bench.frame_us_max = 0U;
	bench.rx_offset_min = 0U;
	bench.rx_offset_max = 0U;
	bench.submit_us_max = 0U;
	for (uint8_t i = 0U; i < 4U; i++) {
		bench.slot_us_max[i] = 0U;
	}
#endif
}

/*
 * System state broadcast: the facts other parts of the application render or report
 * (app/src/state/sys_bus.h). Facts only — the LED thread turns them into colours (doc/10-ux.md).
 */
#if ZLRS_MGMT_ENABLED
static void enter_management_mode(void);
#endif

/*
 * State on a fatal configuration error (region vs radio-design mismatch):
 * alarm but stay reachable. The failsafe LED pattern is shown while the management port remains
 * up, so the host can enter it,
 * trigger serial recovery and reflash. A regulatory or security problem is worth staying off
 * the air for, but the device must stay flashable — otherwise one bad image means BOOTSEL.
 *
 * Exception: a test image (not yet confirmed by the host) resets on a fatal error so that
 * MCUboot rolls back — a flashable fault state is no reason to keep a broken build running.
 */

static void fault_loop(void)
{
	const struct zlrs_alerts alerts = {
		.fault = true,
		.data_drop_bytes = link_data.stats.data_drop_bytes,
	};

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	if (!boot_is_img_confirmed()) {
		/* Give the host 2 s to read the log or step in, then reset to roll back */
		LOG_ERR("fault on unconfirmed image: rebooting to roll back");
		k_msleep(2000);
		sys_reboot(SYS_REBOOT_COLD);
	}
#endif

	/* The fault outranks link and drop indications and uses the failsafe pattern
	 * (app/src/ui/led.c). Nothing else is published here — the links stop, so their last
	 * snapshot is also the truth. */
	zlrs_sys_publish_alerts(&alerts);

	LOG_ERR("fault state: not transmitting; management port stays available for flashing");

	for (;;) {
		zlrs_watchdog_feed();

#if ZLRS_MGMT_ENABLED
		/* Management port alive: management mode, the recovery command and reflashing all still work */
		if (zlrs_mgmt_poll_enter()) {
			enter_management_mode();
		}
#endif

		k_msleep(20);
	}
}

static void broadcast_state(void)
{
	const zlrs_link_t *const active = ZLRS_CONTROL_ENABLED ? &link_control : &link_data;
	const uint32_t drops = link_data.stats.data_drop_bytes;
	struct zlrs_link_status link;
	struct zlrs_alerts alerts;

	link.role_tx = role_tx;
	link.configured = active->configured;
	link.paired = active->paired;
	link.state = active->state;
	link.lq_percent = active->stats.lq_percent;

	/* TX without RC input sends failsafe values; an unconnected RX outputs them to the FC */
	if (ZLRS_CONTROL_ENABLED && link_control.state == ZLRS_STATE_CONNECTED) {
		ever_connected = true;
	}
	/*
	 * Failsafe only means something after a first connection: the TX was connected and then lost
	 * the handset (sending failsafe), or the RX was connected and dropped (outputting failsafe).
	 * A device that never connected shows its normal state (purple/yellow/cyan), never red.
	 */
	alerts.failsafe = ever_connected &&
			  (role_tx ? !rc_input_seen : (link_control.state != ZLRS_STATE_CONNECTED));
	alerts.fault = false;
	alerts.data_drop_bytes = drops;

	zlrs_sys_publish_link(&link);
	zlrs_sys_publish_alerts(&alerts);
}

/*
 * Image confirmation: an upgrade counts as successful only when the host really connected,
 * sent the explicit confirm command and got an answer — that flag is the whole test/revert
 * verdict. Upgrades run with test semantics: if the host never confirms, or the image never
 * comes up at all, the reset makes MCUboot roll back. See doc/07-firmware.md 6.
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

	if (zlrs_mgmt_mode_image_confirmed()) {
		confirmed = true;

		if (boot_write_img_confirmed() == 0) {
			LOG_INF("image confirmed by host: this build stays after a reset");
		} else {
			LOG_ERR("image confirm failed; MCUboot will revert on next reset");
		}
	} else if (zlrs_slot_timer_now_ms() >=
		   (uint32_t)CONFIG_ZLRS_APP_MCUBOOT_CONFIRM_TIMEOUT_MS) {
		/* Never confirmed by the host: reset and let MCUboot roll back instead of clinging on */
		LOG_ERR("upgrade not confirmed by host: rebooting to roll back");
		sys_reboot(SYS_REBOOT_COLD);
	}
#else
#endif
}

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
#define MGMT_DEBUG_REPORT_CAPACITY 4096U

struct mgmt_debug_report_writer {
	size_t len;
	bool failed;
};

static char mgmt_debug_report[MGMT_DEBUG_REPORT_CAPACITY];
static uint16_t mgmt_debug_report_len;
static uint8_t mgmt_debug_report_id;

static void mgmt_debug_append(struct mgmt_debug_report_writer *writer, const char *format, ...)
{
	va_list args;
	const size_t remaining = sizeof(mgmt_debug_report) - writer->len;
	int written;

	if (writer->failed || remaining == 0U) {
		writer->failed = true;
		return;
	}

	va_start(args, format);
	written = vsnprintf(&mgmt_debug_report[writer->len], remaining, format, args);
	va_end(args);

	if (written < 0 || (size_t)written >= remaining) {
		writer->failed = true;
		return;
	}
	writer->len += (size_t)written;
}

static const char *mgmt_debug_state(uint8_t state)
{
	switch (state) {
	case ZLRS_STATE_UNCONFIGURED:
		return "unconfigured";
	case ZLRS_STATE_UNPAIRED:
		return "unpaired";
	case ZLRS_STATE_PAIRING:
		return "pairing";
	case ZLRS_STATE_SEARCHING:
		return "searching";
	case ZLRS_STATE_CONNECTED:
		return "connected";
	default:
		return "unknown";
	}
}

static const char *mgmt_debug_bands(const zlrs_link_cfg_t *cfg)
{
	const bool primary_24 = cfg->domain == ZLRS_DOMAIN_ISM2400;
	const bool second_24 = cfg->second_domain == ZLRS_DOMAIN_ISM2400;

	if (cfg->second_domain == 0xFFU || cfg->second_domain == cfg->domain) {
		return primary_24 ? "2.4 GHz" : "sub-G";
	}
	return (primary_24 || second_24) ? "2.4 GHz + sub-G" : "sub-G";
}

static void mgmt_debug_frequency(struct mgmt_debug_report_writer *writer, uint32_t hz)
{
	if (hz >= 1000000000U) {
		mgmt_debug_append(writer, "%u.%03u GHz", hz / 1000000000U,
				  (hz % 1000000000U) / 1000000U);
	} else {
		mgmt_debug_append(writer, "%u.%03u MHz", hz / 1000000U, (hz % 1000000U) / 1000U);
	}
}

static void mgmt_debug_phy(struct mgmt_debug_report_writer *writer, const zlrs_mod_cfg_t *mod)
{
	if (mod->modulation == ZLRS_MOD_FLRC) {
		mgmt_debug_append(writer, "FLRC, %u.%03u Mbps", mod->rate_bps / 1000000U,
				  (mod->rate_bps / 1000U) % 1000U);
	} else {
		mgmt_debug_append(writer, "LoRa, SF%u / %u kHz", (unsigned int)mod->sf,
				  mod->bw_hz / 1000U);
	}
}

static void mgmt_debug_link(struct mgmt_debug_report_writer *writer, const char *name,
			    const zlrs_link_t *link, bool active)
{
	const zlrs_link_stats_t *const stats = &link->stats;
	const uint32_t frequency_hz = zlrs_fhss_current_hz(&link->fhss);

	mgmt_debug_append(writer, "\n[%s LINK]\n", name);
	mgmt_debug_append(writer, "  Hopping bands      : %s\n", mgmt_debug_bands(&link->cfg));
	mgmt_debug_append(writer, "  Link state / role  : %-12s / %s\n",
			  active ? mgmt_debug_state(link->state) : "unconfigured",
			  link->cfg.role == ZLRS_ROLE_TX ? "TX" : "RX");
	mgmt_debug_append(writer, "  Pairing / session  : %s / %s   Profile: P%u\n",
			  link->paired ? "paired" : "not paired",
			  link->session_valid ? "active" : "inactive",
			  (unsigned int)link->cfg.profile);
	mgmt_debug_append(writer, "  Frequency          : ");
	mgmt_debug_frequency(writer, frequency_hz);
	mgmt_debug_append(writer, "   Hop: %u   Frame: %u\n", (unsigned int)link->fhss.index,
			  link->frame_index);
	mgmt_debug_append(writer, "  PHY / TX setpoint  : ");
	mgmt_debug_phy(writer, &link->cfg.mod);
	mgmt_debug_append(writer, "   /   %d dBm\n", (int)link->cfg.tx_power_dbm);
	mgmt_debug_append(writer, "  RF frames          : TX %u   RX %u   Failed %u\n",
			  stats->tx_frames, stats->rx_frames, stats->radio_fail);
	if (stats->rx_frames != 0U && stats->last_rssi != 0) {
		mgmt_debug_append(writer,
				  "  Signal             : RSSI %d dBm   Link quality %u%%\n",
				  (int)stats->last_rssi, (unsigned int)stats->lq_percent);
	} else if (stats->rx_frames != 0U) {
		mgmt_debug_append(writer, "  Signal             : RSSI n/a   Link quality %u%%\n",
				  (unsigned int)stats->lq_percent);
	} else {
		mgmt_debug_append(writer, "  Signal             : no received frames yet\n");
	}
	mgmt_debug_append(writer, "  RF bytes           : TX %u B   RX %u B\n", stats->tx_bytes,
			  stats->rx_bytes);
	mgmt_debug_append(writer,
			  "  DATA               : %u packets   %u user B   full %u   ACK-only %u\n",
			  stats->tx_data_frames, stats->tx_data_bytes, stats->tx_full_data_frames,
			  stats->tx_ack_only_frames);
	mgmt_debug_append(writer,
			  "  ACK                : %u DATA ACKs   %u stale   %u window stalls\n",
			  stats->tx_data_ack_frames, stats->tx_staged_stale_ack_frames,
			  stats->tx_arq_window_stalls);
	mgmt_debug_append(writer,
			  "  ACK timing         : retry age %u/%u ms   confirmation %u/%u ms\n",
			  stats->tx_retry_age_ms_last, stats->tx_retry_age_ms_max,
			  stats->tx_ack_delay_ms_last, stats->tx_ack_delay_ms_max);
	mgmt_debug_append(writer, "  ACK totals         : %u ms across %u entries\n",
			  stats->tx_ack_delay_ms_sum, stats->tx_ack_entries);
	mgmt_debug_append(writer, "  Slots TX           : %u / %u / %u / %u\n", stats->slot_tx[0],
			  stats->slot_tx[1], stats->slot_tx[2], stats->slot_tx[3]);
	mgmt_debug_append(
		writer, "  Slots RX / hit     : %u / %u / %u / %u   |   %u / %u / %u / %u\n",
		stats->slot_rx[0], stats->slot_rx[1], stats->slot_rx[2], stats->slot_rx[3],
		stats->slot_hit[0], stats->slot_hit[1], stats->slot_hit[2], stats->slot_hit[3]);
	mgmt_debug_append(writer, "  Slot failures      : %u / %u / %u / %u\n", stats->slot_fail[0],
			  stats->slot_fail[1], stats->slot_fail[2], stats->slot_fail[3]);
	mgmt_debug_append(writer, "  Pairing TX/RX      : %u/%u/%u  /  %u/%u/%u   rejected %u\n",
			  stats->pair_req_tx, stats->pair_resp_tx, stats->pair_cfm_tx,
			  stats->pair_req_rx, stats->pair_resp_rx, stats->pair_cfm_rx,
			  stats->pair_rejects);
	mgmt_debug_append(writer,
			  "  Session TX/RX      : SYNC %u/%u   response %u/%u   confirm %u/%u\n",
			  stats->tx_syncs, stats->rx_syncs, stats->sess_resp_tx,
			  stats->sess_resp_rx, stats->sess_cfm_tx, stats->sess_cfm_rx);
	mgmt_debug_append(
		writer,
		"  ARQ / receive      : retries %u   restarts %u   dup %u   out-of-order %u\n",
		stats->tx_retries, stats->tx_arq_drop, stats->rx_dup, stats->rx_out_of_order);
	mgmt_debug_append(writer, "  Data loss          : fragment drops %u   dropped %u B\n",
			  stats->rx_frag_drop, stats->data_drop_bytes);
	mgmt_debug_append(writer, "  Queues             : TX %u   ARQ %u   reassembly %u B\n",
			  (unsigned int)link->queue_count, (unsigned int)link->arq_used,
			  (unsigned int)link->reasm.len);
	mgmt_debug_append(writer,
			  "  Sequence / ACK     : TX %u   RX %u   pending %u   recovery %u\n",
			  (unsigned int)link->arq_seq, (unsigned int)link->arq_rx_seq,
			  (unsigned int)link->arq_ack_pending, (unsigned int)link->arq_resync);
	mgmt_debug_append(
		writer, "  Decode errors      : tag %u   frame %u   hop advances %u   resync %u\n",
		stats->rx_bad_tag, stats->rx_bad_frame, stats->hop_advances, stats->hop_resync);
	mgmt_debug_append(writer, "  Cadence            : telemetry 1/%u   data 1/%u   sync 1/%u\n",
			  (unsigned int)link->cfg.tlm_every_n, (unsigned int)link->cfg.data_every_n,
			  (unsigned int)link->cfg.resync_every_n);
}

static void mgmt_debug_rate(struct mgmt_debug_report_writer *writer)
{
	static const char *const names[2] = {"Control", "Data"};

	mgmt_debug_append(writer, "Link      Windows/s  Heard/s  Hit rate    TX/s  State\n");
	mgmt_debug_append(writer, "--------- ---------- -------- --------- ------- -----------\n");
	for (uint8_t i = 0U; i < ARRAY_SIZE(rate_last); i++) {
		const uint32_t span = (rate_last[i].span_ms != 0U) ? rate_last[i].span_ms : 1U;
		const uint32_t windows_x10 = (rate_last[i].wins * 10000U + span / 2U) / span;
		const uint32_t heard_x10 = (rate_last[i].hits * 10000U + span / 2U) / span;
		const uint32_t hit_x10 =
			(rate_last[i].wins != 0U)
				? (rate_last[i].hits * 1000U + rate_last[i].wins / 2U) /
					  rate_last[i].wins
				: 0U;
		const uint32_t tx_x10 = (rate_last[i].tx * 10000U + span / 2U) / span;

		mgmt_debug_append(writer, "%-9s %6u.%u %6u.%u %7u.%u%% %5u.%u %s\n", names[i],
				  windows_x10 / 10U, windows_x10 % 10U, heard_x10 / 10U,
				  heard_x10 % 10U, hit_x10 / 10U, hit_x10 % 10U, tx_x10 / 10U,
				  tx_x10 % 10U, mgmt_debug_state(rate_last[i].state));
	}
	mgmt_debug_append(writer, "\nHit rate = heard frames / listen windows. Rates use the "
				  "latest sample interval.\n"
				  "TX counts successful RF frames.\n");
}

static void mgmt_debug_radio(struct mgmt_debug_report_writer *writer)
{
	zlrs_radio_diag_t diag;
	struct zlrs_slot_timer_stats timer;
	const uint32_t protocol_ms = zlrs_slot_timer_now_ms();
	const uint32_t kernel_ms = k_uptime_get_32();
	const uint32_t max_airtime_us = zlrs_radio_airtime_us(&data_mod, ZLRS_FRAME_MAX_LEN);

	zlrs_radio_diag(&diag);
	zlrs_slot_timer_stats(&timer);
	mgmt_debug_append(writer, "[FRAME PLAN]\n");
	mgmt_debug_append(writer, "  Frame period       : %u.%03u ms\n", frame_period_us / 1000U,
			  frame_period_us % 1000U);
	mgmt_debug_append(writer, "  Slot budget        : %u.%03u ms   Preparation: %u.%03u ms\n",
			  slot_budget_us / 1000U, slot_budget_us % 1000U, slot_prepare_us / 1000U,
			  slot_prepare_us % 1000U);
	mgmt_debug_append(writer, "  Data MTU           : %u B   Full-frame airtime: %u.%03u ms\n",
			  (unsigned int)data_mtu, max_airtime_us / 1000U, max_airtime_us % 1000U);
	mgmt_debug_append(writer, "  Launch guard       : %u us\n",
			  (unsigned int)CONFIG_ZLRS_APP_TX_GUARD_US);

	mgmt_debug_append(writer, "\n[CLOCK / TIMER]\n");
	mgmt_debug_append(writer, "  TIMER1 / kernel    : %u / %u ms   Delta: %+d ms\n",
			  protocol_ms, kernel_ms, (int32_t)(protocol_ms - kernel_ms));
	mgmt_debug_append(writer, "  Boundaries         : %u   IRQ max lateness: %u us\n",
			  timer.boundaries, timer.irq_late_us_max);
	mgmt_debug_append(writer, "  Missed / skipped   : %u boundaries   %u frames   %u slots\n",
			  timer.missed_boundaries, skipped_frames, missed_slots);
	mgmt_debug_append(
		writer, "  Prepare wait       : skipped %u   late %u us   IRQ %u us   wake %u us\n",
		prepare_wait_skipped, prepare_wait_late_us_max, prepare_irq_late_us_max,
		prepare_wake_us_max);
	mgmt_debug_append(writer,
			  "  Phase correction   : error %+d us   applied %+d us   hits %u\n",
			  (int)phase_err_us, (int)phase_last_us, phase_hits);

	mgmt_debug_append(writer, "\n[RADIO DRIVER]\n");
	mgmt_debug_append(writer, "  RAL transactions   : %u   TX done %u   RX packets %u\n",
			  diag.transactions, diag.tx_done, diag.rx_packets);
	mgmt_debug_append(writer, "  Overrun / late     : %u / %u   Empty IRQ: %u\n", diag.overruns,
			  diag.late_starts, diag.empty_irq);
	mgmt_debug_append(writer, "  Setup last / max   : %u / %u us   TX %u us   RX %u us\n",
			  diag.submit_us_last, diag.submit_us_max, diag.tx_us_last,
			  diag.rx_us_last);
	mgmt_debug_append(writer, "  IRQ status         : 0x%08x   Stale IRQs: %u (0x%08x)\n",
			  diag.irq_last, diag.stale_irq, diag.stale_irq_bits);

	mgmt_debug_append(writer, "\n[TRANSMIT TIMING]\n");
	mgmt_debug_append(writer, "  Late launch        : %u attempts   last / max %u / %u us\n",
			  diag.prepare_late_starts + diag.trigger_late_starts,
			  diag.launch_late_us_last, diag.launch_late_us_max);
	mgmt_debug_append(writer, "  Late by phase      : prepare %u   trigger %u\n",
			  diag.prepare_late_starts, diag.trigger_late_starts);
	mgmt_debug_append(writer, "  Launch IRQ delay   : last / max %u / %u us   Late IRQ %u\n",
			  diag.launch_irq_late_us_last, diag.launch_irq_late_us_max,
			  diag.late_trigger_irq);
	mgmt_debug_append(writer, "  Launch wake delay  : last / max %u / %u us   Late wake %u\n",
			  diag.launch_wake_us_last, diag.launch_wake_us_max,
			  diag.late_trigger_wake);
	mgmt_debug_append(writer, "  Preparation cost   : entry %u us   build %u us   SPI %u us\n",
			  late_prepare_entry_us, late_prepare_build_us, late_prepare_spi_us);
	mgmt_debug_append(
		writer, "  Launch command     : last / max %u / %u us   FIFO lead %u us\n",
		diag.launch_cmd_us_last, diag.launch_cmd_us_max, diag.preload_lead_us_last);
	mgmt_debug_append(writer, "  Jitter / startup   : %u / %u us   TX startup %u us\n",
			  diag.launch_jitter_us_last, diag.launch_jitter_us_max,
			  diag.tx_startup_us_last);
	mgmt_debug_append(writer, "  TX command         : last / max %u / %u us\n",
			  diag.tx_launch_cmd_us_last, diag.tx_launch_cmd_us_max);
	mgmt_debug_append(
		writer,
		"  TX setup phases    : standby/IRQ %u   modem %u   FIFO %u   FS/BUSY %u us\n",
		diag.tx_prepare_us_last[0], diag.tx_prepare_us_last[1], diag.tx_prepare_us_last[2],
		diag.tx_prepare_us_last[3]);
	mgmt_debug_append(writer, "  FIFO preload       : %u packets   %u used   %u / %u us\n",
			  diag.tx_preloaded, diag.tx_preload_used, diag.tx_preload_us_last,
			  diag.tx_preload_us_max);
	mgmt_debug_append(writer, "  FIFO tail          : %u   %u B   errors %u   underflows %u\n",
			  diag.tx_fifo_tails, diag.tx_fifo_bytes, diag.tx_fifo_errors,
			  diag.tx_fifo_underflows);
	mgmt_debug_append(writer, "  FIFO ready         : last / max %u / %u us\n",
			  diag.tx_fifo_ready_us_last, diag.tx_fifo_ready_us_max);

	mgmt_debug_append(writer, "\n[RECEIVE TIMING]\n");
	mgmt_debug_append(writer,
			  "  Decode errors      : header %u   CRC %u   length %u   payload %u\n",
			  diag.rx_header_errors, diag.rx_crc_errors, diag.rx_length_errors,
			  diag.rx_payload_errors);
	mgmt_debug_append(writer,
			  "  Window outcomes    : expired %u   incomplete %u   restarted %u\n",
			  diag.rx_expired, diag.rx_incomplete, diag.rx_restarted);
	mgmt_debug_append(writer, "  RX finish          : last / max %u / %u us\n",
			  diag.rx_finish_us_last, diag.rx_finish_us_max);
	mgmt_debug_append(writer, "  Link processing    : last / max %u / %u us\n",
			  diag.rx_protocol_us_last, diag.rx_protocol_us_max);
	mgmt_debug_append(writer,
			  "  FIFO reads         : %u chunks   %u B   %u / %u us   coalesced %u\n",
			  diag.rx_fifo_chunks, diag.rx_fifo_bytes, diag.rx_fifo_us_last,
			  diag.rx_fifo_us_max, diag.rx_done_coalesced);
	mgmt_debug_append(writer, "  IRQ service        : last / max %u / %u us   TX expired %u\n",
			  diag.irq_service_us_last, diag.irq_service_us_max, diag.tx_expired);

	mgmt_debug_append(writer, "\n[BUILD / RADIO HEALTH]\n");
	mgmt_debug_append(writer, "  Serial / protocol  : %u / %u us\n", serial_pump_us_last,
			  protocol_build_us_last);
	mgmt_debug_append(writer, "  Packet build       : last / max %u / %u us\n",
			  packet_build_us_last, packet_build_us_max);
	mgmt_debug_append(writer, "  FS reused          : %u   TX startup max: %u us\n",
			  diag.fs_reused, diag.tx_startup_us_max);
	mgmt_debug_append(writer, "  Chip errors        : 0x%04x\n",
			  (unsigned int)diag.chip_errors);
	if (diag.chip_error_freq_hz != 0U) {
		mgmt_debug_append(writer, "  Last error channel : ");
		mgmt_debug_frequency(writer, diag.chip_error_freq_hz);
		mgmt_debug_append(writer, "\n");
	} else {
		mgmt_debug_append(writer, "  Last error channel : none recorded\n");
	}
}

static int mgmt_debug_render(uint8_t report_id, struct mgmt_debug_report_writer *writer)
{
	switch (report_id) {
	case ZLRS_MGMT_DEBUG_REPORT_LINK:
		mgmt_debug_link(writer, "CONTROL", &link_control, ZLRS_CONTROL_ENABLED);
		mgmt_debug_link(writer, "DATA", &link_data, ZLRS_DATA_ENABLED);
		return ZLRS_MGMT_OK;
	case ZLRS_MGMT_DEBUG_REPORT_RATE:
		mgmt_debug_rate(writer);
		return ZLRS_MGMT_OK;
	case ZLRS_MGMT_DEBUG_REPORT_RADIO:
		mgmt_debug_radio(writer);
		return ZLRS_MGMT_OK;
	case ZLRS_MGMT_DEBUG_REPORT_POWER: {
		int8_t current_dbm = 0;
		uint32_t raises = 0U;
		uint32_t lowers = 0U;
		uint32_t fallbacks = 0U;
		const int8_t subg_ceiling = (region != NULL)
						    ? MIN(tx_power_dbm, region->max_tx_power_dbm)
						    : tx_power_dbm;

		mgmt_debug_append(writer, "[POWER LIMITS]\n");
		mgmt_debug_append(writer, "  Configured target      : %+d dBm\n",
				  (int)tx_power_dbm);
		mgmt_debug_append(writer, "  Sub-G region cap       : %s / %+d dBm\n",
				  (region != NULL) ? region->name : "unknown",
				  (region != NULL) ? (int)region->max_tx_power_dbm : 0);
		mgmt_debug_append(writer, "  Sub-G link ceiling     : %+d dBm\n",
				  (int)subg_ceiling);

		mgmt_debug_append(writer, "\n[PER-LINK TX SETPOINTS]\n");
		mgmt_debug_append(writer, "  %-8s %-20s %-12s %s\n", "Link", "RF bands", "Mode",
				  "LR2021 target");
		mgmt_debug_append(writer, "  %-8s %-20s %-12s %s\n", "--------",
				  "--------------------", "------------", "-------------");
		mgmt_debug_append(writer, "  %-8s %-20s %-12s %+4d dBm\n", "Control",
				  mgmt_debug_bands(&link_control.cfg),
				  ZLRS_CONTROL_ENABLED ? "enabled" : "unconfigured",
				  (int)link_control.cfg.tx_power_dbm);
		mgmt_debug_append(writer, "  %-8s %-20s %-12s %+4d dBm\n", "Data",
				  mgmt_debug_bands(&link_data.cfg),
				  ZLRS_DATA_ENABLED ? "enabled" : "unconfigured",
				  (int)link_data.cfg.tx_power_dbm);
#if IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)
		zlrs_link_power_stats(&current_dbm, &raises, &lowers, &fallbacks);
		const int8_t floor_dbm = (int8_t)MIN((int)CONFIG_ZLRS_APP_DYNAMIC_POWER_FLOOR_DBM,
						     (int)subg_ceiling);

		mgmt_debug_append(writer, "\n[ADAPTIVE CONTROL POWER]\n");
		if (ZLRS_CONTROL_ENABLED) {
			mgmt_debug_append(writer,
					  "  Current / range      : %+d dBm / %+d..%+d dBm\n",
					  (int)current_dbm, (int)floor_dbm, (int)subg_ceiling);
			mgmt_debug_append(writer,
					  "  Changes up / down    : %u / %u   Fallbacks: %u\n",
					  raises, lowers, fallbacks);
		} else {
			mgmt_debug_append(
				writer,
				"  Status               : unconfigured (control link disabled)\n");
		}
#else
		mgmt_debug_append(writer, "\n[ADAPTIVE CONTROL POWER]\n  Disabled\n");
#endif
		mgmt_debug_append(
			writer, "\nSub-G control is clamped to its regional ceiling; 2.4 GHz data "
				"uses the configured target.\n"
				"These are LR2021 setpoints; RF output is not measured.\n");
		return ZLRS_MGMT_OK;
	}
	default:
		return ZLRS_MGMT_E_PARAM;
	}
}

static int mgmt_debug_report_read(uint8_t report_id, uint16_t offset, uint8_t *out,
				  uint8_t capacity, uint16_t *total_len, uint8_t *written)
{
	if (out == NULL || total_len == NULL || written == NULL ||
	    capacity > ZLRS_MGMT_DEBUG_REPORT_CHUNK_MAX) {
		return ZLRS_MGMT_E_PARAM;
	}

	if (offset == 0U) {
		struct mgmt_debug_report_writer writer = {0};
		const int rc = mgmt_debug_render(report_id, &writer);

		if (rc != ZLRS_MGMT_OK) {
			return rc;
		}
		if (writer.failed || writer.len > UINT16_MAX) {
			return ZLRS_MGMT_E_STATE;
		}
		mgmt_debug_report_len = (uint16_t)writer.len;
		mgmt_debug_report_id = report_id;
	} else if (report_id != mgmt_debug_report_id || offset > mgmt_debug_report_len) {
		return ZLRS_MGMT_E_PARAM;
	}

	const uint16_t remaining = mgmt_debug_report_len - offset;
	const uint8_t chunk_len = (remaining < capacity) ? (uint8_t)remaining : capacity;

	memcpy(out, &mgmt_debug_report[offset], chunk_len);
	*total_len = mgmt_debug_report_len;
	*written = chunk_len;

	return ZLRS_MGMT_OK;
}
#endif

#if ZLRS_MGMT_ENABLED
/* The shell entry only sets this flag: the session runs in the main loop (superframe stopped,
 * data port not stolen by the passthrough), see the main loop. */
static atomic_t mgmt_request;

/* Run one management session (ends on EXIT or idle timeout), then resume normal mode. Shared. */
static void enter_management_mode(void)
{
	const int previous_priority = k_thread_priority_get(k_current_get());

	/* Filesystem, configuration and USB replies use ordinary thread context. */
	k_thread_priority_set(k_current_get(), CONFIG_MAIN_THREAD_PRIORITY);
	LOG_WRN("entering management mode");

	/* 0 = EXIT, 1 = timeout, -1 = no usable port: the log has to tell them apart */
	{
		const zlrs_mgmt_ctx_t ctx = {
			.control = &link_control,
			.data = &link_data,
			.active = active_link(),
			.mode = operating_mode,
			.failsafe = failsafe_payload,
			.device_id = device_id,
			.region_id = region_id,
			.tx_power_dbm = tx_power_dbm,
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
			.debug_report = mgmt_debug_report_read,
#endif
		};
		const int rc = zlrs_mgmt_mode_run(&ctx);

		if (rc != 0) {
			LOG_WRN("management mode ended without EXIT (rc %d)", rc);
		}
	}
	memcpy(rc_payload, failsafe_payload, sizeof(rc_payload));
	TRACE('2');
	zlrs_link_set_rc(&link_control, rc_payload, sizeof(rc_payload));

	LOG_INF("management mode done, back to normal");
	/* Confirmation and log persistence both write flash; finish them before resuming RF. */
	confirm_image();
	log_flush();
	k_thread_priority_set(k_current_get(), previous_priority);
}

/* ---- application-side actions the management protocol triggers ----------------- */

#endif /* ZLRS_MGMT_ENABLED */

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_SHELL)
/* Bench: print the init breadcrumbs (see trace_add) */
static int cmd_trace(const struct shell *sh, size_t argc, char **argv)
{
	char buf[sizeof(init_trace) + 1];
	const uint8_t len = init_trace_len;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (uint8_t i = 0U; i < len; i++) {
		buf[i] = (char)init_trace[i];
	}
	buf[len] = '\0';
	shell_print(sh, "init trace: [%s] len=%u", buf, (unsigned int)len);

	return 0;
}

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
	shell_print(sh, "radio  : exclusive RAL + LR2021, TIMER1 slots");

	return 0;
}

#if ZLRS_MGMT_ENABLED
static int cmd_mgmt(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	atomic_set(&mgmt_request, 1);
	shell_print(
		sh,
		"management mode requested: the management CDC speaks the protocol, EXIT returns");

	return 0;
}
#endif

/*
 * Data port endpoint state, for bench debugging. Picking an endpoint bit us twice on this
 * board: first DTR (never reaches us over usbipd), then "host bytes seen". This dumps both.
 */
static int cmd_ports(const struct shell *sh, size_t argc, char **argv)
{
	zlrs_link_data_state_t states[2];
	const size_t count = zlrs_link_data_state(states, ARRAY_SIZE(states));

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
		shell_print(sh, "                 rx_queue=%u tx_queue=%u rx_paused=%d",
			    states[i].rx_queued, states[i].tx_queued, states[i].rx_paused);
	}
	shell_print(sh, "air-to-port=%u B host_drop=%u B", counters.data_rx,
		    zlrs_link_data_tx_dropped());

	return 0;
}

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
static int cmd_recovery(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "rebooting into MCUboot serial recovery");
	(void)zlrs_platform_recovery_enter(); /* does not return */

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
	shell_print(sh, "confirmed by host: %d", (int)zlrs_mgmt_mode_image_confirmed());
	mcuboot_dump_slot(sh, "slot0", PARTITION_ID(slot0_partition));
	mcuboot_dump_slot(sh, "slot1", PARTITION_ID(slot1_partition));

	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	zlrs_cmds, SHELL_CMD(status, NULL, "role / SKU / region / link state", cmd_status),
	SHELL_CMD(ports, NULL, "state of both data port endpoints", cmd_ports),
	SHELL_CMD(trace, NULL, "print the init breadcrumbs", cmd_trace),
#if ZLRS_MGMT_ENABLED
	SHELL_CMD(mgmt, NULL, "enter management mode (EXIT returns)", cmd_mgmt),
#endif
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	SHELL_CMD(recovery, NULL, "reboot into MCUboot serial recovery", cmd_recovery),
#endif
#ifdef PICO_RP2350
	SHELL_CMD(bootsel, NULL, "reboot into the RP2350 ROM bootloader (USB drive)", cmd_bootsel),
#endif
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	SHELL_CMD(mcuboot, NULL, "image state as MCUboot sees it (debug)", cmd_mcuboot),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(zlrs, &zlrs_cmds, "zlrs debug commands", NULL);
#endif /* CONFIG_ZLRS_APP_DEBUG_SHELL */

int main(void)
{
	TRACE('0');
	/* Debug image only (gated in Kconfig, so the symbol does not exist in production builds):
	 * USB is up, so wait for the host to open the console port — otherwise a panic log lands
	 * on a port nobody has open. */
	/*
	 * Debug image only: wait for the host to open the console port. Feed the watchdog while
	 * waiting — it is armed before main() runs, so a wait longer than its timeout would reset the
	 * device mid-boot (and look exactly like a broken image).
	 */
#ifdef CONFIG_ZLRS_APP_DEBUG_BOOT_DELAY_MS
	for (uint32_t waited = 0U; waited < (uint32_t)CONFIG_ZLRS_APP_DEBUG_BOOT_DELAY_MS;
	     waited += 100U) {
		zlrs_watchdog_feed();
		k_msleep(MIN(100U, (uint32_t)CONFIG_ZLRS_APP_DEBUG_BOOT_DELAY_MS - waited));
	}
#endif

	TRACE('M');
	LOG_INF("ZLRS %s", APP_VERSION_STRING);
	TRACE('V');

	/* What the build is, and what this board has: ports and peripherals come from devicetree,
	 * so a missing one is reported without being an error (app/src/platform/caps.h) */
	if (!ZLRS_CONTROL_ENABLED) {
		LOG_WRN("isolation: control link off, only the data link runs "
			"(pairing uses the data link)");
	}
	if (!ZLRS_DATA_ENABLED) {
		LOG_WRN("isolation: data link off, no serial passthrough");
	}
	if (!ZLRS_CRSF_ENABLED) {
		LOG_WRN("no CRSF port on this board, TX sends the configured failsafe values");
	}
	if (!ZLRS_MGMT_ENABLED) {
		LOG_WRN("no management port on this board, parameters can only come from the "
			"defaults");
	}

	/*
	 * Bring NVS up first: region, power, keys, pairing, failsafe values and baud all come from
	 * it. A broken NVS is survivable, but the consequences are worth spelling out: nothing can
	 * be read or written, so the device stays unconfigured and off the air. store logs its own error.
	 */
	TRACE('S');
	if (zlrs_storage_settings_init() != 0) {
		LOG_ERR("store unavailable: keys, pairing and failsafe will not persist");
	}

#if ZLRS_MGMT_ENABLED
	/*
	 * Bring the management port up early: later fatal configuration steps land in
	 * fault_loop, and that path stays flashable only while the management port is alive.
	 */
	(void)zlrs_mgmt_init();
	TRACE('C');
#endif

	/* Identity comes before anything that could use it; without one, stay off the air */
	if (!ensure_device_id()) {
		TRACE('I');
		LOG_ERR("no device id: staying off the air until one can be made");
	} else {
		TRACE('D');
	}

	/*
	 * Region and power ceiling are runtime parameters (the management protocol writes them): use the
	 * stored value when present, the board's own default otherwise (app/src/platform/caps.h).
	 * The management commands validate whatever they are asked to store (app/src/mgmt/mode.c).
	 */
	operating_mode =
		IS_ENABLED(CONFIG_ZLRS_APP_LINK_CONTROL) ? ZLRS_MODE_MIXED : ZLRS_MODE_DATA24;
	{
		uint8_t stored_mode;

		if (zlrs_storage_settings_load(ZLRS_SETTING_MODE, &stored_mode,
					       sizeof(stored_mode)) == (int)sizeof(stored_mode) &&
		    ((stored_mode == ZLRS_MODE_DATA24 && ZLRS_DATA_ENABLED) ||
		     (stored_mode == ZLRS_MODE_MIXED &&
		      IS_ENABLED(CONFIG_ZLRS_APP_LINK_CONTROL)))) {
			operating_mode = stored_mode;
		}
	}
	LOG_INF("operating mode: %s", ZLRS_DATA24 ? "data24" : "mixed");
	region_id = ZLRS_DEFAULT_REGION;
	tx_power_dbm = CONFIG_ZLRS_APP_TX_POWER_DBM;
	{
		uint8_t stored_region;
		int8_t stored_power;

		if (zlrs_storage_settings_load(ZLRS_SETTING_REGION, &stored_region,
					       sizeof(stored_region)) ==
		    (int)sizeof(stored_region)) {
			region_id = stored_region;
		}
		if (zlrs_storage_settings_load(ZLRS_SETTING_POWER, &stored_power,
					       sizeof(stored_power)) == (int)sizeof(stored_power)) {
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
	/* What the build believes the hardware is, derived from devicetree (app/src/platform/caps.h) */
	LOG_INF("caps: ports=%s%s%s usb=%c btn=%c led=%c wdt=%c rf=%s(%u-%u MHz)",
		ZLRS_PORT_CTRL_FIXED ? "control" : "", ZLRS_PORT_DATA_FIXED ? ",data" : "",
		ZLRS_PORT_MGMT ? ",cfg" : "", ZLRS_HAS_USB ? 'y' : 'n', ZLRS_HAS_BUTTON ? 'y' : 'n',
		ZLRS_HAS_LED ? 'y' : 'n', ZLRS_HAS_WDT ? 'y' : 'n',
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

		if (zlrs_storage_settings_load(ZLRS_SETTING_ROLE, &stored_role,
					       sizeof(stored_role)) == (int)sizeof(stored_role) &&
		    stored_role <= ZLRS_ROLE_RX) {
			role_tx = (stored_role == ZLRS_ROLE_TX);
		}
		if (zlrs_storage_settings_load(ZLRS_SETTING_PROFILE, &stored_profile,
					       sizeof(stored_profile)) ==
		    (int)sizeof(stored_profile)) {
			if (zlrs_mgmt_mode_profile_allowed(stored_profile)) {
				active_profile = stored_profile;
			} else {
				LOG_WRN("stored profile P%u is not available in this image, using "
					"P%u",
					(unsigned int)stored_profile,
					(unsigned int)ZLRS_PROFILE_DEFAULT);
			}
		}
	}
	LOG_INF("role %s, profile P%u", role_tx ? "TX" : "RX", (unsigned int)active_profile);

	/* Bench switches: with these off the link is no longer production-shaped, so say so at boot */
	if (active_profile != ZLRS_PROFILE_P1) {
		LOG_WRN("isolation: security profile P0, frames carry no tag");
	}
	if (!IS_ENABLED(CONFIG_ZLRS_APP_FHSS)) {
		LOG_WRN("isolation: hopping off, every frame stays on the sync channel");
	}
	if (!IS_ENABLED(CONFIG_ZLRS_APP_SESSION_HANDSHAKE)) {
		LOG_WRN("isolation: session handshake off, pairing keys are session keys");
	}
	if (!IS_ENABLED(CONFIG_ZLRS_APP_TELEMETRY)) {
		LOG_WRN("isolation: telemetry off, control link is RC-only");
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
	/* The status LED comes up on its own thread and reads the state it renders: nothing to
	 * init here (app/src/ui/led.c, app/src/state/sys_bus.h) */

	if (zlrs_slot_timer_init() != 0) {
		LOG_ERR("hardware slot timer init failed");
		return 0;
	}

	TRACE('1');
	plan_init();
	build_links();
	load_identity();
	failsafe_init();

	/* The data port must come up before management mode: the session reads that port directly */
	{
		uint32_t baud = 0U;

		if (zlrs_storage_settings_load(ZLRS_SETTING_BAUD, &baud, sizeof(baud)) !=
		    (int)sizeof(baud)) {
			baud = 0U;
		}
		(void)zlrs_link_data_init(baud);
	}

	zlrs_link_set_rc(&link_control, rc_payload, sizeof(rc_payload));
	if (ZLRS_CRSF_ENABLED) {
		(void)zlrs_link_crsf_init();
	}

	TRACE('c');
	if (zlrs_radio_init() != 0) {
		TRACE('X');
		LOG_ERR("radio driver init failed");
		return 0;
	}

	TRACE('d');

	LOG_INF("entering superframe loop: %u Hz, %s / %s (%s)", (unsigned int)control_rate_hz,
		ZLRS_CONTROL_ENABLED ? "control on" : "control off",
		ZLRS_DATA_ENABLED ? "data on" : "data off", role_tx ? "TX" : "RX");

	/*
	 * Init is done at this point (ports, NVS, links, USB). Image confirmation does not happen
	 * here: after an upgrade it waits for the host's explicit confirm (see confirm_image).
	 */

	log_flush();
	zlrs_slot_timer_start(frame_period_us * CONFIG_ZLRS_APP_PACING_SCALE,
			      slot_prepare_us, zlrs_radio_wake);
	uint32_t consumed_frame = 0U;

	if (ZLRS_DATA24) {
		/* RF servicing must resume at DMA/radio/timer events even when an upstream
		 * cooperative USB worker is active. Serial notifications are deferred. */
		BUILD_ASSERT(CONFIG_NUM_METAIRQ_PRIORITIES >= 1);
		k_thread_priority_set(k_current_get(), K_PRIO_COOP(0));
	}

	for (;;) {
		uint32_t start_us;
		uint32_t timer_frame;

		zlrs_slot_timer_wait_frame(&start_us, &timer_frame);
		if ((int32_t)(timer_frame - consumed_frame) <= 0) {
			continue;
		}
		/* Dropped frames still advance frame/hop state on the original cadence. */
		while (++consumed_frame < timer_frame) {
			skipped_frames++;
			for (uint8_t slot = 0U; slot < 4U; slot++) {
				const bool data_slot = ZLRS_DATA24 || slot >= ZLRS_SLOT_DATA_A;
				zlrs_link_t *link = data_slot ? &link_data : &link_control;
				const uint8_t link_slot =
					ZLRS_DATA24 ? ((slot & 1U) == 0U ? ZLRS_SLOT_DATA_A
									 : ZLRS_SLOT_DATA_B)
						    : slot;

				if (data_slot ? !ZLRS_DATA_ENABLED : !ZLRS_CONTROL_ENABLED) {
					continue;
				}
				zlrs_link_skip_slot(link, link_slot);
			}
		}

		frame_start_us = start_us;
		phase_adj_set = false;
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
		if (bench.frames > 0U) {
			bench.frame_us_last = start_us - prev_frame_start_us;
			if (bench.frame_us_min == 0U || bench.frame_us_last < bench.frame_us_min) {
				bench.frame_us_min = bench.frame_us_last;
			}
			if (bench.frame_us_last > bench.frame_us_max) {
				bench.frame_us_max = bench.frame_us_last;
			}
		}
		prev_frame_start_us = start_us;
		bench.frames++;
#endif

		zlrs_watchdog_feed();
		confirm_image();

#if ZLRS_MGMT_ENABLED
		/* All management-mode entries converge here: management port command, shell, debug magic.
		 * During the session radio work pauses and the data port drops bytes; the clock continues. */
		if (zlrs_mgmt_poll_enter() || atomic_cas(&mgmt_request, 1, 0)) {
			/* This frame has not run any slots; include it in the catch-up after the pause. */
			consumed_frame--;
			/* Keep the absolute clock running: catch up hop state after this pause. */
			enter_management_mode();
			continue;
		}
#endif

		/*
		 * The timestamp must be re-read after a management session. Anything stamped inside the session
		 * (a pairing triggered from the management port) is later than the "now" taken before it, so
		 * unsigned subtraction underflows and the pairing window looks long expired.
		 */
		TRACE('L');
		const uint32_t period = frame_period_us * CONFIG_ZLRS_APP_PACING_SCALE;

		for (uint8_t slot = 0U; slot < 4U; slot++) {
			zlrs_watchdog_feed();
			slot_nominal_us = start_us + ((uint64_t)period * slot) / 4U;
			const uint32_t next_us =
				slot == 3U ? zlrs_slot_timer_next_frame_us()
					   : start_us + ((uint64_t)period * (slot + 1U)) / 4U;

			slot_deadline_us = next_us - slot_prepare_us - 100U;
			run_slot(slot);
		}

		/* Completions timestamp receptions during the slots; timeout checks need a newer time. */
		const uint32_t now_ms = zlrs_slot_timer_now_ms();

		if (ZLRS_CONTROL_ENABLED) {
			zlrs_link_tick(&link_control, now_ms);
		}
		if (ZLRS_DATA_ENABLED) {
			zlrs_link_tick(&link_data, now_ms);
		}

		adopt_pairing();
		pump_crsf(now_ms);
		/* The button thread reports holds on the bus (app/src/ui/button.c); the pairing request
		 * itself stays on this thread, where the link state machine lives. */
		{
			struct zlrs_input asked;

			if (zbus_chan_read(&zlrs_input_chan, &asked, K_NO_WAIT) == 0 &&
			    asked.button_holds != button_holds_seen) {
				button_holds_seen = asked.button_holds;
				zlrs_link_press(active_link(), now_ms);
			}
		}
		broadcast_state();
		report(now_ms);

		if (!role_tx) {
			const zlrs_link_t *const clock_link = active_link();
			const uint32_t cadence_us = ZLRS_DATA24 ? period / 2U : period;
			const uint32_t scan_after_ms = zlrs_slot_timer_scan_delay_ms(
				cadence_us, clock_link->cfg.resync_every_n, phase_samples > 0U);

			if ((uint32_t)(now_ms - last_rx_any_ms) > scan_after_ms) {
				phase_samples = 0U;
				phase_adj_us = ZLRS_PHASE_SCAN_STEP_US;
			}
			zlrs_slot_timer_shift(phase_adj_us);
		}
		phase_adj_us = 0;
	}

	return 0;
}

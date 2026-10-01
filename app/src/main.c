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
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/reboot.h>

#include <stdlib.h>
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

#include "zlrs/crsf/crsf.h"
#include "zlrs/mgmt/mgmt.h"
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
 * Band role (doc/07-architecture.md 2): control owns sub-G by default, data owns 2.4G;
 * racing mode swaps them. The regulatory gate (power, rate, duty) applies to the sub-G link.
 */
#if IS_ENABLED(CONFIG_ZLRS_APP_BAND_ROLE_SUBG_DATA)
#define CONTROL_ON_SUBG 0
#else
#define CONTROL_ON_SUBG 1
#endif

/* Fingerprint for the bench output: enough to compare two ends, not enough to leak the key */
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_SHELL) || IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
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
/* Identity: read or generated once at boot, before anything that could use it (doc/10-security.md 1) */
static uint8_t device_id[ZLRS_DEV_ID_LEN];

/*
 * Role and security profile: runtime settings with a compile-time default (set-role /
 * set-profile, doc/08-firmware.md 3). Loaded once at boot, before the links are built.
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

		/* Just stash it: the power verdict runs in report() at 1 Hz (doc/09-protocol.md 4.5) */
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
	const zlrs_region_t *ctl_region = !ZLRS_DATA24 && CONTROL_ON_SUBG ? region : NULL;
	const zlrs_region_t *data_region = ZLRS_DATA24 || CONTROL_ON_SUBG ? NULL : region;
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
	if (ZLRS_DATA24) {
		/* No sub-G modulation or regional duty cadence in exclusive 2.4 GHz mode. */
		control_mod = data_mod;
		req.want_hz = CONFIG_ZLRS_APP_CONTROL_RATE_HZ;
		/* Keep 1 ms more of each 7.35 ms slot available for data airtime. */
		slot_prepare_us = 5000U;
	}
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

		while (len > 16U && zlrs_radio_airtime_us(&data_mod, (uint16_t)(len + 9U)) +
						    slot_prepare_us +
						    CONFIG_ZLRS_APP_TX_GUARD_US +
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
		/* Bench switches, each can be turned off, see doc/12-debug.md 3 */
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
	 * An ACK can need two scheduled frames, including cadence thinning and bench pacing.
	 * Base the timeout on the actual timer period rather than the packet airtime alone.
	 */
	data_cfg.arq_timeout_ms =
		(uint16_t)MIN(UINT16_MAX, MAX(ZLRS_ARQ_TIMEOUT_MS,
					      DIV_ROUND_UP(2ULL * frame_period_us * data_every_n *
								   CONFIG_ZLRS_APP_PACING_SCALE,
							   1000U)));
	/*
	 * The data link is best-effort: it sends nothing when idle, so it has no peer-loss check,
	 * which would otherwise rebuild the session over and over while idle (doc/09-protocol.md 3.7).
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
	const uint32_t now_ms = k_uptime_get_32();

	zlrs_link_set_dev_id(&link_control, device_id);
	zlrs_link_set_dev_id(&link_data, device_id);

	/* Root key: NVS only, written once with the management tool (set-phrase / set-key-hex) */
	if (zlrs_storage_settings_load(ZLRS_SETTING_ROOT_KEY, root_key, sizeof(root_key)) ==
	    (int)ZLRS_KEY_LEN) {
		have_root_key = true;
	}

	if (!have_root_key) {
		LOG_WRN("no root key: staying off air until one is written (doc/10-security.md)");
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

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
/* Result of the at-boot raw bench, kept for `zlrs radio`: the console drops the log line often */
static uint32_t raw_bench_frames;
static uint32_t raw_bench_ms;
#endif

/* Wait against real elapsed time. Hardware alarms wake the thread at slot deadlines. */
static void radio_wait_done(uint32_t budget_us)
{
	const uint32_t deadline = zlrs_slot_timer_now_us() + budget_us;

	while ((int32_t)(deadline - zlrs_slot_timer_now_us()) > 0) {
		if (zlrs_radio_irq_pending()) {
			zlrs_radio_drain();
			return;
		}
		/* Radio IRQ and hardware slot boundary both wake this wait. */
		const int32_t remain = (int32_t)(deadline - zlrs_slot_timer_now_us());

		if (remain > 0) {
			(void)zlrs_radio_wait_us((uint32_t)remain);
		}
	}
}

/* Debug measurements of the absolute frame clock, slot work and authenticated RX offsets. */
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
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
static zlrs_radio_diag_t radio_diag;
#endif

/* TX owns the absolute clock; RX follows verified IRQ timestamps and scans when deaf. */
#define ZLRS_PHASE_DEADBAND_US   100
#define ZLRS_PHASE_SCAN_AFTER_MS 300U
#define ZLRS_PHASE_SCAN_STEP_US  25U

static uint32_t frame_start_us;
static uint32_t slot_deadline_us;
static uint32_t slot_nominal_us;
static uint32_t skipped_frames;
static uint32_t missed_slots;
static uint32_t packet_build_us_last;
static uint32_t packet_build_us_max;
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
	const bool data_slot =
		ZLRS_DATA24 || (slot == ZLRS_SLOT_DATA_A || slot == ZLRS_SLOT_DATA_B);
	const uint8_t link_slot =
		ZLRS_DATA24 ? ((slot & 1U) == 0U ? ZLRS_SLOT_DATA_A : ZLRS_SLOT_DATA_B) : slot;
	zlrs_link_t *link = data_slot ? &link_data : &link_control;
	const uint32_t slot_start_us = now_us();
	/* Slot decisions use the planned time even though construction happens before it. */
	const int32_t until_slot_us = (int32_t)(slot_nominal_us - slot_start_us);
	const uint32_t now_ms =
		k_uptime_get_32() + (until_slot_us > 0 ? (uint32_t)until_slot_us / 1000U : 0U);
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
	if (data_slot) {
		pump_serial();
	}
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

	if (zlrs_radio_prepare(link, req) != 0) {
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, k_uptime_get_32());
		return;
	}

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
	/* How much of a slot goes into the port's own submit path (vs. the air) */
	bench.submit_us_last = now_us() - slot_start_us;
	if (bench.submit_us_last > bench.submit_us_max) {
		bench.submit_us_max = bench.submit_us_last;
	}
#endif

	/* Modem, authentication and FIFO are ready: the timer wake leads only to SET_TX. */
	const int launch_rc =
		req->is_tx ? zlrs_radio_launch_at(slot_nominal_us + CONFIG_ZLRS_APP_TX_GUARD_US)
			   : zlrs_radio_launch();

	if (launch_rc != 0) {
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, k_uptime_get_32());
		return;
	}

	slot_off_us[slot] = slot_nominal_us - frame_start_us;
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
				   k_uptime_get_32());
	}

	if (link->stats.slot_hit[link_slot] != hit_before && link->stats.rx_frames != good_before) {
		uint32_t done_us;
		uint32_t airtime_us;

		last_rx_any_ms = k_uptime_get_32();
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

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
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
 * recovery command and the debug shell, see doc/10-security.md 9.
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
				   k_uptime_get_32());
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
	last_rc_input_ms = k_uptime_get_32();
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

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
/*
 * Bench: measure one link over a few seconds. Receive windows, how many of them heard a frame, and
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
 * Most recent one-second window, kept up to date by report(). The shell command must not block:
 * sleeping in the shell thread stalls the console CDC and the host drops the device (measured).
 */
static struct {
	uint32_t wins;
	uint32_t hits;
	uint32_t tx;
	uint8_t state;
} rate_last[2];

static void rate_update(uint8_t idx, const zlrs_link_t *link, const rate_snap_t *before,
			uint32_t span_ms)
{
	const rate_snap_t now = rate_snapshot(link);
	const uint32_t wins = now.wins - before->wins;
	const uint32_t hits = now.hits - before->hits;
	const uint32_t tx = now.tx - before->tx;
	const uint32_t span = (span_ms != 0U) ? span_ms : 1U;

	rate_last[idx].wins = (wins * 1000U) / span;
	rate_last[idx].hits = (hits * 1000U) / span;
	rate_last[idx].tx = (tx * 1000U) / span;
	rate_last[idx].state = link->state;
}
#endif

static void report(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
	/* Per-second link rates for `zlrs rate` (see below) */
	static rate_snap_t rate_before[2];
	static uint32_t rate_ms;
#endif

	if ((uint32_t)(now_ms - last_report_ms) < REPORT_PERIOD_MS) {
		return;
	}
	last_report_ms = now_ms;

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
	rate_update(0U, &link_control, &rate_before[0],
		    (rate_ms != 0U) ? now_ms - rate_ms : REPORT_PERIOD_MS);
	rate_update(1U, &link_data, &rate_before[1],
		    (rate_ms != 0U) ? now_ms - rate_ms : REPORT_PERIOD_MS);
	rate_before[0] = rate_snapshot(&link_control);
	rate_before[1] = rate_snapshot(&link_data);
	rate_ms = now_ms;
#endif

	zlrs_link_rate_tick(&link_control, now_ms);

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
	 * hears anything is a phase problem, not a range problem (see zlrs link for the rest).
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
	bench.frame_us_min = 0U;
	bench.frame_us_max = 0U;
	bench.rx_offset_min = 0U;
	bench.rx_offset_max = 0U;
	bench.submit_us_max = 0U;
	for (uint8_t i = 0U; i < 4U; i++) {
		bench.slot_us_max[i] = 0U;
	}
#endif /* CONFIG_ZLRS_APP_DEBUG_STATS_LOG */
}

/*
 * System state broadcast: the facts other parts of the application render or report
 * (app/src/state/sys_bus.h). Facts only — the LED thread turns them into colours (doc/11-ux.md).
 */
#if ZLRS_MGMT_ENABLED
static void enter_management_mode(void);
#endif

/*
 * State on a fatal error (region vs SKU mismatch, entropy self-test failure, PSA init failure):
 * alarm but stay reachable. Red LED, the management port still up, so the host can enter it,
 * trigger serial recovery and reflash. A regulatory or security problem is worth staying off
 * the air for, but the device must stay flashable — otherwise one bad image means BOOTSEL.
 *
 * Exception: a test image (not yet confirmed by the host) resets on a fatal error so that
 * MCUboot rolls back — a flashable fault state is no reason to keep a broken build running.
 */
/* Set when the entropy self-test never passed: keys then come from a weaker source */
static bool entropy_degraded;

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

	/* One fact, once: the fault outranks the boot self-test and shows as the red alert colour
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

	if (zlrs_mgmt_mode_image_confirmed()) {
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

#if ZLRS_MGMT_ENABLED
/* The shell entry only sets this flag: the session runs in the main loop (superframe stopped,
 * data port not stolen by the passthrough), see the main loop. */
static atomic_t mgmt_request;

/* Run one management session (ends on EXIT or idle timeout), then resume normal mode. Shared. */
static void enter_management_mode(void)
{
	LOG_WRN("entering management mode");
	log_flush();

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
}

/* ---- application-side actions the management protocol triggers ----------------- */

#endif /* ZLRS_MGMT_ENABLED */

#if CONFIG_ZLRS_APP_BENCH_RAW_MS > 0
/*
 * Bench helpers: run one radio transaction and report whether it finished. Debug images only — the
 * raw radio test is what says whether the two radios can hear each other at all, with no slots,
 * phases or ARQ involved (doc/12-debug.md 4).
 */
static bool bench_wait(zlrs_link_t *link, const zlrs_radio_req_t *req, uint32_t budget_us)
{
	if (zlrs_radio_prepare(link, req) != 0 || zlrs_radio_launch() != 0) {
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, k_uptime_get_32());
		return false;
	}

	radio_wait_done(budget_us);

	zlrs_radio_drain();
	if (link->req_pending) {
		zlrs_radio_abort();
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, k_uptime_get_32());
		return false;
	}

	return true;
}

/*
 * Raw radio bench at boot, see CONFIG_ZLRS_APP_BENCH_RAW_MS. The role decides the direction: the TX
 * role sends complete frames back to back, the RX role opens 100 ms windows back to back. Both log
 * once a second, which is all the host needs — the console stays usable because nothing has to be
 * typed while this runs.
 */
static void bench_raw_at_boot(void)
{
	uint32_t left_ms = (uint32_t)CONFIG_ZLRS_APP_BENCH_RAW_MS;
	uint32_t last_ms = k_uptime_get_32();
	uint32_t tx0 = link_data.stats.tx_frames;
	uint32_t rx0 = link_data.stats.rx_frames;
	static uint8_t payload[32];
	const uint32_t start_ms = last_ms;
	uint32_t seq = 0U;

	if (left_ms == 0U) {
		return;
	}

	LOG_INF("raw bench: %s for %u ms", role_tx ? "blast" : "listen", (unsigned int)left_ms);

	while ((uint32_t)(k_uptime_get_32() - start_ms) < left_ms) {
		const zlrs_radio_req_t *req;

		if (role_tx) {
			for (uint8_t i = 0U; i < sizeof(payload); i++) {
				payload[i] = (uint8_t)(seq + i);
			}
			req = zlrs_link_blast(&link_data, payload, sizeof(payload));
			if (req != NULL) {
				seq++;
			}
		} else {
			req = zlrs_link_listen_raw(&link_data, 100U);
		}
		if (req == NULL) {
			k_usleep(200);
			continue;
		}
		/* The direct backend programs the radio itself per transaction, which needs more room than
		 * the planner path: give the bench a budget that is not the thing being measured. */
		(void)bench_wait(&link_data, req, role_tx ? 100000U : 120000U);
		zlrs_watchdog_feed();

		{
			const uint32_t now = k_uptime_get_32();

			if ((uint32_t)(now - last_ms) >= 1000U) {
				const uint32_t tx = link_data.stats.tx_frames - tx0;
				const uint32_t rx = link_data.stats.rx_frames - rx0;

				LOG_INF("raw: %s %u tx/s, %u rx/s, rssi %d",
					role_tx ? "blast" : "listen", (unsigned int)tx,
					(unsigned int)rx, link_data.stats.last_rssi);
				tx0 = link_data.stats.tx_frames;
				rx0 = link_data.stats.rx_frames;
				last_ms = now;
			}
		}
	}

	LOG_INF("raw bench done: %u frames sent, %u heard", (unsigned int)seq,
		(unsigned int)(link_data.stats.rx_frames - rx0));
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
	/* Kept for `zlrs radio`: the console drops this line often enough to lose the measurement */
	raw_bench_frames = seq;
	raw_bench_ms = (uint32_t)CONFIG_ZLRS_APP_BENCH_RAW_MS;
#endif
}

#endif /* CONFIG_ZLRS_APP_BENCH_RAW_MS */

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_SHELL)
/* Little-endian u32 out of an id or key, for bench output that has to be comparable */
static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void print_link(const struct shell *sh, const char *name, const zlrs_link_t *link)
{
	/* Little-endian u32 out of the pairing salt, printed as the two nonces they are */
	const uint8_t *salt = link->pair_salt;
	const uint32_t salt_a = le32(salt);
	const uint32_t salt_b = le32(&salt[4]);

	/*
	 * hop is the index both ends have to agree on; frame is this end's own frame counter and
	 * adv how often it has moved the table, which is what shows where a desync comes from.
	 */
	shell_print(sh,
		    "%-7s state=%u paired=%u sess=%u freq=%u hop=%u frame=%u adv=%u resync=%u"
		    " pair_key=%08x word=%08x",
		    name, link->state, link->paired ? 1U : 0U, link->session_valid ? 1U : 0U,
		    zlrs_fhss_current_hz(&link->fhss), link->fhss.index, link->frame_index,
		    link->stats.hop_advances, link->stats.hop_resync,
		    fingerprint(link->pair_key, sizeof(link->pair_key)),
		    fingerprint(link->sync_word, sizeof(link->sync_word)));
	shell_print(sh, "        tx=%u rx=%u fail=%u rssi=%d..%d lq=%u%%", link->stats.tx_frames,
		    link->stats.rx_frames, link->stats.radio_fail, link->stats.rssi_min,
		    link->stats.rssi_max, link->stats.lq_percent);
	shell_print(sh,
		    "        slot tx %u/%u/%u/%u rx %u/%u/%u/%u got %u/%u/%u/%u fail %u/%u/%u/%u",
		    link->stats.slot_tx[0], link->stats.slot_tx[1], link->stats.slot_tx[2],
		    link->stats.slot_tx[3], link->stats.slot_rx[0], link->stats.slot_rx[1],
		    link->stats.slot_rx[2], link->stats.slot_rx[3], link->stats.slot_hit[0],
		    link->stats.slot_hit[1], link->stats.slot_hit[2], link->stats.slot_hit[3],
		    link->stats.slot_fail[0], link->stats.slot_fail[1], link->stats.slot_fail[2],
		    link->stats.slot_fail[3]);
	shell_print(sh, "        pair tx %u/%u/%u rx %u/%u/%u rej=%u sent=%u pending=%u wait=%u",
		    link->stats.pair_req_tx, link->stats.pair_resp_tx, link->stats.pair_cfm_tx,
		    link->stats.pair_req_rx, link->stats.pair_resp_rx, link->stats.pair_cfm_rx,
		    link->stats.pair_rejects, link->pair_cfm_sent, link->pair_cfm_pending ? 1U : 0U,
		    link->pair_wait_cfm ? 1U : 0U);
	shell_print(sh, "        sync tx=%u rx=%u resp %u+%u cfm %u+%u", link->stats.tx_syncs,
		    link->stats.rx_syncs, link->stats.sess_resp_tx, link->stats.sess_resp_rx,
		    link->stats.sess_cfm_tx, link->stats.sess_cfm_rx);
	/* Paired ends must agree on the id pair and the salt: a mismatch here is why keys differ */
	shell_print(sh, "        dev=%08x peer=%08x salt=%08x/%08x root=%08x auth=%08x",
		    le32(link->dev_id), le32(link->peer_id), salt_a, salt_b,
		    fingerprint(link->root_key, sizeof(link->root_key)),
		    fingerprint(link->auth_key, sizeof(link->auth_key)));
	/* Raw key words: two ends must agree byte for byte, fingerprints hide which half differs */
	shell_print(sh, "        key=%08x%08x%08x%08x", le32(link->pair_key),
		    le32(&link->pair_key[4]), le32(&link->pair_key[8]), le32(&link->pair_key[12]));
	/* Data path health: what the ARQ gave up on, and what the reassembler threw away */
	shell_print(sh, "        arq retx=%u restarts=%u | rx dup=%u ooo=%u frag_drop=%u lost=%u B",
		    link->stats.tx_retries, link->stats.tx_arq_drop, link->stats.rx_dup,
		    link->stats.rx_out_of_order, link->stats.rx_frag_drop,
		    link->stats.data_drop_bytes);
	shell_print(sh,
		    "        queue=%u window=%u seq=%u rx_seq=%u ack=%u recovery=%u"
		    " reasm=%u bad_tag=%u bad_frame=%u",
		    link->queue_count, link->arq_used, link->arq_seq, link->arq_rx_seq,
		    link->arq_ack_pending, link->arq_resync, link->reasm.len,
		    link->stats.rx_bad_tag, link->stats.rx_bad_frame);
	shell_print(sh, "        cadence tlm=1/%u data=1/%u resync=1/%u", link->cfg.tlm_every_n,
		    link->cfg.data_every_n, link->cfg.resync_every_n);
	/* The exact KDF context the link derives from: byte compare of two ends settles the rest */
	{
		uint8_t ctx[ZLRS_PAIR_CTX_LEN];

		zlrs_link_pair_ctx(link, ctx);
		shell_print(sh, "        raw dev=%02x%02x%02x%02x peer=%02x%02x%02x%02x",
			    link->dev_id[0], link->dev_id[1], link->dev_id[2], link->dev_id[3],
			    link->peer_id[0], link->peer_id[1], link->peer_id[2], link->peer_id[3]);
		shell_print(sh,
			    "        ctx=%02x%02x%02x%02x%02x%02x%02x%02x"
			    "%02x%02x%02x%02x%02x%02x%02x%02x",
			    ctx[0], ctx[1], ctx[2], ctx[3], ctx[4], ctx[5], ctx[6], ctx[7], ctx[8],
			    ctx[9], ctx[10], ctx[11], ctx[12], ctx[13], ctx[14], ctx[15]);
	}
}

static int cmd_link(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	print_link(sh, "control", &link_control);
	print_link(sh, "data", &link_data);
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
	shell_print(sh, "bench  frame=%u us (%u..%u) slot=%u/%u/%u/%u rx=slot%u+%u (%u..%u)",
		    bench.frame_us_last, bench.frame_us_min, bench.frame_us_max,
		    bench.slot_us_last[0], bench.slot_us_last[1], bench.slot_us_last[2],
		    bench.slot_us_last[3], bench.rx_slot, bench.rx_offset_us, bench.rx_offset_min,
		    bench.rx_offset_max);
	shell_print(sh,
		    "radio  txn=%u stale_irq=%u(%08x) empty_irq=%u overruns=%u rx_win=%u "
		    "late_starts=%u",
		    radio_diag.transactions, radio_diag.stale_irq, radio_diag.stale_irq_bits,
		    radio_diag.empty_irq, radio_diag.overruns, radio_diag.rx_windows,
		    radio_diag.late_starts);
#endif

	return 0;
}

/*
 * KDF self test: IKM 0x0b*22, zero salt, info 0xf0..0xf9 (independently computed vector).
 * Two ends deriving different keys from the same pairing inputs have different inputs or a broken KDF;
 * this tells those apart without a second device.
 */
static int cmd_kdf(const struct shell *sh, size_t argc, char **argv)
{
	static const uint8_t ikm[22] = {
		0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
		0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
	};
	static const uint8_t info[10] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4,
					 0xf5, 0xf6, 0xf7, 0xf8, 0xf9};
	static const uint8_t want[32] = {
		0xab, 0xba, 0xfb, 0x13, 0xf5, 0xc1, 0xbc, 0x48, 0x9d, 0x42, 0x03,
		0x13, 0x58, 0x17, 0x95, 0x6d, 0xd5, 0x21, 0xb3, 0x9e, 0x3b, 0xd6,
		0x1d, 0x1c, 0xc8, 0x5c, 0xef, 0x88, 0x4d, 0x1f, 0x8e, 0x2e,
	};
	uint8_t out[32];
	uint8_t ikm_fixed[32];
	uint8_t ctx_fixed[16];
	uint8_t pair_out[32];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/* Empty label: the info the KDF builds is then exactly the vector's info */
	zlrs_kdf(ikm, sizeof(ikm), "", info, sizeof(info), out, sizeof(out));
	shell_print(sh, "kdf hkdf-sha256 %s", memcmp(out, want, sizeof(want)) == 0 ? "ok" : "FAIL");
	shell_print(sh, "  got  %02x%02x%02x%02x%02x%02x%02x%02x...", out[0], out[1], out[2],
		    out[3], out[4], out[5], out[6], out[7]);
	shell_print(sh, "  want %02x%02x%02x%02x%02x%02x%02x%02x...", want[0], want[1], want[2],
		    want[3], want[4], want[5], want[6], want[7]);

	/*
	 * Same call the pairing makes, but with fixed inputs: two boards that disagree here disagree
	 * inside the KDF, not in the record they feed it.
	 */
	for (uint8_t i = 0U; i < sizeof(ikm_fixed); i++) {
		ikm_fixed[i] = i;
	}
	for (uint8_t i = 0U; i < sizeof(ctx_fixed); i++) {
		ctx_fixed[i] = (uint8_t)(0x40U + i);
	}
	zlrs_kdf(ikm_fixed, sizeof(ikm_fixed), "zlrs/pair", ctx_fixed, sizeof(ctx_fixed), pair_out,
		 sizeof(pair_out));
	shell_print(sh, "kdf fixed %08x%08x%08x%08x", le32(pair_out), le32(&pair_out[4]),
		    le32(&pair_out[8]), le32(&pair_out[12]));

	return 0;
}

/*
 * Bench: stretch the data link's receive window. A steady-state window is a couple of
 * milliseconds in a multi-millisecond frame, so almost everything depends on the two ends being
 * phased; this takes the phase out of the question and asks whether the receive path works at all
 * (continuous listening, peer transmitting). 0 restores the build's value.
 */
static uint16_t rxwin_default;
static uint16_t acqwin_default;

static int cmd_rxwin(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long ms = 0UL;

	if (argc > 1) {
		ms = strtoul(argv[1], NULL, 0);
		if (ms > 500UL) {
			ms = 500UL;
		}
	}
	if (rxwin_default == 0U) {
		rxwin_default = link_data.cfg.rx_timeout_ms;
		acqwin_default = link_data.cfg.acq_timeout_ms;
	}
	link_data.cfg.rx_timeout_ms = (uint16_t)ms;
	link_data.cfg.acq_timeout_ms = (uint16_t)ms;

	shell_print(sh, "data link window = %lu ms (default %u); `zlrs link` shows wins and hits",
		    ms, rxwin_default);

	return 0;
}

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

/* Bench: ask the log store to mount again and print the error it returns */
static int cmd_logstore(const struct shell *sh, size_t argc, char **argv)
{
	const int rc = zlrs_storage_log_probe();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "log store mount: %d", rc);

	return 0;
}

static int cmd_rate(const struct shell *sh, size_t argc, char **argv)
{
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
	static const char *const names[2] = {"control", "data"};
#endif

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
	for (uint8_t i = 0U; i < 2U; i++) {
		const uint32_t wins = rate_last[i].wins;
		const uint32_t hits = rate_last[i].hits;
		const uint32_t permille = (wins != 0U) ? (hits * 1000U) / wins : 0U;

		shell_print(sh, "%-7s %u wins/s, %u got/s (%u.%u%% of windows), %u tx/s, state %u",
			    names[i], (unsigned int)wins, (unsigned int)hits,
			    (unsigned int)(permille / 10U), (unsigned int)(permille % 10U),
			    (unsigned int)rate_last[i].tx, rate_last[i].state);
	}
#else
	shell_print(sh, "no rate measurements in this image (CONFIG_ZLRS_APP_DEBUG_STATS_LOG=n)");
#endif

	return 0;
}

/* Bench: show the pair key, re-derive it from the record on hand, show it again */
static int cmd_rekey(const struct shell *sh, size_t argc, char **argv)
{
	const uint32_t before = fingerprint(link_control.pair_key, ZLRS_KEY_LEN);
	uint32_t after;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	zlrs_link_rederive(&link_control);
	after = fingerprint(link_control.pair_key, ZLRS_KEY_LEN);
	shell_print(sh, "ctl pair_key %08x -> %08x %s", before, after,
		    (before == after) ? "same" : "CHANGED");

	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "role   : %s", role_tx ? "TX" : "RX");
	if (entropy_degraded) {
		shell_print(sh, "entropy: DEGRADED (self-test failed at boot)");
	}
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
	}
	shell_print(sh, "air-to-port=%u B host_drop=%u B", counters.data_rx,
		    zlrs_link_data_tx_dropped());

	return 0;
}

/*
 * Bench: where the milliseconds in a transaction go. The per-second log line carrying this gets
 * dropped by the console whenever the host reads in bursts, and this is the number the timing
 * work turns on, so it has to be readable on demand.
 */
static int cmd_radio(const struct shell *sh, size_t argc, char **argv)
{
	zlrs_radio_diag_t diag;
	struct zlrs_slot_timer_stats timer;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	zlrs_radio_diag(&diag);
	zlrs_slot_timer_stats(&timer);
	shell_print(sh, "plan frame=%u us slots=%u us data_mtu=%u prepare=%u us",
		    frame_period_us, slot_budget_us, data_mtu, slot_prepare_us);
	shell_print(sh,
		    "TIMER1 boundaries=%u irq_late_max=%u us missed=%u skipped_frames=%u slots=%u",
		    timer.boundaries, timer.irq_late_us_max, timer.missed_boundaries,
		    skipped_frames, missed_slots);
	shell_print(sh,
		    "RAL txn=%u tx_done=%u rx_packets=%u overruns=%u late_starts=%u empty_irq=%u",
		    diag.transactions, diag.tx_done, diag.rx_packets, diag.overruns,
		    diag.late_starts, diag.empty_irq);
	shell_print(sh, "setup last/max=%u/%u us tx=%u us rx=%u us irq=%08x stale=%u",
		    diag.submit_us_last, diag.submit_us_max, diag.tx_us_last, diag.rx_us_last,
		    diag.irq_last, diag.stale_irq);
	shell_print(sh, "stale bits=%08x airtime 255B=%u us guard=%u us", diag.stale_irq_bits,
		    zlrs_radio_airtime_us(&data_mod, 255U), CONFIG_ZLRS_APP_TX_GUARD_US);
	shell_print(sh, "late launch last/max=%u/%u us", diag.launch_late_us_last,
		    diag.launch_late_us_max);
	shell_print(sh, "launch command last/max=%u/%u us FIFO ready lead=%u us prepare=%u us",
		    diag.launch_cmd_us_last, diag.launch_cmd_us_max, diag.preload_lead_us_last,
		    slot_prepare_us);
	shell_print(sh, "launch jitter last/max=%u/%u us TX startup=%u us",
		    diag.launch_jitter_us_last, diag.launch_jitter_us_max, diag.tx_startup_us_last);
	shell_print(sh, "chip errors=%04x packet build last/max=%u/%u us", diag.chip_errors,
		    packet_build_us_last, packet_build_us_max);
	shell_print(sh, "phase error/correction=%d/%d us hits=%u frame=%u us", phase_err_us,
		    phase_last_us, phase_hits, frame_period_us);
	shell_print(sh, "last chip error frequency=%u Hz", diag.chip_error_freq_hz);

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
	shell_print(sh, "confirmed by host: %d", (int)zlrs_mgmt_mode_image_confirmed());
	mcuboot_dump_slot(sh, "slot0", PARTITION_ID(slot0_partition));
	mcuboot_dump_slot(sh, "slot1", PARTITION_ID(slot1_partition));

	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	zlrs_cmds, SHELL_CMD(status, NULL, "role / SKU / region / link state", cmd_status),
	SHELL_CMD(link, NULL, "per-link state, frequency, hop index and key fingerprints",
		  cmd_link),
	SHELL_CMD(kdf, NULL, "run the HKDF-SHA256 test vector", cmd_kdf),
	SHELL_CMD(rekey, NULL, "re-derive the control link keys from its record", cmd_rekey),
	SHELL_CMD(rxwin, " [ms]", "stretch the data link receive window (bench)", cmd_rxwin),
	SHELL_CMD(rate, " [s]", "measure windows/hits/frames per second per link", cmd_rate),
	SHELL_CMD(ports, NULL, "state of both data port endpoints", cmd_ports),
	SHELL_CMD(radio, NULL, "LR2021 timing: preparation, launch and wait", cmd_radio),
	SHELL_CMD(logstore, NULL, "mount the log filesystem and report the result", cmd_logstore),
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
	 * Bring the management port up early: any later fatal step (entropy, region/SKU, PSA) lands in
	 * fault_loop, and that path stays flashable only while the management port is alive.
	 */
	(void)zlrs_mgmt_init();
	TRACE('C');
#endif

#if IS_ENABLED(CONFIG_ZLRS_RP2350_TRNG)
	/*
	 * The entropy source is retried and then reported rather than fatal. Parking the whole
	 * application in fault_loop() over a self-test means the device cannot be reached at all —
	 * no link, no management port, nothing to debug — and the TRNG does come back on its own after a
	 * reset far more often than not. A device that boots and says "entropy is degraded" is
	 * strictly more useful than one that blinks forever.
	 */
	{
		int rc = -1;

		for (uint8_t try = 0U; try < 3U; try++) {
			rc = entropy_selftest();
			if (rc == 0) {
				break;
			}
			k_msleep(20);
		}
		TRACE('E');
		if (rc != 0) {
			entropy_degraded = true;
			LOG_ERR("entropy self-test failed (%d) after 3 tries: running degraded",
				rc);
		}
	}
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
	crypto_init();
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
#if CONFIG_ZLRS_APP_BENCH_RAW_MS > 0
	/* Bench images only: run the raw radio bench before the superframe takes over */
	bench_raw_at_boot();
#endif

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
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
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
		const uint32_t now_ms = k_uptime_get_32();

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
			if ((uint32_t)(now_ms - last_rx_any_ms) > ZLRS_PHASE_SCAN_AFTER_MS) {
				phase_samples = 0U;
				phase_adj_us = ZLRS_PHASE_SCAN_STEP_US;
			}
			zlrs_slot_timer_shift(phase_adj_us);
		}
		phase_adj_us = 0;
	}

	return 0;
}

/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * RF owner: links, air transport, superframe, radio policy and maintenance windows.
 */

#include <zephyr/app_version.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include <string.h>
#include <errno.h>

#include "persist.h"
#include "serial.h"
#include "rf.h"
#include "diagnostics.h"
#include "power.h"
#include "band.h"
#include "platform/slot_timer.h"
#include "platform/watchdog.h"
#include "platform/lr2021.h"

#include "zlrs/link/link.h"
#include "zlrs/region/region.h"

LOG_MODULE_REGISTER(zlrs_rf, CONFIG_APP_LOG_LEVEL);

BUILD_ASSERT(IS_ENABLED(CONFIG_CSPRNG_ENABLED), "RF requires a cryptographic entropy source");
BUILD_ASSERT(!IS_ENABLED(CONFIG_TEST_CSPRNG_GENERATOR), "RF must not use a test CSPRNG");

ZBUS_CHAN_DEFINE(zlrs_rf_chan, zlrs_rf_status_t, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

static bool links_ready;
static bool radio_ready;
static zlrs_lr2021_status_t radio_status;
static bool paused;
static int pause_priority;
static uint32_t paused_at_ms;
static bool pause_had_plan;

/* Two alternating DATA slots per timer frame. */
static uint32_t data_rate_hz;
static uint32_t frame_period_us;
static uint32_t slot_budget_us;
static uint32_t slot_prepare_us = CONFIG_ZLRS_APP_PREPARE_US;
static uint16_t resync_every_n;
static uint16_t data_mtu;
static zlrs_data_band_t data_band = ZLRS_DATA_BAND_24;
static zlrs_data_band_t band_mode;
static zlrs_band_policy_t band_policy;
static uint32_t band_switches;
static bool transition_reserved;
#define ZLRS_DATA_SUBG (data_band == ZLRS_DATA_BAND_SUBG)

/* Copy batch only; the pump drains until its source or the link queue is empty. */
#define SERIAL_PUMP_CHUNK_LEN 256U
#define REPORT_PERIOD_MS      1000U

static zlrs_link_t link_data;
static zlrs_mod_cfg_t data_mod;

static const zlrs_rf_design_t rf_design = {
	.band = DT_PROP(DT_NODELABEL(zlrs_rf), band),
	.low_hz = DT_PROP(DT_NODELABEL(zlrs_rf), low_hz),
	.high_hz = DT_PROP(DT_NODELABEL(zlrs_rf), high_hz),
};

/* Region: picks the band, the power ceiling and the duty accounting */
static const zlrs_region_t *region;
static zlrs_region_id_t region_id;
static zlrs_power_t power;
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
/* Zero means no request; signed dBm values occupy the range after AUTO. */
enum {
	POWER_REQUEST_AUTO = 1,
	POWER_REQUEST_FIXED_OFFSET = 2 - INT8_MIN
};
static atomic_t power_request;
static bool power_fixed;
static int8_t fixed_power_dbm;
#endif
static zlrs_duty_t duty;
/* Identity is read or generated at boot before use.
 * See doc/08-security.md#identity-and-threat-model. */
static uint8_t device_id[ZLRS_DEV_ID_LEN];

/*
 * Role and security profile: runtime settings with a compile-time default (set-role /
 * set-profile, doc/06-firmware.md). Loaded once at boot, before the links are built.
 */
static bool role_leader;
static zlrs_profile_t active_profile;

/* The profile a build starts on, and which ones it may run: P1 is the production profile, P0
 * carries no tag at all so only a debug image may select it, P2 is not implemented yet. */
#define ZLRS_PROFILE_DEFAULT                                                                       \
	(IS_ENABLED(CONFIG_ZLRS_APP_SECURITY_PROFILE_P0) ? ZLRS_PROFILE_P0 : ZLRS_PROFILE_P1)

static uint32_t last_report_ms;
static uint32_t consumed_frame;
/* Link-loss alarm only follows a connection already established. */
static bool ever_connected;

static uint32_t data_rx_bytes;
static uint8_t phase_samples;

/* ---- link callbacks (run on the link thread) ------------------------------ */

static bool on_data(void *ctx, const uint8_t *data, uint16_t len)
{
	(void)ctx;

	if (!zlrs_serial_receive(data, len)) {
		return false;
	}
	data_rx_bytes += len;
	return true;
}

static void on_state(void *ctx, zlrs_link_state_t state)
{
	const char *name = "?";
	(void)ctx;

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
		/* A restarted peer has a new clock phase, even before the scan timeout. */
		phase_samples = 0U;
		break;
	case ZLRS_STATE_CONNECTED:
		name = "connected";
		break;
	default:
		break;
	}

	LOG_INF("data link -> %s", name);
}

/* The selected band alone determines modulation, airtime and duty budget. */
static uint16_t sync_period_for_mod(const zlrs_mod_cfg_t *mod)
{
	if (mod->rate_bps >= 2600000U) {
		return 100U;
	}
	if (mod->rate_bps >= 1300000U) {
		return 150U;
	}
	return mod->rate_bps >= 650000U ? 300U : 600U;
}

static void plan_init(void)
{
	data_mod = zlrs_mod_flrc(
		ZLRS_DATA_SUBG ? zlrs_region_clamp_rate(region, CONFIG_ZLRS_APP_DATA_RATE_BPS)
			       : CONFIG_ZLRS_APP_DATA_RATE_BPS);
	slot_prepare_us =
		ZLRS_DATA_SUBG ? CONFIG_ZLRS_APP_PREPARE_US : CONFIG_ZLRS_APP_DATA_24_PREPARE_US;
	data_mtu = ZLRS_DATA_MAX_LEN - zlrs_profile_tag_len(active_profile);
	slot_budget_us = slot_prepare_us + CONFIG_ZLRS_APP_TX_GUARD_US + ZLRS_LR2021_STARTUP_US +
			 zlrs_lr2021_airtime_us(&data_mod, ZLRS_FRAME_MAX_LEN) + 200U;
	frame_period_us = ZLRS_SLOT_COUNT * slot_budget_us;
	data_rate_hz = 1000000U / frame_period_us;
	if (CONFIG_ZLRS_APP_RESYNC_PERIOD_MS > 0) {
		const uint32_t frames = ((uint32_t)CONFIG_ZLRS_APP_RESYNC_PERIOD_MS * 1000U) /
					(frame_period_us * CONFIG_ZLRS_APP_PACING_SCALE);
		resync_every_n = (uint16_t)MAX(frames, 1U);
	} else {
		resync_every_n = 0U;
	}
	LOG_INF("data %s: mtu=%u frame=%u us rate=%u Hz", ZLRS_DATA_SUBG ? "sub-G" : "2.4G",
		data_mtu, frame_period_us, data_rate_hz);
}

/* ---- link setup ---------------------------------------------------------- */

static bool random_u32(void *ctx, uint32_t *out)
{
	ARG_UNUSED(ctx);
	return sys_csrand_get(out, sizeof(*out)) == 0;
}

static int8_t power_minimum(void)
{
	return ZLRS_DATA_SUBG ? DT_PROP(DT_NODELABEL(zlrs_rf), subg_min_tx_power_dbm)
			      : DT_PROP(DT_NODELABEL(zlrs_rf), ism2400_min_tx_power_dbm);
}

static int8_t power_maximum(void)
{
	return ZLRS_DATA_SUBG ? MIN(DT_PROP(DT_NODELABEL(zlrs_rf), subg_max_tx_power_dbm),
				    region->max_tx_power_dbm)
			      : DT_PROP(DT_NODELABEL(zlrs_rf), ism2400_max_tx_power_dbm);
}

static void power_reset(uint32_t now_ms)
{
	zlrs_power_init(&power, power_maximum(), power_minimum(), now_ms);
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	power_fixed = false;
	atomic_set(&power_request, 0);
#endif
}

static void update_power(uint32_t now_ms)
{
	const zlrs_link_info_t info = zlrs_link_get_info(&link_data);
	const bool ready =
		info.state == ZLRS_STATE_CONNECTED && info.session_valid && !info.radio_acquiring;
	int8_t next;
	if (!ready) {
		next = zlrs_power_recover(&power, now_ms);
		if (info.state == ZLRS_STATE_PAIRING) {
			next = CLAMP(ZLRS_PAIR_POWER_DBM, power.floor_dbm, power.ceiling_dbm);
		}
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
		power_fixed = false;
		atomic_set(&power_request, 0);
#endif
	} else {
		next = zlrs_power_update(&power, info.peer_rssi_dbm, info.peer_lq_percent,
					 info.last_peer_feedback_ms, now_ms);
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
		const int request = atomic_set(&power_request, 0);
		if (power.last_feedback_ms == 0U) {
			power_fixed = false;
		} else if (request != 0) {
			power_fixed = request != POWER_REQUEST_AUTO;
			fixed_power_dbm = CLAMP(request - POWER_REQUEST_FIXED_OFFSET,
						power.floor_dbm, power.ceiling_dbm);
			next = zlrs_power_recover(&power, now_ms);
		}
		if (power_fixed) {
			zlrs_power_recover(&power, now_ms);
			next = fixed_power_dbm;
		}
#endif
		if (!IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)) {
			next = zlrs_power_recover(&power, now_ms);
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
			if (power_fixed) {
				next = fixed_power_dbm;
			}
#endif
		}
	}
	zlrs_link_set_power(&link_data, next);
}

static zlrs_link_cfg_t link_config(void)
{
	zlrs_link_cfg_t cfg = {
		.role = role_leader ? ZLRS_ROLE_LEADER : ZLRS_ROLE_FOLLOWER,
		.domain = ZLRS_DATA_SUBG ? region->domain : ZLRS_DOMAIN_ISM2400,

		.profile = active_profile,
		.hopping = IS_ENABLED(CONFIG_ZLRS_APP_FHSS) ? 1U : 0U,
		.mod = data_mod,
		.tx_power_dbm = power_maximum(),
		.pair_tx_power_dbm = CLAMP(ZLRS_PAIR_POWER_DBM, power_minimum(), power_maximum()),
		.rx_timeout_ms = 2U,
		.acq_timeout_ms = 8U,
		.sync_period_ms = sync_period_for_mod(&data_mod),
		.peer_timeout_ms = 0U,
		.pair_period_ms = 100U,
		.pair_window_ms = 30000U,
		.data_mtu = data_mtu,
		.arq = IS_ENABLED(CONFIG_ZLRS_APP_DATA_ARQ) ? 1U : 0U,
		.feedback_period_ms = 250U,
		.arq_timeout_ms = (uint16_t)MAX(
			10U, DIV_ROUND_UP((uint64_t)frame_period_us * CONFIG_ZLRS_APP_PACING_SCALE,
					  1000U)),

		.resync_every_n = resync_every_n,
		.automatic_band = band_mode == ZLRS_DATA_BAND_AUTO,
	};
	return cfg;
}

static void build_links(void)
{
	const zlrs_link_cfg_t cfg = link_config();
	const zlrs_link_events_t events = {
		.on_data = on_data,
		.on_state = on_state,
	};
	zlrs_link_init(&link_data, &cfg, random_u32, NULL, &events);
	power_reset(zlrs_slot_timer_now_ms());
}

static void load_identity(void)
{
	const uint32_t now_ms = zlrs_slot_timer_now_ms();
	zlrs_link_set_dev_id(&link_data, device_id);
	uint8_t root_key[ZLRS_KEY_LEN];
	if (!zlrs_persist_get_root_key(root_key)) {
		LOG_WRN("no root key: staying off air until one is written");
		return;
	}
	zlrs_link_set_root_key(&link_data, root_key, now_ms);
	memset(root_key, 0, sizeof(root_key));
	zlrs_persist_pairing_t pairing;
	if (zlrs_persist_get_pairing(&pairing)) {
		zlrs_link_set_peer(&link_data, pairing.peer_id, pairing.pair_salt, now_ms);
	}
}

/* ---- superframe ---------------------------------------------------------- */

static void pump_serial(void);

static uint32_t rx_finish_us_last, rx_finish_us_max;
static uint32_t rx_protocol_us_last, rx_protocol_us_max;

/* RF owns protocol completion; the LR2021 adapter only returns hardware facts. */
static bool complete_radio(zlrs_lr2021_result_t *result)
{
	if (!zlrs_lr2021_process_irq(result)) {
		return false;
	}
	const uint32_t protocol_us = zlrs_slot_timer_now_us();
	zlrs_link_complete(&link_data, result->status == 0 ? ZLRS_RADIO_OK : ZLRS_RADIO_FAIL,
			   result->size, result->rssi_dbm, result->snr_db, result->completed_ms);
	if (result->status == 0 && result->size > 0U) {
		const uint32_t finish_us = zlrs_slot_timer_now_us();
		rx_protocol_us_last = finish_us - protocol_us;
		rx_protocol_us_max = MAX(rx_protocol_us_max, rx_protocol_us_last);
		rx_finish_us_last = finish_us - result->done_us;
		rx_finish_us_max = MAX(rx_finish_us_max, rx_finish_us_last);
	}
	return true;
}

/* Wait against real elapsed time. Hardware alarms wake the thread at slot deadlines. */
static void radio_wait_done(uint32_t budget_us, zlrs_lr2021_result_t *result)
{
	const uint32_t deadline = zlrs_slot_timer_now_us() + budget_us;

	while ((int32_t)(deadline - zlrs_slot_timer_now_us()) > 0) {
		if (zlrs_lr2021_irq_pending()) {
			if (complete_radio(result)) {
				return;
			}
		}
		/* Radio IRQ and hardware slot boundary both wake this wait. */
		const int32_t remain = (int32_t)(deadline - zlrs_slot_timer_now_us());

		if (remain > 0) {
			(void)zlrs_lr2021_wait_us((uint32_t)remain);
		}
	}
}

/* Debug measurements of the absolute frame clock, slot work and authenticated RX offsets. */
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
static zlrs_lr2021_diag_t radio_diag;
#endif

/* Leader owns the clock; Follower follows verified IRQ timestamps and scans after loss. */
#define ZLRS_PHASE_DEADBAND_US 100

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
static int32_t phase_adj_us;
static bool phase_adj_set;      /* one correction per frame: several receptions would fight */
static int32_t phase_last_us;   /* what the last correction was, for the bench log */
static int32_t phase_err_us;    /* residual the last correction was computed from */
static uint32_t phase_hits;     /* corrections since the last report */
static uint32_t last_rx_any_ms; /* any verified DATA reception: the scan trigger */

static uint32_t now_us(void)
{
	return zlrs_slot_timer_now_us();
}

static void change_band(zlrs_data_band_t target, bool probe, uint32_t now_ms)
{
	zlrs_slot_timer_cancel_launch();
	zlrs_lr2021_abort();
	const zlrs_data_band_t previous = data_band;
	data_band = target;
	plan_init();
	const zlrs_link_cfg_t cfg = link_config();
	if (!zlrs_link_reconfigure_radio(&link_data, &cfg)) {
		data_band = previous;
		plan_init();
		return;
	}
	zlrs_link_acquire_radio(&link_data, now_ms);
	power_reset(now_ms);
	phase_samples = 0U;
	phase_adj_us = 0;
	last_rx_any_ms = now_ms;
	zlrs_slot_timer_reconfigure(frame_period_us * CONFIG_ZLRS_APP_PACING_SCALE, slot_prepare_us,
				    transition_reserved ? 0U : 12000U);
	transition_reserved = false;
	zlrs_band_policy_changed(&band_policy, target == ZLRS_DATA_BAND_SUBG, probe, now_ms,
				 zlrs_link_get_info(&link_data).last_rx_ms);
	band_switches++;
	LOG_INF("auto band -> %s (%s), switches=%u", ZLRS_DATA_SUBG ? "sub-G" : "2.4G",
		probe ? "probe" : "capture", band_switches);
}

static void update_band(uint32_t now_ms)
{
	if (band_mode != ZLRS_DATA_BAND_AUTO) {
		return;
	}
	const zlrs_link_info_t info = zlrs_link_get_info(&link_data);
	if (info.band_plan.switch_frame != 0U &&
	    info.frame_index >= info.band_plan.switch_frame - 1U) {
		change_band(info.band_plan.target_subg ? ZLRS_DATA_BAND_SUBG : ZLRS_DATA_BAND_24,
			    !info.band_plan.target_subg, now_ms);
		return;
	}
	const zlrs_link_stats_t *const stats = zlrs_link_get_stats(&link_data);
	const zlrs_band_observation_t observation = {
		.now_ms = now_ms,
		.last_rx_ms = info.last_rx_ms,
		.feedback_ms = info.last_peer_feedback_ms,
		.paired = info.paired,
		.pairing = info.state == ZLRS_STATE_PAIRING,
		.connected = info.session_valid && info.state == ZLRS_STATE_CONNECTED,
		.acquiring = info.radio_acquiring,
		.planned = info.band_plan.switch_frame != 0U,
		.rssi_dbm = stats->last_rssi,
		.peer_rssi_dbm = info.peer_rssi_dbm,
		.lq_percent = stats->lq_percent,
		.peer_lq_percent = info.peer_lq_percent,
	};
	const zlrs_band_action_t action = zlrs_band_policy_update(&band_policy, &observation);
	if (action == ZLRS_BAND_SEARCH) {
		change_band(ZLRS_DATA_SUBG ? ZLRS_DATA_BAND_24 : ZLRS_DATA_BAND_SUBG, false,
			    now_ms);
	} else if (action == ZLRS_BAND_PLAN) {
		const uint64_t period = (uint64_t)frame_period_us * CONFIG_ZLRS_APP_PACING_SCALE;
		const uint32_t lead = resync_every_n + DIV_ROUND_UP(2000000U, period);
		if (lead <= UINT16_MAX && info.frame_index < UINT32_MAX - lead) {
			(void)zlrs_link_plan_band(&link_data, !ZLRS_DATA_SUBG,
						  info.frame_index + lead);
		}
	}
}

static void run_slot(uint8_t slot)
{
	/* An idle or early-completed predecessor must not open RX in an earlier slot.
	 * Both packet preparation and RX entry start at this slot's reserved window. */
	struct zlrs_slot_timer_wait timing;
	const uint32_t prepare_at_us = slot_nominal_us - slot_prepare_us;

	/* A completed RX leaves a safe opportunity to build our next TX immediately.
	 * RX entry still waits for its own window; searching/pairing retain that rule. */
	const bool early_tx = zlrs_link_get_state(&link_data) == ZLRS_STATE_CONNECTED &&
			      (role_leader == (slot == ZLRS_SLOT_DATA_A));

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

	const uint8_t link_slot = slot;
	zlrs_link_t *link = &link_data;
	const uint32_t slot_start_us = now_us();
	/* Slot decisions use the planned time even though construction happens before it. */
	const uint32_t now_ms = zlrs_slot_timer_uptime_ms(slot_nominal_us);
	const uint32_t hit_before = zlrs_link_get_stats(link)->slot_hit[link_slot];
	const uint32_t good_before = zlrs_link_get_info(link).accepted_rx_events;
	const bool on_subg = ZLRS_DATA_SUBG;
	const zlrs_radio_req_t *req;

	/* Disabled slots stay idle; acquisition outside assigned slots corrupts phase/hop alignment. */
	if ((int32_t)(slot_deadline_us - now_us()) <= 100) {
		missed_slots++;
		zlrs_link_skip_slot(link, link_slot);
		return;
	}
	const uint32_t pump_at_us = now_us();

	pump_serial();
	const uint32_t protocol_at_us = now_us();

	update_power(zlrs_slot_timer_now_ms());
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
	zlrs_lr2021_slot(slot_nominal_us + CONFIG_ZLRS_APP_TX_GUARD_US, slot_deadline_us);

	if (req->is_tx) {
		serial_pump_us_last = protocol_at_us - pump_at_us;
		protocol_build_us_last = now_us() - protocol_at_us;
		packet_build_us_last = now_us() - slot_start_us;
		packet_build_us_max = MAX(packet_build_us_max, packet_build_us_last);
	}

	/* Out of duty budget: give the slot up (finish as a radio failure, the link retries next
	 * round). Only constrains the sub-G link. */
	if (req->is_tx && on_subg &&
	    !zlrs_duty_reserve(&duty, (zlrs_lr2021_airtime_us(&req->mod, req->size) + 999U) / 1000U,
			       now_ms)) {
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, now_ms);
		return;
	}

	const uint32_t prepare_started_us = now_us();

	if (zlrs_lr2021_prepare(req) != 0) {
		if (req->is_tx) {
			late_prepare_entry_us = slot_start_us - prepare_at_us;
			late_prepare_build_us = prepare_started_us - slot_start_us;
			late_prepare_spi_us = now_us() - prepare_started_us;
		}
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, zlrs_slot_timer_now_ms());
		return;
	}

	/* Modem, authentication and FIFO prefix are ready. The timer issues SET_TX;
	 * the owner then appends a fallback FLRC packet's tail while it is on air. */
	const int launch_rc =
		req->is_tx ? zlrs_lr2021_launch_at(slot_nominal_us + CONFIG_ZLRS_APP_TX_GUARD_US)
			   : zlrs_lr2021_launch();

	if (launch_rc != 0) {
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, zlrs_slot_timer_now_ms());
		return;
	}

	if (!req->is_tx && zlrs_link_get_state(link) == ZLRS_STATE_CONNECTED) {
		uint16_t size;
		const uint32_t next_us = slot_nominal_us + slot_budget_us;
		const uint32_t next_ms = zlrs_slot_timer_uptime_ms(next_us);

		pump_serial();
		const uint8_t *buf = zlrs_link_prepare_data(
			link, role_leader ? ZLRS_SLOT_DATA_A : ZLRS_SLOT_DATA_B, next_ms, &size);

		if (buf != NULL) {
			(void)zlrs_lr2021_preload_tx(buf, size);
		}
	}
	const int32_t remaining_us = (int32_t)(slot_deadline_us - now_us());

	zlrs_lr2021_result_t result = {0};
	if (remaining_us > 0) {
		radio_wait_done((uint32_t)remaining_us, &result);
	}

	(void)complete_radio(&result);

	/* If completion did not close the request, abort it before the next slot. */
	if (zlrs_link_is_busy(link)) {
		zlrs_lr2021_abort();
		zlrs_link_complete(link, req->is_tx ? ZLRS_RADIO_FAIL : ZLRS_RADIO_OK, 0U, 0, 0,
				   zlrs_slot_timer_now_ms());
	}

	if (zlrs_link_get_stats(link)->slot_hit[link_slot] != hit_before &&
	    zlrs_link_get_info(link).accepted_rx_events != good_before) {
		last_rx_any_ms = zlrs_slot_timer_now_ms();
		/* Only Follower follows verified packets timestamped in the GPIO ISR. */
		if (!role_leader && !phase_adj_set && result.status == 0 && result.size > 0U) {
			/* Include SPI command transfer and synthesizer/PA startup in the phase origin. */
			const uint32_t expected = slot_nominal_us + CONFIG_ZLRS_APP_TX_GUARD_US +
						  ZLRS_LR2021_PHASE_STARTUP_US;
			const int32_t err =
				(int32_t)(result.done_us - result.airtime_us - expected);
			const int32_t limit = (int32_t)frame_period_us / 16;

			/* Once acquired, discard delayed IRQ outliers rather than move the clock. */
			if (phase_samples >= 8U && !zlrs_link_get_info(link).radio_acquiring &&
			    (err > limit || err < -limit)) {
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
			if (err <= ZLRS_PHASE_DEADBAND_US && err >= -ZLRS_PHASE_DEADBAND_US) {
				(void)zlrs_link_radio_captured(link);
			}
		}
	}
}

static void pump_serial(void)
{
	uint8_t chunk[SERIAL_PUMP_CHUNK_LEN];
	uint16_t got;
	uint16_t room;

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

		got = zlrs_serial_read(chunk, room);
		if (got == 0U) {
			return;
		}

		/* room was measured immediately above in this single-owner thread. */
		if (zlrs_link_write(&link_data, chunk, got) != got) {
			return;
		}
	}
}

bool zlrs_rf_pairing(uint8_t peer_id[ZLRS_DEV_ID_LEN], uint8_t pair_salt[ZLRS_PAIR_SALT_LEN])
{
	return links_ready && zlrs_link_get_pairing(&link_data, peer_id, pair_salt);
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

	s.wins = zlrs_link_get_stats(link)->slot_rx[0] + zlrs_link_get_stats(link)->slot_rx[1];
	s.hits = zlrs_link_get_stats(link)->slot_hit[0] + zlrs_link_get_stats(link)->slot_hit[1];
	s.tx = zlrs_link_get_stats(link)->tx_frames;

	return s;
}

/*
 * Most recent one-second window, kept up to date by report().
 */
static zlrs_rf_rate_sample_t rate_last;

static void rate_update(const zlrs_link_t *link, const rate_snap_t *before, uint32_t span_ms)
{
	const rate_snap_t now = rate_snapshot(link);
	const uint32_t wins = now.wins - before->wins;
	const uint32_t hits = now.hits - before->hits;
	const uint32_t tx = now.tx - before->tx;
	const uint32_t span = (span_ms != 0U) ? span_ms : 1U;

	rate_last.wins = wins;
	rate_last.hits = hits;
	rate_last.tx = tx;
	rate_last.span_ms = span;
	rate_last.state = zlrs_link_get_state(link);
}
#endif

static void report(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	static rate_snap_t rate_before;
	static uint32_t rate_ms;
#endif
	if ((uint32_t)(now_ms - last_report_ms) < REPORT_PERIOD_MS) {
		return;
	}
	last_report_ms = now_ms;
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	rate_update(&link_data, &rate_before, rate_ms != 0U ? now_ms - rate_ms : REPORT_PERIOD_MS);
	rate_before = rate_snapshot(&link_data);
	rate_ms = now_ms;
	phase_hits = 0U;
#endif
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
	const zlrs_link_stats_t *stats = zlrs_link_get_stats(&link_data);
	zlrs_lr2021_diag(&radio_diag);
	LOG_INF("data: bytes=%u air_tx=%u air_rx=%u failed=%u drop=%u lq=%u%%", data_rx_bytes,
		stats->tx_frames, stats->rx_frames, stats->radio_fail,
		stats->data_drop_bytes + zlrs_serial_dropped(), stats->lq_percent);
	LOG_INF("radio: transactions=%u late=%u phase=%d/%d duty_denied=%u",
		radio_diag.transactions, radio_diag.late_starts, phase_err_us, phase_last_us,
		duty.denied);
#endif
}

/* RF facts for UI and diagnostics, including failed initialization. */
void zlrs_rf_publish_state(void)
{
	if (zlrs_link_get_state(&link_data) == ZLRS_STATE_CONNECTED) {
		ever_connected = true;
	}
	const zlrs_rf_status_t state = zlrs_rf_status();
	static zlrs_rf_status_t published;
	static bool have_state;
	static uint32_t published_ms;
	const bool same =
		published.role == state.role && published.profile == state.profile &&
		published.data_band == state.data_band && published.band_mode == state.band_mode &&
		published.band_acquiring == state.band_acquiring &&
		published.band_planned == state.band_planned &&
		published.band_switches == state.band_switches &&
		published.region == state.region && published.state == state.state &&
		published.configured == state.configured && published.paired == state.paired &&
		published.session_valid == state.session_valid &&
		published.paused == state.paused && published.link_lost == state.link_lost &&
		published.fault == state.fault &&
		published.data_drop_bytes == state.data_drop_bytes &&
		published.lq_percent == state.lq_percent &&
		published.tx_power_dbm == state.tx_power_dbm &&
		published.power_fixed == state.power_fixed &&
		published.rssi_dbm == state.rssi_dbm && published.snr_db == state.snr_db &&
		published.frame_period_us == state.frame_period_us &&
		published.radio.sampled_ms == state.radio.sampled_ms &&
		memcmp(published.dev_id, state.dev_id, sizeof(state.dev_id)) == 0 &&
		memcmp(published.peer_id, state.peer_id, sizeof(state.peer_id)) == 0;
	/* Keep changing packet/hop counters readable without waking UI for every packet. */
	const uint32_t now_ms = k_uptime_get_32();
	const bool metrics_due = (uint32_t)(now_ms - published_ms) >= REPORT_PERIOD_MS;
	if ((!have_state || !same || metrics_due) &&
	    zbus_chan_pub(&zlrs_rf_chan, &state, K_NO_WAIT) == 0) {
		published = state;
		have_state = true;
		published_ms = now_ms;
	}
}

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
void zlrs_rf_diagnostics_capture(zlrs_rf_diagnostics_t *out)
{
	out->link = (zlrs_rf_link_snapshot_t){
		.config = *zlrs_link_get_config(&link_data),
		.info = zlrs_link_get_info(&link_data),
		.stats = *zlrs_link_get_stats(&link_data),
		.diagnostics = zlrs_link_get_diagnostics(&link_data),
	};
	out->rate = rate_last;
	out->data_mod = data_mod;
	out->region = region;
	out->frame_period_us = frame_period_us;
	out->slot_budget_us = slot_budget_us;
	out->slot_prepare_us = slot_prepare_us;
	out->data_mtu = data_mtu;
	out->skipped_frames = skipped_frames;
	out->missed_slots = missed_slots;
	out->prepare_wait_skipped = prepare_wait_skipped;
	out->prepare_wait_late_us_max = prepare_wait_late_us_max;
	out->prepare_irq_late_us_max = prepare_irq_late_us_max;
	out->prepare_wake_us_max = prepare_wake_us_max;
	out->phase_err_us = phase_err_us;
	out->phase_last_us = phase_last_us;
	out->phase_hits = phase_hits;
	out->late_prepare_entry_us = late_prepare_entry_us;
	out->late_prepare_build_us = late_prepare_build_us;
	out->late_prepare_spi_us = late_prepare_spi_us;
	out->serial_pump_us_last = serial_pump_us_last;
	out->protocol_build_us_last = protocol_build_us_last;
	out->packet_build_us_last = packet_build_us_last;
	out->packet_build_us_max = packet_build_us_max;
	out->rx_finish_us_last = rx_finish_us_last;
	out->rx_finish_us_max = rx_finish_us_max;
	out->rx_protocol_us_last = rx_protocol_us_last;
	out->rx_protocol_us_max = rx_protocol_us_max;
	out->power_fixed = power_fixed;
	out->power = power;
}
#endif

void zlrs_rf_log_snapshot(void)
{
	/* Capture production RF facts only at a safe boundary, before log persistence. */
	const zlrs_link_stats_t *stats = zlrs_link_get_stats(&link_data);
	const zlrs_link_diagnostics_t data = zlrs_link_get_diagnostics(&link_data);
	LOG_INF("data snapshot: state=%u tx=%u rx=%u fail=%u tag=%u sync=%u/%u resp=%u/%u "
		"cfm=%u/%u queue=%u arq=%u frame=%u last_rx=%u pending=%u",
		zlrs_link_get_state(&link_data), stats->tx_frames, stats->rx_frames,
		stats->radio_fail, stats->rx_bad_tag, stats->tx_syncs, stats->rx_syncs,
		stats->sess_resp_tx, stats->sess_resp_rx, stats->sess_cfm_tx, stats->sess_cfm_rx,
		data.queue_count, data.arq_used, data.frame_index, data.last_rx_frame,
		data.sess_cfm_due ? 1U : 0U);
	LOG_INF("data progress: hop=%u fail_a=%u fail_b=%u dup=%u ooo=%u retry=%u "
		"seq=%u/%u nonces=%08x/%08x pair=%08x",
		data.hop_index, stats->slot_fail[ZLRS_SLOT_DATA_A],
		stats->slot_fail[ZLRS_SLOT_DATA_B], stats->rx_dup, stats->rx_out_of_order,
		stats->tx_retries, data.arq_seq, data.arq_rx_seq, data.sess_nonce_leader,
		data.sess_nonce_follower, data.pair_fingerprint);
	zlrs_lr2021_diag_t radio_state;
	zlrs_lr2021_diag(&radio_state);
	LOG_INF("radio snapshot: late=%u setup=%u errors=%04x error_freq=%u phase=%d/%d",
		radio_state.late_starts, radio_state.submit_us_max, radio_state.chip_errors,
		radio_state.chip_error_freq_hz, phase_err_us, phase_last_us);
	LOG_INF("radio receive: hdr=%u crc=%u len=%u payload=%u expired=%u restart=%u",
		radio_state.rx_header_errors, radio_state.rx_crc_errors,
		radio_state.rx_length_errors, radio_state.rx_payload_errors, radio_state.rx_expired,
		radio_state.rx_restarted);
	LOG_INF("radio transmit: tails=%u errors=%u underflows=%u ready_max=%u",
		radio_state.tx_fifo_tails, radio_state.tx_fifo_errors,
		radio_state.tx_fifo_underflows, radio_state.tx_fifo_ready_us_max);
	zlrs_serial_state_t endpoints[2];
	const size_t count = zlrs_serial_state(endpoints, ARRAY_SIZE(endpoints));
	for (size_t i = 0U; i < count; i++) {
		LOG_INF("data endpoint: %s active=%u input=%u rx_queue=%u tx_queue=%u drop=%u",
			endpoints[i].name, endpoints[i].active ? 1U : 0U, endpoints[i].rx_total,
			endpoints[i].rx_queued, endpoints[i].tx_queued, endpoints[i].rx_dropped);
	}
}

zlrs_rf_status_t zlrs_rf_status(void)
{
	const zlrs_link_info_t info = zlrs_link_get_info(&link_data);
	const zlrs_link_stats_t *stats = zlrs_link_get_stats(&link_data);
	const zlrs_link_diagnostics_t diagnostics = zlrs_link_get_diagnostics(&link_data);
	zlrs_lr2021_diag_t radio;
	zlrs_lr2021_diag(&radio);
	zlrs_rf_status_t out = {
		.role = role_leader ? ZLRS_ROLE_LEADER : ZLRS_ROLE_FOLLOWER,
		.profile = active_profile,
		.data_band = data_band,
		.band_mode = band_mode,
		.band_acquiring = info.radio_acquiring,
		.band_planned = info.band_plan.switch_frame != 0U,
		.band_switches = band_switches,
		.region = region_id,
		.state = info.state,
		.configured = info.configured,
		.paired = info.paired,
		.session_valid = info.session_valid,
		.paused = paused,
		.link_lost = ever_connected &&
			     (info.state != ZLRS_STATE_CONNECTED || info.radio_acquiring),
		.fault = !radio_ready,
		.data_drop_bytes = stats->data_drop_bytes,
		.lq_percent = stats->lq_percent,
		.tx_power_dbm = links_ready ? zlrs_link_get_config(&link_data)->tx_power_dbm
					    : power.ceiling_dbm,
		.rssi_dbm = stats->last_rssi,
		.snr_db = stats->last_snr,
		.frame_period_us = frame_period_us,
		.peer_rssi_dbm = info.peer_rssi_dbm,
		.peer_lq_percent = info.peer_lq_percent,
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
		.power_fixed = power_fixed,
#endif
		.power_floor_dbm = power.floor_dbm,
		.power_ceiling_dbm = power.ceiling_dbm,
		.tx_frames = stats->tx_frames,
		.rx_frames = stats->rx_frames,
		.rx_crc_errors = radio.rx_crc_errors,
		.rx_bad_tag = stats->rx_bad_tag,
		.tx_retries = stats->tx_retries,
		.missed_slots = missed_slots,
		.radio_fail = stats->radio_fail,
		.frequency_hz = diagnostics.frequency_hz,
		.hop_index = diagnostics.hop_index,
		.radio = radio_status,
	};
	memcpy(out.dev_id, device_id, sizeof(out.dev_id));
	memcpy(out.peer_id, info.peer_id, sizeof(out.peer_id));
	return out;
}

void zlrs_rf_status_refresh(void)
{
	__ASSERT_NO_MSG(paused);
	if (radio_ready) {
		zlrs_lr2021_read_status(&radio_status);
	}
	zlrs_rf_publish_state();
}

int zlrs_rf_pair_start(void)
{
	zlrs_link_t *link = &link_data;
	if (!links_ready || !zlrs_link_get_info(link).configured) {
		return -ENOTCONN;
	}
	if (zlrs_link_get_state(link) != ZLRS_STATE_PAIRING) {
		if (band_mode == ZLRS_DATA_BAND_AUTO && ZLRS_DATA_SUBG) {
			change_band(ZLRS_DATA_BAND_24, false, zlrs_slot_timer_now_ms());
		}
		zlrs_link_press(link, zlrs_slot_timer_now_ms());
	}
	update_power(zlrs_slot_timer_now_ms());
	return 0;
}

int zlrs_rf_pair_cancel(void)
{
	zlrs_link_t *link = &link_data;
	if (!links_ready) {
		return -ENOTCONN;
	}
	if (zlrs_link_get_state(link) == ZLRS_STATE_PAIRING) {
		(void)zlrs_link_cancel_pair(link, zlrs_slot_timer_now_ms());
	}
	update_power(zlrs_slot_timer_now_ms());
	return 0;
}

void zlrs_rf_credentials(void)
{
	if (!zlrs_persist_get_device_id(device_id)) {
		memset(device_id, 0, sizeof(device_id));
	}
	if (links_ready) {
		build_links();
		load_identity();
		ever_connected = false;
	}
}

void zlrs_rf_pause(void)
{
	__ASSERT_NO_MSG(!paused);
	paused = true;
	paused_at_ms = zlrs_slot_timer_now_ms();
	pause_had_plan = links_ready && zlrs_link_get_info(&link_data).band_plan.switch_frame != 0U;
	pause_priority = k_thread_priority_get(k_current_get());
	k_thread_priority_set(k_current_get(), CONFIG_MAIN_THREAD_PRIORITY);
	if (radio_ready) {
		zlrs_slot_timer_cancel_launch();
		zlrs_lr2021_abort();
	}
	if (links_ready) {
		zlrs_link_discard_prepared_data(&link_data);
	}
}

void zlrs_rf_resume(void)
{
	__ASSERT_NO_MSG(paused);
	paused = false;
	k_thread_priority_set(k_current_get(), pause_priority);
	if (links_ready && band_mode == ZLRS_DATA_BAND_AUTO) {
		const uint32_t now_ms = zlrs_slot_timer_now_ms();
		if (pause_had_plan || (uint32_t)(now_ms - paused_at_ms) >= band_policy.loss_ms) {
			zlrs_link_acquire_radio(&link_data, now_ms);
			phase_samples = 0U;
			zlrs_band_policy_resume(&band_policy, now_ms,
						zlrs_link_get_info(&link_data).last_rx_ms);
		}
	}
	if (links_ready) {
		update_power(zlrs_slot_timer_now_ms());
	}
}

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
int zlrs_rf_power_request(bool automatic, int dbm)
{
	zlrs_rf_status_t status;
	if (zbus_chan_read(&zlrs_rf_chan, &status, K_MSEC(10)) != 0) {
		return -EAGAIN;
	}
	if (!automatic && (dbm < status.power_floor_dbm || dbm > status.power_ceiling_dbm)) {
		return -EINVAL;
	}
	if (!automatic && (status.state != ZLRS_STATE_CONNECTED || !status.session_valid ||
			   status.band_acquiring)) {
		return -ENOTCONN;
	}
	atomic_set(&power_request,
		   automatic ? POWER_REQUEST_AUTO : dbm + POWER_REQUEST_FIXED_OFFSET);
	return 0;
}
#endif

int zlrs_rf_init(void)
{
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

	LOG_INF("ZLRS %s", APP_VERSION_STRING);

	/* Optional ports come directly from the board's devicetree aliases. */
	if (!DT_NODE_EXISTS(DT_ALIAS(zlrs_mgmt_cdc))) {
		LOG_WRN("no management port on this board, parameters can only come from the "
			"defaults");
	}

	/* Identity comes before anything that could use it; without one, stay off the air */
	if (!zlrs_persist_get_device_id(device_id)) {
		LOG_ERR("no device id: staying off the air until one can be made");
		return -ENODEV;
	}

	/*
	 * Persist loads settings or board defaults and validates each frontend's requested
	 * region. RF owns the parameters adopted for this boot.
	 */
	const zlrs_persist_settings_t *const settings = zlrs_persist_settings();
	band_mode = settings->band;
	data_band = band_mode == ZLRS_DATA_BAND_SUBG ? ZLRS_DATA_BAND_SUBG : ZLRS_DATA_BAND_24;
	region_id = settings->region;
	role_leader = settings->role == ZLRS_ROLE_LEADER;
	active_profile = settings->profile;
	LOG_INF("data band: %s", ZLRS_DATA_SUBG ? "sub-G" : "2.4G");
	region = zlrs_region_get(region_id);
	if (region == NULL) {
		LOG_ERR("unknown region %d", region_id);
		return -EINVAL;
	}
	/* The first two things to look at on the bench: which SKU this board is, which region it runs */
	LOG_INF("board: sub-G %s (band %d), region %s",
		rf_design.band == ZLRS_BAND_SUBG_LF ? "LF" : "HF", rf_design.band, region->name);
	LOG_INF("hardware: ports=%s%s usb=%c btn=%c led=%c wdt=%c rf=%s(%u-%u MHz)",
		DT_NODE_EXISTS(DT_ALIAS(zlrs_data_uart)) || DT_NODE_EXISTS(DT_ALIAS(zlrs_data_cdc))
			? "data"
			: "",
		DT_NODE_EXISTS(DT_ALIAS(zlrs_mgmt_cdc)) ? ",cfg" : "",
		IS_ENABLED(CONFIG_ZLRS_USB) ? 'y' : 'n',
		DT_NODE_EXISTS(DT_ALIAS(zlrs_button)) ? 'y' : 'n',
		DT_NODE_EXISTS(DT_ALIAS(zlrs_led)) ? 'y' : 'n',
		DT_NODE_EXISTS(DT_ALIAS(watchdog0)) ? 'y' : 'n',
		rf_design.band == ZLRS_BAND_SUBG_LF ? "LF" : "HF", rf_design.low_hz / 1000000U,
		rf_design.high_hz / 1000000U);

	LOG_INF("role %s, profile P%u", role_leader ? "Leader" : "Follower",
		(unsigned int)active_profile);

	/* Bench switches: with these off the link is no longer production-shaped, so say so at boot */
	if (active_profile != ZLRS_PROFILE_P1) {
		LOG_WRN("isolation: security profile P0, frames carry no tag");
	}
	if (!IS_ENABLED(CONFIG_ZLRS_APP_FHSS)) {
		LOG_WRN("isolation: hopping off, every frame stays on the sync channel");
	}
	if (zlrs_region_validate(region, &rf_design, region->max_tx_power_dbm) != 0 ||
	    power_minimum() > power_maximum()) {
		LOG_ERR("region %s is incompatible with radio design", region->name);
		return -EINVAL;
	}
	power_reset(zlrs_slot_timer_now_ms());
	zlrs_duty_init(&duty, region, 0U);
	zlrs_band_policy_init(&band_policy, band_mode == ZLRS_DATA_BAND_AUTO, role_leader,
			      ZLRS_DATA_SUBG, region->duty_window_ms, zlrs_slot_timer_now_ms());
	band_switches = 0U;
	/* The status LED comes up on its own thread and reads the state it renders: nothing to
	 * init here (app/src/ui.c, rf.h) */

	int rc = zlrs_slot_timer_init();
	if (rc != 0) {
		LOG_ERR("hardware slot timer init failed (%d)", rc);
		return rc;
	}

	plan_init();
	build_links();
	links_ready = true;
	load_identity();

	rc = zlrs_lr2021_init();
	if (rc != 0) {
		LOG_ERR("radio driver init failed (%d)", rc);
		return rc;
	}

	radio_ready = true;
	/* Initial snapshot before the timer starts; later refreshes use management. */
	zlrs_lr2021_read_status(&radio_status);
	return 0;
}

void zlrs_rf_start(void)
{
	LOG_INF("entering data loop: %u Hz (%s)", (unsigned int)data_rate_hz,
		role_leader ? "Leader" : "Follower");
	zlrs_slot_timer_start(frame_period_us * CONFIG_ZLRS_APP_PACING_SCALE, slot_prepare_us,
			      zlrs_lr2021_wake);
	consumed_frame = 0U;
	BUILD_ASSERT(CONFIG_NUM_METAIRQ_PRIORITIES >= 1);
	k_thread_priority_set(k_current_get(), K_PRIO_COOP(0));
}

void zlrs_rf_frame(void)
{
	uint32_t start_us;
	uint32_t timer_frame;

	do {
		zlrs_slot_timer_wait_frame(&start_us, &timer_frame);
	} while ((int32_t)(timer_frame - consumed_frame) <= 0);

	/* Frames elapsed during maintenance still advance the original hop cadence. */
	while (++consumed_frame < timer_frame) {
		skipped_frames++;
		for (uint8_t slot = 0U; slot < ZLRS_SLOT_COUNT; slot++) {
			zlrs_link_skip_slot(&link_data, slot);
		}
	}
	phase_adj_set = false;
	const uint32_t period = frame_period_us * CONFIG_ZLRS_APP_PACING_SCALE;
	transition_reserved = false;

	for (uint8_t slot = 0U; slot < ZLRS_SLOT_COUNT; slot++) {
		const zlrs_link_info_t info = zlrs_link_get_info(&link_data);
		if (band_mode == ZLRS_DATA_BAND_AUTO && !transition_reserved &&
		    info.band_plan.switch_frame != 0U &&
		    info.band_plan.switch_frame == info.frame_index + (slot == 0U ? 2U : 1U)) {
			zlrs_slot_timer_hold_next_frame(12000U);
			transition_reserved = true;
		}
		zlrs_watchdog_feed();
		slot_nominal_us = start_us + ((uint64_t)period * slot) / ZLRS_SLOT_COUNT;
		const uint32_t next_us =
			slot == ZLRS_SLOT_DATA_B
				? (transition_reserved ? start_us + period
						       : zlrs_slot_timer_next_frame_us())
				: start_us + ((uint64_t)period * (slot + 1U)) / ZLRS_SLOT_COUNT;
		slot_deadline_us = next_us - slot_prepare_us - 100U;
		run_slot(slot);
	}

	/* Re-read time after radio completions and any preceding maintenance window. */
	const uint32_t now_ms = zlrs_slot_timer_now_ms();
	zlrs_link_tick(&link_data, now_ms);
	update_power(now_ms);
	report(now_ms);

	if (!role_leader) {
		const zlrs_link_cfg_t *const cfg = zlrs_link_get_config(&link_data);
		const zlrs_link_info_t info = zlrs_link_get_info(&link_data);
		const bool acquiring = info.radio_acquiring || info.state == ZLRS_STATE_SEARCHING;
		const uint32_t capture_wait_ms =
			info.radio_sync_seen
				? MAX(300U, 2U * cfg->sync_period_ms + DIV_ROUND_UP(period, 1000U))
				: 300U;
		const uint32_t scan_after_ms =
			acquiring ? capture_wait_ms
				  : zlrs_slot_timer_scan_delay_ms(period, cfg->resync_every_n,
								  phase_samples > 0U);
		if ((uint32_t)(now_ms - last_rx_any_ms) > scan_after_ms) {
			phase_samples = 0U;
			const uint16_t sync_frames =
				cfg->resync_every_n > 0U
					? cfg->resync_every_n
					: (uint16_t)MAX(1U,
							DIV_ROUND_UP(cfg->sync_period_ms * 1000U,
								     period));

			phase_adj_us = acquiring ? (int32_t)MAX(1U, period / 32U)
						 : (int32_t)zlrs_slot_timer_scan_step_us(
							   period, sync_frames);
		}
		zlrs_slot_timer_shift(phase_adj_us);
	}
	phase_adj_us = 0;
	update_band(now_ms);
}

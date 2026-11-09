/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * RF owner: links, air transport, superframe, radio policy and maintenance windows.
 */

#include <zephyr/app_version.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <string.h>

#include "band.h"
#include "persist.h"
#include "platform/lr2021.h"
#include "platform/sha256.h"
#include "platform/slot_timer.h"
#include "platform/watchdog.h"
#include "power.h"
#include "rf.h"
#include "serial.h"
#include "trace.h"

#include "link/data.h"
#include "link/handshake.h"
#include "link/link.h"
#include "region/region.h"

BUILD_ASSERT(CONFIG_ZLRS_APP_DATA_RATE_BPS == 2600000 || CONFIG_ZLRS_APP_DATA_RATE_BPS == 1300000 ||
		     CONFIG_ZLRS_APP_DATA_RATE_BPS == 650000 ||
		     CONFIG_ZLRS_APP_DATA_RATE_BPS == 325000,
	     "fixed DATA rate must select a supported FLRC PHY");

LOG_MODULE_REGISTER(zlrs_rf, CONFIG_APP_LOG_LEVEL);

BUILD_ASSERT(IS_ENABLED(CONFIG_CSPRNG_ENABLED), "RF requires a cryptographic entropy source");
BUILD_ASSERT(!IS_ENABLED(CONFIG_TEST_CSPRNG_GENERATOR), "RF must not use a test CSPRNG");
BUILD_ASSERT(IS_ENABLED(CONFIG_HARDWARE_DEVICE_CS_GENERATOR),
	     "RF entropy reads must use the same hardware source as the secure random API");

ZBUS_CHAN_DEFINE(zlrs_rf_chan, zlrs_rf_event_t, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

static K_SEM_DEFINE(owner_wake, 0, 1);
static zlrs_link_t link_data;
static zlrs_rf_state_t facts = {
	.link = &link_data.status,
	.slot_prepare_us = CONFIG_ZLRS_APP_PREPARE_US,
	.temperature_centi_c = INT16_MIN,
};

static void refresh_hardware_status(void)
{
	zlrs_lr2021_read_status(&facts.radio);
	facts.temperature_centi_c = INT16_MIN;
#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(die_temp0)) && IS_ENABLED(CONFIG_SENSOR)
	const struct device *const sensor = DEVICE_DT_GET(DT_ALIAS(die_temp0));
	struct sensor_value value;
	if (device_is_ready(sensor) &&
	    sensor_sample_fetch_chan(sensor, SENSOR_CHAN_DIE_TEMP) == 0 &&
	    sensor_channel_get(sensor, SENSOR_CHAN_DIE_TEMP, &value) == 0) {
		facts.temperature_centi_c = (int16_t)(value.val1 * 100 + value.val2 / 10000);
	}
#endif
	facts.hardware_sampled_ms = k_uptime_get_32();
}

static int pause_priority;
static uint32_t paused_at_ms;
static bool pause_had_plan;

/* Two alternating DATA slots per timer frame. */
static zlrs_band_policy_t band_policy;
static bool transition_reserved;

#define REPORT_PERIOD_MS 1000U

/* Region: picks the band, the power ceiling and the duty accounting */
static zlrs_region_id_t region_id;
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
/* Zero means no request; signed dBm values occupy the range after AUTO. */
enum {
	POWER_REQUEST_AUTO = 1,
	POWER_REQUEST_FIXED_OFFSET = 2 - INT8_MIN
};

static atomic_t power_request;
static atomic_t phy_request;
static int8_t fixed_power_dbm;
static bool maintenance_gap;

#endif
static zlrs_duty_t duty;
/* Identity is read or generated at boot before use.
 * See doc/08-security.md#identity-and-threat-model. */

static uint32_t consumed_frame;
/* Link-loss alarm only follows a connection already established. */

static uint32_t data_rx_bytes;
static uint8_t phase_samples;

static bool is_subg(void)
{
	return facts.links_ready && link_data.status.cfg.region != ZLRS_REGION_ISM2400;
}

/* ---- link callbacks (run on the link thread) ------------------------------ */

static bool on_data(void *ctx, const uint8_t *data, uint16_t len)
{
	(void)ctx;

	ZLRS_TRACE_EVENT("rf_deliver_begin", len, 0);
	const bool accepted = zlrs_serial_receive(data, len);
	ZLRS_TRACE_EVENT("rf_deliver_end", len, accepted);
	if (!accepted) {
		return false;
	}
	data_rx_bytes += len;
	return true;
}

static void on_state(void *ctx, zlrs_link_state_t state)
{
	ARG_UNUSED(ctx);
	if (state == ZLRS_STATE_SEARCHING) {
		/* A restarted peer has a new clock phase. */
		phase_samples = 0U;
	}
	LOG_INF("data link -> %s", zlrs_link_state_name(state));
}

/* The active band and profile determine modulation, airtime and duty budget. */
/* ---- link setup ---------------------------------------------------------- */

static int random_error;

static bool random_u32(void *ctx, uint32_t *out)
{
	ARG_UNUSED(ctx);
	const struct device *const dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
	if (!device_is_ready(dev)) {
		random_error = -ENODEV;
		return false;
	}
	uint32_t nonce;
	const int rc = entropy_get_entropy_isr(dev, (uint8_t *)&nonce, sizeof(nonce), 0U);
	if (rc != sizeof(nonce)) {
		random_error = rc >= 0 || rc == -EAGAIN ? -EAGAIN : rc;
		return false;
	}
	*out = nonce;
	return true;
}

static int8_t power_minimum(bool subg)
{
	return subg ? DT_PROP(DT_NODELABEL(zlrs_rf), subg_min_tx_power_dbm)
		    : DT_PROP(DT_NODELABEL(zlrs_rf), ism2400_min_tx_power_dbm);
}

static int8_t power_maximum(bool subg)
{
	return subg ? MIN(DT_PROP(DT_NODELABEL(zlrs_rf), subg_max_tx_power_dbm),
			  facts.region->max_tx_power_dbm)
		    : DT_PROP(DT_NODELABEL(zlrs_rf), ism2400_max_tx_power_dbm);
}

static void power_reset(uint32_t now_ms)
{
	zlrs_power_init(&facts.power, power_maximum(is_subg()), power_minimum(is_subg()), now_ms);
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	facts.power_fixed = false;
	atomic_set(&power_request, 0);
#endif
}

static void update_power(uint32_t now_ms)
{
	const zlrs_link_status_t *info = &link_data.status;
	const bool ready = info->state == ZLRS_STATE_CONNECTED && info->session_valid &&
			   !info->radio_acquiring;
	int8_t next;
	if (!ready || (link_data.status.cfg.automatic_band && band_policy.power_boosted)) {
		next = zlrs_power_recover(&facts.power, now_ms);
		if (info->state == ZLRS_STATE_PAIRING) {
			next = CLAMP(ZLRS_PAIR_POWER_DBM, facts.power.floor_dbm,
				     facts.power.ceiling_dbm);
		}
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
		facts.power_fixed = false;
		atomic_set(&power_request, 0);
#endif
	} else {
		const uint32_t round_ms = DIV_ROUND_UP(
			2ULL * facts.frame_period_us * CONFIG_ZLRS_APP_PACING_SCALE, 1000U);
		const zlrs_link_observation_t observation = {
			.link = info,
			.now_ms = now_ms,
			.settle_ms = MAX(500U, round_ms),
		};
		next = zlrs_power_update(&facts.power, &observation);
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
		const int request = atomic_set(&power_request, 0);
		if (facts.power.last_feedback_ms == 0U) {
			facts.power_fixed = false;
		} else if (request != 0) {
			facts.power_fixed = request != POWER_REQUEST_AUTO;
			fixed_power_dbm = CLAMP(request - POWER_REQUEST_FIXED_OFFSET,
						facts.power.floor_dbm, facts.power.ceiling_dbm);
			next = zlrs_power_recover(&facts.power, now_ms);
		}
		if (facts.power_fixed) {
			zlrs_power_recover(&facts.power, now_ms);
			next = fixed_power_dbm;
		}
#endif
		if (!IS_ENABLED(CONFIG_ZLRS_APP_DYNAMIC_POWER)) {
			next = zlrs_power_recover(&facts.power, now_ms);
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
			if (facts.power_fixed) {
				next = fixed_power_dbm;
			}
#endif
		}
	}
	link_data.status.cfg.tx_power_dbm = next;
}

struct rf_plan {
	zlrs_link_cfg_t config;
	uint32_t period_us, prepare_us;
};

static void link_config(zlrs_role_t role, zlrs_profile_t profile, bool automatic, bool subg,
			zlrs_phy_t phy, struct rf_plan *out)
{
	const zlrs_phy_t selected_phy =
		!subg && automatic ? (phy)
				   : zlrs_phy_for_rate(subg ? zlrs_region_clamp_rate(
								      facts.region,
								      CONFIG_ZLRS_APP_DATA_RATE_BPS)
							    : CONFIG_ZLRS_APP_DATA_RATE_BPS);
	const uint32_t prepare_us =
		subg ? CONFIG_ZLRS_APP_PREPARE_US : CONFIG_ZLRS_APP_DATA_24_PREPARE_US;
	/* A common MTU preserves outstanding ARQ packets across FLRC/LoRa changes. */
	const uint16_t max_frame = automatic ? ZLRS_LORA_FRAME_MAX_LEN : ZLRS_FRAME_MAX_LEN;
	const uint16_t mtu = max_frame - ZLRS_FRAME_HDR_LEN - ZLRS_DATA_PREFIX_LEN -
			     zlrs_profile_tag_len(profile);
	const uint32_t slot_budget = prepare_us + CONFIG_ZLRS_APP_TX_GUARD_US +
				     ZLRS_LR2021_STARTUP_US +
				     zlrs_phy_airtime_us(selected_phy, max_frame) + 200U;
	const uint32_t period_us = ZLRS_SLOT_COUNT * slot_budget;
	uint16_t resync_every_n;
	if (CONFIG_ZLRS_APP_RESYNC_PERIOD_MS > 0 || automatic) {
		const uint32_t sync_ms =
			automatic ? (CONFIG_ZLRS_APP_RESYNC_PERIOD_MS > 0
					     ? MIN(CONFIG_ZLRS_APP_RESYNC_PERIOD_MS, 1000U)
					     : 1000U)
				  : CONFIG_ZLRS_APP_RESYNC_PERIOD_MS;
		const uint32_t frames =
			(sync_ms * 1000U) / (period_us * CONFIG_ZLRS_APP_PACING_SCALE);
		resync_every_n = (uint16_t)MAX(frames, 1U);
	} else {
		resync_every_n = 0U;
	}
	out->config = (zlrs_link_cfg_t){
		.role = role,
		.region = subg ? region_id : ZLRS_REGION_ISM2400,

		.profile = profile,
		.hopping = IS_ENABLED(CONFIG_ZLRS_APP_FHSS) ? 1U : 0U,
		.phy = selected_phy,
		.tx_power_dbm = power_maximum(subg),
		.pair_tx_power_dbm =
			CLAMP(ZLRS_PAIR_POWER_DBM, power_minimum(subg), power_maximum(subg)),

		.sync_period_ms = zlrs_phys[selected_phy].sync_period_ms,
		.pair_period_ms = 100U,
		.pair_window_ms = 30000U,
		.data_mtu = mtu,
		.arq = IS_ENABLED(CONFIG_ZLRS_APP_DATA_ARQ) ? 1U : 0U,
		.feedback_period_ms = 250U,
		/* Below one minimum window: two unseen toggles cannot be treated as unchanged. */
		.lq_feedback_gap_ms = MAX(
			1U, MIN(1000U, 127ULL * period_us * CONFIG_ZLRS_APP_PACING_SCALE / 1000U)),
		/* Own TX slots already leave a peer ACK opportunity. Round down so
		 * millisecond quantization cannot postpone a retry by another frame. */
		.arq_timeout_ms = (uint16_t)MAX(1U, (uint64_t)period_us *
							    CONFIG_ZLRS_APP_PACING_SCALE / 1000U),

		.resync_every_n = resync_every_n,
		.automatic_band = automatic,
	};
	LOG_INF("data %s: mtu=%u frame=%u us rate=%u Hz", subg ? "sub-G" : "2.4G",
		out->config.data_mtu, period_us, 1000000U / period_us);
	LOG_INF("modem: %s", zlrs_phy_name(out->config.phy));
	out->period_us = period_us;
	out->prepare_us = prepare_us;
}

static void build_links(const zlrs_link_cfg_t *cfg)
{
	const zlrs_link_events_t events = {.on_data = on_data, .on_state = on_state};
	zlrs_link_init(&link_data, cfg, random_u32, NULL, &events);
	facts.links_ready = true;
	power_reset(zlrs_slot_timer_now_ms());
	link_data.status.cfg.tx_power_dbm = facts.power.ceiling_dbm;
	zlrs_link_restore(&link_data, zlrs_persist_state(), zlrs_slot_timer_now_ms());
	if (link_data.status.state == ZLRS_STATE_UNCONFIGURED) {
		LOG_WRN("no root key: staying off air until one is written");
	}
}

/* ---- superframe ---------------------------------------------------------- */

static void pump_serial(void);

/* RF owns protocol completion; the LR2021 adapter only returns hardware facts. */
static bool complete_radio(zlrs_lr2021_result_t *result, uint32_t accept_deadline_us)
{
	ZLRS_TRACE_EVENT("rf_irq_begin", 0, 0);
	const bool completed = zlrs_lr2021_process_irq(result);
	ZLRS_TRACE_EVENT("rf_irq_end", 0, completed);
	if (!completed) {
		return false;
	}
	const uint32_t accept_started = zlrs_slot_timer_now_us();
	const bool received = result->size > 0U;
	ZLRS_TRACE_EVENT("rf_rx_begin", result->done_us, result->size);
	bool hash_held = false;
	if (result->status == 0 && result->size > 0U &&
	    link_data.status.cfg.profile == ZLRS_PROFILE_P1) {
		const int32_t remaining = (int32_t)(accept_deadline_us - zlrs_slot_timer_now_us());
		hash_held = remaining > 0 && zlrs_hash_acquire((uint32_t)remaining);
		if (!hash_held) {
			/* No accepted protocol effects without successful authentication. */
			facts.work.hash_busy_rx++;
			result->size = 0U;
		}
	}
	zlrs_link_complete(&link_data, result->status == 0 ? ZLRS_RADIO_OK : ZLRS_RADIO_FAIL,
			   result->size, result->rssi_dbm, result->snr_db, result->completed_ms);
	if (hash_held) {
		zlrs_hash_release();
	}
	if (received) {
		zlrs_timing_record(&facts.work.rx_accept_us,
				   zlrs_slot_timer_now_us() - accept_started);
	}
	ZLRS_TRACE_EVENT("rf_rx_end", result->done_us, result->size);
	ZLRS_TRACE_EVENT("rf_arq", link_data.status.stats.tx_retries,
			 link_data.status.stats.tx_frames);
	return true;
}

/* Wait against real elapsed time. Hardware alarms wake the thread at slot deadlines. */
static void radio_wait_done(uint32_t budget_us, uint32_t accept_deadline_us,
			    zlrs_lr2021_result_t *result)
{
	const uint32_t deadline = zlrs_slot_timer_now_us() + budget_us;

	while ((int32_t)(deadline - zlrs_slot_timer_now_us()) > 0) {
		if (zlrs_lr2021_irq_pending()) {
			if (complete_radio(result, accept_deadline_us)) {
				return;
			}
		}
		/* Radio IRQ and hardware slot boundary both wake this wait. */
		const int32_t remain = (int32_t)(deadline - zlrs_slot_timer_now_us());

		if (remain > 0) {
			(void)k_sem_take(&owner_wake, K_USEC(remain));
		}
	}
}

/* Debug measurements of the absolute frame clock, slot work and authenticated RX offsets. */
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)

#endif

/* Leader owns the clock; Follower follows verified IRQ timestamps and scans after loss. */
#define ZLRS_PHASE_DEADBAND_US 100

static uint32_t slot_deadline_us;
static uint32_t slot_nominal_us;
static int32_t phase_adj_us;
static bool phase_adj_set;      /* one correction per frame: several receptions would fight */
static uint32_t last_rx_any_ms; /* any verified DATA reception: the scan trigger */

static void change_band(zlrs_data_band_t target, zlrs_phy_t phy, bool probe, uint32_t now_ms)
{
	zlrs_lr2021_abort();
	const zlrs_link_cfg_t *current = &link_data.status.cfg;
	struct rf_plan plan;
	link_config(current->role, current->profile, current->automatic_band,
		    target == ZLRS_DATA_BAND_SUBG, phy, &plan);
	if (!zlrs_link_reconfigure_radio(&link_data, &plan.config, now_ms)) {
		return;
	}
	facts.frame_period_us = plan.period_us;
	facts.slot_prepare_us = plan.prepare_us;
	power_reset(now_ms);
	phase_samples = 0U;
	phase_adj_us = 0;
	last_rx_any_ms = now_ms;
	zlrs_slot_timer_reconfigure(facts.frame_period_us * CONFIG_ZLRS_APP_PACING_SCALE,
				    facts.slot_prepare_us, transition_reserved ? 0U : 12000U);
	transition_reserved = false;
	const zlrs_link_status_t *info = &link_data.status;
	zlrs_band_policy_reset(&band_policy, probe, now_ms, info->last_rx_ms);
	facts.band_switches++;
	LOG_INF("auto band -> %s (%s), switches=%u", is_subg() ? "sub-G" : "2.4G",
		probe ? "probe" : "capture", facts.band_switches);
}

static bool plan_radio(bool subg, zlrs_phy_t phy, uint32_t frame_index)
{
	const uint64_t period = (uint64_t)facts.frame_period_us * CONFIG_ZLRS_APP_PACING_SCALE;
	const uint32_t lead = link_data.status.cfg.resync_every_n + DIV_ROUND_UP(500000U, period);
	return lead <= UINT16_MAX && frame_index < UINT32_MAX - lead &&
	       zlrs_link_plan_radio(&link_data, subg, phy, frame_index + lead);
}

static void update_band(uint32_t now_ms)
{
	const zlrs_link_status_t *info = &link_data.status;
	if (!info->cfg.automatic_band) {
		return;
	}
	if (info->band_plan.switch_frame != 0U &&
	    info->frame_index >= info->band_plan.switch_frame - 1U) {
		change_band(info->band_plan.target_subg ? ZLRS_DATA_BAND_SUBG : ZLRS_DATA_BAND_24,
			    info->band_plan.target_phy,
			    !info->band_plan.target_subg &&
				    (is_subg() || info->band_plan.target_phy < info->cfg.phy),
			    now_ms);
		return;
	}
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	const atomic_val_t requested = atomic_get(&phy_request);
	if (requested != 0 && info->session_valid && info->state == ZLRS_STATE_CONNECTED &&
	    !info->radio_acquiring && info->band_plan.switch_frame == 0U) {
		const zlrs_phy_t phy = requested - 1;
		if ((!is_subg() && phy == info->cfg.phy) ||
		    plan_radio(false, phy, info->frame_index)) {
			(void)atomic_cas(&phy_request, requested, 0);
		}
		return;
	}
#endif
	const zlrs_link_observation_t observation = {
		.link = info,
		.now_ms = now_ms,
		.settle_ms = MAX(500U, DIV_ROUND_UP(2ULL * facts.frame_period_us *
							    CONFIG_ZLRS_APP_PACING_SCALE,
						    1000U)),
	};
	const zlrs_band_action_t action =
		zlrs_band_policy_update(&band_policy, &observation, &facts.power);
	if (action == ZLRS_BAND_BOOST) {
		/* Apply before the next slot, including when poor feedback arrived on Follower. */
		update_power(now_ms);
	} else if (action == ZLRS_BAND_SEARCH) {
		change_band(band_policy.target_subg ? ZLRS_DATA_BAND_SUBG : ZLRS_DATA_BAND_24,
			    band_policy.target_phy, false, now_ms);
	} else if (action == ZLRS_BAND_PLAN) {
		(void)plan_radio(band_policy.target_subg, band_policy.target_phy,
				 info->frame_index);
	}
}

static void run_slot(uint8_t slot, uint32_t next_slot_us)
{
	/* An idle or early-completed predecessor must not open RX in an earlier slot.
	 * Both packet preparation and RX entry start at this slot's reserved window. */
	const zlrs_link_status_t *info = &link_data.status;
	struct zlrs_slot_timer_wait timing;
	const uint32_t prepare_at_us = slot_nominal_us - facts.slot_prepare_us;

	/* A completed RX leaves a safe opportunity to build our next TX immediately.
	 * RX entry still waits for its own window; searching/pairing retain that rule. */
	const bool early_tx = link_data.status.state == ZLRS_STATE_CONNECTED &&
			      ((info->cfg.role == ZLRS_ROLE_LEADER) == (slot == ZLRS_SLOT_DATA_A));

	if (early_tx) {
		timing = (struct zlrs_slot_timer_wait){.wake_us = zlrs_slot_timer_now_us()};
	} else {
		zlrs_slot_timer_wait_until(prepare_at_us, &timing);
	}
	if (timing.armed) {
		ZLRS_TRACE_EVENT("rf_prep_irq", prepare_at_us, timing.irq_us);
		ZLRS_TRACE_EVENT("rf_owner_wake", timing.irq_us, timing.wake_us);
	} else {
		facts.prepare_wait_skipped++;
	}

	const uint8_t link_slot = slot;
	zlrs_link_t *link = &link_data;
	const uint32_t slot_start_us = zlrs_slot_timer_now_us();
	/* Slot decisions use the planned time even though construction happens before it. */
	const uint32_t now_ms = zlrs_slot_timer_uptime_ms(slot_nominal_us);
	const uint32_t hit_before = link->status.stats.slot_hit[link_slot];
	const uint32_t good_before = info->accepted_rx_events;
	const bool on_subg = is_subg();
	const zlrs_radio_req_t *req;

	/* Disabled slots stay idle; acquisition outside assigned slots corrupts phase/hop
	 * alignment. */
	if ((int32_t)(slot_deadline_us - zlrs_slot_timer_now_us()) <= 100) {
		facts.missed_slots++;
		zlrs_link_skip_slot(link, link_slot);
		return;
	}
	ZLRS_TRACE_EVENT("rf_serial_begin", slot_nominal_us, slot);
	pump_serial();
	ZLRS_TRACE_EVENT("rf_serial_end", slot_nominal_us, slot);
	ZLRS_TRACE_EVENT("rf_protocol_begin", slot_nominal_us, slot);

	update_power(zlrs_slot_timer_now_ms());
	if (!zlrs_link_advance_slot(link, link_slot, now_ms)) {
		return;
	}
	zlrs_link_plan_t plan;
	zlrs_link_plan(link, &plan);
	ZLRS_TRACE_EVENT("rf_protocol_end", slot_nominal_us, slot);
	if (plan.op == ZLRS_OP_IDLE) {
		(void)zlrs_link_finalize(link, &plan);
		return;
	}
	const uint32_t target_us = slot_nominal_us + CONFIG_ZLRS_APP_TX_GUARD_US;
	if ((int32_t)(slot_deadline_us - zlrs_slot_timer_now_us()) <= 100 ||
	    (plan.op == ZLRS_OP_TX &&
	     !zlrs_slot_timer_launch_ready(zlrs_slot_timer_now_us(), target_us,
					   ZLRS_SLOT_TIMER_LAUNCH_LEAD_US))) {
		facts.missed_slots++;
		return;
	}
	if (plan.op == ZLRS_OP_TX && on_subg &&
	    !zlrs_duty_reserve(
		    &duty, (zlrs_phy_airtime_us(plan.radio.phy, plan.radio.size) + 999U) / 1000U,
		    now_ms)) {
		return;
	}
	const uint32_t finalize_started = zlrs_slot_timer_now_us();
	bool hash_held = false;
	if (plan.op == ZLRS_OP_TX && link_data.status.cfg.profile == ZLRS_PROFILE_P1) {
		const int32_t budget = (int32_t)(target_us - zlrs_slot_timer_now_us()) -
				       ZLRS_SLOT_TIMER_LAUNCH_LEAD_US;
		hash_held = budget > 0 && zlrs_hash_acquire((uint32_t)budget);
		if (!hash_held) {
			facts.work.hash_busy_tx++;
			facts.missed_slots++;
			return;
		}
	}
	req = zlrs_link_finalize(link, &plan);
	if (hash_held) {
		zlrs_hash_release();
	}
	if (req == NULL) {
		return;
	}
	if (req->is_tx) {
		zlrs_timing_record(&facts.work.tx_finalize_us,
				   zlrs_slot_timer_now_us() - finalize_started);
	}
	zlrs_lr2021_slot(target_us, slot_deadline_us);

	const uint32_t prepare_started_us = zlrs_slot_timer_now_us();

	/* Channel and public packet metadata let two MCU histories be correlated.
	 * arg1: TX bit 31, PHY bits 24..27, sync slot bits 16..23, length bits 0..15. */
	ZLRS_TRACE_EVENT("rf_packet", req->freq_hz,
			 (req->is_tx ? BIT(31) : 0U) | ((uint32_t)req->phy << 24) |
				 ((uint32_t)req->sync_slot << 16) |
				 (req->is_tx ? req->size : req->max_rx_size));
	ZLRS_TRACE_EVENT("rf_prepare_begin", slot_nominal_us, req->is_tx);
	const int prepare_rc = zlrs_lr2021_prepare(req);
	ZLRS_TRACE_EVENT("rf_prepare_end", slot_nominal_us, (uint32_t)prepare_rc);
	if (req->is_tx) {
		zlrs_timing_record(&facts.work.tx_prepare_us,
				   zlrs_slot_timer_now_us() - slot_start_us);
	}
	if (prepare_rc != 0) {
		if (req->is_tx) {
			facts.late_prepare_entry_us = slot_start_us - prepare_at_us;
			facts.late_prepare_build_us = prepare_started_us - slot_start_us;
			facts.late_prepare_spi_us = zlrs_slot_timer_now_us() - prepare_started_us;
		}
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, zlrs_slot_timer_now_ms());
		return;
	}

	/* The complete frame is immutable in FIFO; the timer only issues SET_TX. */
	const int launch_rc = zlrs_lr2021_launch();

	if (launch_rc != 0) {
		zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0, zlrs_slot_timer_now_ms());
		return;
	}

	if (req->is_tx) {
		zlrs_link_tx_started(link, zlrs_slot_timer_uptime_ms(zlrs_lr2021_started_us()));
	}
	if (!req->is_tx) {
		/* RX-time work only fills payload storage. RX completion resolves ACK
		 * and packet identity before the next assigned TX is finalized. */
		pump_serial();
	}
	/* Window close ends RX waiting, not protocol acceptance of an already received
	 * frame. FIFO service/authentication may enter the next preparation window.
	 * The next TX still rechecks its own launch budget before finalization. */
	const uint32_t accept_deadline_us =
		next_slot_us + CONFIG_ZLRS_APP_TX_GUARD_US - ZLRS_SLOT_TIMER_LAUNCH_LEAD_US;
	const int32_t remaining_us = (int32_t)(slot_deadline_us - zlrs_slot_timer_now_us());

	zlrs_lr2021_result_t result = {0};
	if (remaining_us > 0) {
		radio_wait_done((uint32_t)remaining_us, accept_deadline_us, &result);
	}

	(void)complete_radio(&result, accept_deadline_us);

	/* If completion did not close the request, abort it before the next slot. */
	if (link->status.busy) {
		zlrs_lr2021_abort();
		zlrs_link_complete(link, req->is_tx ? ZLRS_RADIO_FAIL : ZLRS_RADIO_OK, 0U, 0, 0,
				   zlrs_slot_timer_now_ms());
	}

	info = &link->status;
	if (link->status.stats.slot_hit[link_slot] != hit_before &&
	    info->accepted_rx_events != good_before) {
		last_rx_any_ms = zlrs_slot_timer_now_ms();
		/* Only Follower follows verified packets timestamped in the GPIO ISR. */
		if (!(info->cfg.role == ZLRS_ROLE_LEADER) && !phase_adj_set && result.status == 0 &&
		    result.size > 0U) {
			/* Include SPI command transfer and synthesizer/PA startup in the phase
			 * origin. */
			const uint32_t expected = slot_nominal_us + CONFIG_ZLRS_APP_TX_GUARD_US +
						  ZLRS_LR2021_PHASE_STARTUP_US;
			const int32_t err =
				(int32_t)(result.done_us - result.airtime_us - expected);
			const int32_t limit = (int32_t)facts.frame_period_us / 16;

			/* Once acquired, discard delayed IRQ outliers rather than move the clock.
			 */
			if (phase_samples >= 8U && !info->radio_acquiring &&
			    (err > limit || err < -limit)) {
				return;
			}
			facts.phase_err_us = err;
			phase_adj_us =
				(err > ZLRS_PHASE_DEADBAND_US || err < -ZLRS_PHASE_DEADBAND_US)
					? CLAMP(phase_samples < 8U ? err : err / 4, -limit, limit)
					: 0;
			facts.phase_last_us = phase_adj_us;
			facts.phase_hits++;
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
	/* Keep the serial consumer and packet ownership in the RF thread. serial_read
	 * copies straight into the final payload, leaving headroom and tag room. */
	for (;;) {
		uint16_t capacity;
		uint8_t *payload = zlrs_link_tx_reserve(&link_data, &capacity);
		if (payload == NULL) {
			return;
		}
		const uint16_t got = zlrs_serial_read(payload, capacity);
		zlrs_link_tx_commit(&link_data, got);
		if (got == 0U) {
			return;
		}
	}
}

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
/* Most recent one-second window, kept up to date by report(). */
#endif

static void report(uint32_t now_ms)
{
	static uint32_t last_report_ms;
	if ((uint32_t)(now_ms - last_report_ms) < REPORT_PERIOD_MS) {
		return;
	}
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	static zlrs_rf_rate_sample_t before;
	const zlrs_link_stats_t *stats = &link_data.status.stats;
	const zlrs_rf_rate_sample_t now = {
		.wins = stats->slot_rx[0] + stats->slot_rx[1],
		.hits = stats->slot_hit[0] + stats->slot_hit[1],
		.tx = stats->tx_frames,
	};
	facts.rate = (zlrs_rf_rate_sample_t){
		.wins = now.wins - before.wins,
		.hits = now.hits - before.hits,
		.tx = now.tx - before.tx,
		.span_ms = last_report_ms != 0U ? now_ms - last_report_ms : REPORT_PERIOD_MS,
		.state = link_data.status.state,
	};
	before = now;
	facts.phase_hits = 0U;
#endif
	last_report_ms = now_ms;
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_STATS_LOG)
	const zlrs_lr2021_diag_t *radio_diag = zlrs_lr2021_diag();
	LOG_INF("data: bytes=%u air_tx=%u air_rx=%u failed=%u drop=%u lq=%u%%", data_rx_bytes,
		stats->tx_frames, stats->rx_frames, stats->radio_fail,
		stats->data_drop_bytes + zlrs_serial_dropped(), stats->lq_percent);
	LOG_INF("radio: transactions=%u late=%u phase=%d/%d duty_denied=%u",
		radio_diag->transactions,
		radio_diag->prepare_late_starts + radio_diag->trigger_late_starts,
		facts.phase_err_us, facts.phase_last_us, duty.denied);
#endif
}

/* RF facts for UI and diagnostics, including failed initialization. */
void zlrs_rf_publish_state(bool force)
{
	const zlrs_link_status_t *info = &link_data.status;
	if (info->state == ZLRS_STATE_CONNECTED) {
		facts.ever_connected = true;
	}
	const zlrs_rf_event_t state = {
		.role = info->cfg.role,
		.data_band = (is_subg() ? ZLRS_DATA_BAND_SUBG : ZLRS_DATA_BAND_24),
		.band_mode = info->cfg.automatic_band
				     ? ZLRS_DATA_BAND_AUTO
				     : (is_subg() ? ZLRS_DATA_BAND_SUBG : ZLRS_DATA_BAND_24),
		.data_phy = info->cfg.phy,
		.band_acquiring = info->radio_acquiring,
		.band_planned = info->band_plan.switch_frame != 0U,
		.state = info->state,
		.session_valid = info->session_valid,
		.link_lost = facts.ever_connected &&
			     (info->state != ZLRS_STATE_CONNECTED || info->radio_acquiring),
		.fault = !facts.radio_ready,
		.data_drop_bytes = info->stats.data_drop_bytes,
		.lq_percent = info->stats.lq_percent,
		.frame_period_us = facts.frame_period_us,
		.power_floor_dbm = facts.power.floor_dbm,
		.power_ceiling_dbm = facts.power.ceiling_dbm,
	};
	static zlrs_rf_event_t published;
	static bool have_state;
	if (!force && have_state && published.role == state.role &&
	    published.data_band == state.data_band && published.band_mode == state.band_mode &&
	    published.data_phy == state.data_phy &&
	    published.band_acquiring == state.band_acquiring &&
	    published.band_planned == state.band_planned && published.state == state.state &&
	    published.session_valid == state.session_valid &&
	    published.link_lost == state.link_lost && published.fault == state.fault &&
	    published.data_drop_bytes == state.data_drop_bytes &&
	    published.lq_percent == state.lq_percent &&
	    published.frame_period_us == state.frame_period_us &&
	    published.power_floor_dbm == state.power_floor_dbm &&
	    published.power_ceiling_dbm == state.power_ceiling_dbm) {
		return;
	}
	if (zbus_chan_pub(&zlrs_rf_chan, &state, K_NO_WAIT) == 0) {
		published = state;
		have_state = true;
	}
}

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
int zlrs_rf_phy_request(unsigned int phy)
{
	if (phy >= ZLRS_PHY_AUTO_COUNT) {
		return -EINVAL;
	}
	zlrs_rf_event_t status;
	if (zbus_chan_read(&zlrs_rf_chan, &status, K_MSEC(10)) != 0) {
		return -EAGAIN;
	}
	if (status.band_mode != ZLRS_DATA_BAND_AUTO || status.role != ZLRS_ROLE_LEADER) {
		return -EPERM;
	}
	atomic_set(&phy_request, phy + 1U);
	return 0;
}

#endif

const zlrs_rf_state_t *zlrs_rf_state(void)
{
	return &facts;
}

void zlrs_rf_log_snapshot(void)
{
	/* Capture production RF facts only at a safe boundary, before log persistence. */
	const zlrs_link_status_t *info = &link_data.status;

	LOG_INF("data snapshot: state=%u tx=%u rx=%u fail=%u tag=%u sync=%u/%u resp=%u/%u "
		"cfm=%u/%u queue=%u arq=%u frame=%u last_rx=%u pending=%u",
		link_data.status.state, info->stats.tx_frames, info->stats.rx_frames,
		info->stats.radio_fail, info->stats.rx_bad_tag, info->stats.tx_syncs,
		info->stats.rx_syncs, info->stats.sess_resp_tx, info->stats.sess_resp_rx,
		info->stats.sess_cfm_tx, info->stats.sess_cfm_rx, info->queue_count, info->arq_used,
		info->frame_index, info->last_rx_frame, info->sess_cfm_due ? 1U : 0U);
	LOG_INF("data progress: hop=%u fail_a=%u fail_b=%u dup=%u ooo=%u retry=%u "
		"seq=%u/%u nonces=%08x/%08x pair=%08x",
		info->fhss.index, info->stats.slot_fail[ZLRS_SLOT_DATA_A],
		info->stats.slot_fail[ZLRS_SLOT_DATA_B], info->stats.rx_dup,
		info->stats.rx_out_of_order, info->stats.tx_retries, info->arq_seq,
		info->arq_rx_seq, info->sess_nonce_leader, info->sess_nonce_follower,
		zlrs_link_pair_fingerprint(&link_data));
	const zlrs_lr2021_diag_t *radio_state = zlrs_lr2021_diag();
	LOG_INF("radio snapshot: late=%u setup=%u errors=%04x error_freq=%u phase=%d/%d",
		radio_state->prepare_late_starts + radio_state->trigger_late_starts,
		radio_state->submit_us.max, radio_state->chip_errors,
		radio_state->chip_error_freq_hz, facts.phase_err_us, facts.phase_last_us);
	LOG_INF("radio receive: hdr=%u crc=%u len=%u payload=%u expired=%u restart=%u",
		radio_state->rx_header_errors, radio_state->rx_crc_errors,
		radio_state->rx_length_errors, radio_state->rx_payload_errors,
		radio_state->rx_expired, radio_state->rx_restarted);
	LOG_INF("radio transmit: loads=%u errors=%u underflows=%u load_max=%u",
		radio_state->tx_fifo_loads, radio_state->tx_fifo_errors,
		radio_state->tx_fifo_underflows, radio_state->tx_fifo_load_us.max);
	zlrs_serial_state_t endpoints[2];
	const size_t count = zlrs_serial_state(endpoints, ARRAY_SIZE(endpoints));
	for (size_t i = 0U; i < count; i++) {
		LOG_INF("data endpoint: %s active=%u input=%u rx_queue=%u tx_queue=%u drop=%u",
			endpoints[i].name, endpoints[i].active ? 1U : 0U, endpoints[i].rx_total,
			endpoints[i].rx_queued, endpoints[i].tx_queued, endpoints[i].rx_dropped);
	}
}

void zlrs_rf_status_refresh(void)
{
	__ASSERT_NO_MSG(facts.paused);
	if (facts.radio_ready) {
		refresh_hardware_status();
	}
	zlrs_rf_publish_state(true);
}

int zlrs_rf_pair_start(void)
{
	zlrs_link_t *link = &link_data;
	if (!facts.links_ready) {
		return -ENOTCONN;
	}
	const zlrs_link_status_t *info = &link->status;
	if (info->state == ZLRS_STATE_UNCONFIGURED) {
		return -ENOTCONN;
	}
	if (link->status.state != ZLRS_STATE_PAIRING) {
		const uint32_t now_ms = zlrs_slot_timer_now_ms();
		random_error = 0;
		if (!zlrs_link_press(link, now_ms)) {
			return random_error != 0 ? random_error : -EIO;
		}
		if (link_data.status.cfg.automatic_band &&
		    (is_subg() || info->cfg.phy != ZLRS_PHY_FLRC_2600)) {
			change_band(ZLRS_DATA_BAND_24, ZLRS_PHY_FLRC_2600, false,
				    zlrs_slot_timer_now_ms());
		}
	}
	update_power(zlrs_slot_timer_now_ms());
	return 0;
}

void zlrs_rf_credentials(void)
{
	if (facts.links_ready) {
		const zlrs_link_cfg_t cfg = link_data.status.cfg;
		build_links(&cfg);
		facts.ever_connected = false;
	}
}

void zlrs_rf_pause(void)
{
	__ASSERT_NO_MSG(!facts.paused);
	zlrs_trace_maintenance(true);
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	facts.pause_started_cycles = k_cycle_get_64();
	facts.maintenance.count++;
	maintenance_gap = true;
#endif
	facts.paused = true;
	paused_at_ms = zlrs_slot_timer_now_ms();
	pause_had_plan = false;
	if (facts.links_ready) {
		const zlrs_link_status_t *info = &link_data.status;
		pause_had_plan = info->band_plan.switch_frame != 0U;
	}
	pause_priority = k_thread_priority_get(k_current_get());
	k_thread_priority_set(k_current_get(), CONFIG_MAIN_THREAD_PRIORITY);
	if (facts.radio_ready) {
		zlrs_lr2021_abort();
	}
	if (facts.links_ready && link_data.status.busy) {
		zlrs_link_complete(&link_data, ZLRS_RADIO_FAIL, 0U, 0, 0, zlrs_slot_timer_now_ms());
	}
}

void zlrs_rf_resume(void)
{
	__ASSERT_NO_MSG(facts.paused);
	zlrs_trace_maintenance(false);
	facts.paused = false;
	k_thread_priority_set(k_current_get(), pause_priority);
	if (facts.links_ready && link_data.status.cfg.automatic_band) {
		const uint32_t now_ms = zlrs_slot_timer_now_ms();
		if (pause_had_plan || (uint32_t)(now_ms - paused_at_ms) >= band_policy.loss_ms) {
			zlrs_link_acquire_radio(&link_data, now_ms);
			phase_samples = 0U;
			const zlrs_link_status_t *info = &link_data.status;
			zlrs_band_policy_reset(&band_policy, band_policy.probing, now_ms,
					       info->last_rx_ms);
		}
	}
	if (facts.links_ready) {
		update_power(zlrs_slot_timer_now_ms());
	}
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	const uint64_t elapsed = k_cyc_to_us_floor64(k_cycle_get_64() - facts.pause_started_cycles);
	facts.maintenance.total_us += elapsed;
	facts.maintenance.max_us = MAX(facts.maintenance.max_us, elapsed);
#endif
}

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
int zlrs_rf_power_request(bool automatic, int dbm)
{
	zlrs_rf_event_t status;
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
	/* A retried initialization must not publish or access a previous radio instance. */
	facts.links_ready = false;
	facts.radio_ready = false;
	memset(&facts.radio, 0, sizeof(facts.radio));
	facts.temperature_centi_c = INT16_MIN;
	facts.hardware_sampled_ms = 0U;
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	facts.maintenance = (zlrs_rf_maintenance_t){0};
	maintenance_gap = false;
#endif
	facts.skipped_frames = 0U;
	facts.missed_slots = 0U;
	memset(&facts.work, 0, sizeof(facts.work));
	/* Debug image only (gated in Kconfig, so the symbol does not exist in production builds):
	 * USB is up, so wait for the host to open the console port — otherwise a panic log lands
	 * on a port nobody has open. */
	/*
	 * Debug image only: wait for the host to open the console port. Feed the watchdog while
	 * waiting — it is armed before main() runs, so a wait longer than its timeout would reset
	 * the device mid-boot (and look exactly like a broken image).
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
	if (!zlrs_persist_state()->identity_valid) {
		LOG_ERR("no device id: staying off the air until one can be made");
		return -ENODEV;
	}

	/*
	 * Persist loads settings or board defaults and validates each frontend's requested
	 * region. RF owns the parameters adopted for this boot.
	 */
	const zlrs_persist_settings_t *const settings = &zlrs_persist_state()->settings;
	const bool automatic = settings->band == ZLRS_DATA_BAND_AUTO;
	const bool subg = settings->band == ZLRS_DATA_BAND_SUBG;
	region_id = settings->region;
	LOG_INF("data band: %s", subg ? "sub-G" : "2.4G");
	facts.region = zlrs_region_get(region_id);
	if (facts.region == NULL) {
		LOG_ERR("unsupported region %d", region_id);
		return -EINVAL;
	}
	LOG_INF("region %s, supported regions 0x%02x", facts.region->name,
		(unsigned int)zlrs_region_supported_mask());

	LOG_INF("role %s, profile P%u",
		(settings->role == ZLRS_ROLE_LEADER) ? "Leader" : "Follower",
		(unsigned int)settings->profile);

	/* Bench switches: with these off the link is no longer production-shaped, so say so at boot
	 */
	if (settings->profile != ZLRS_PROFILE_P1) {
		LOG_WRN("isolation: security profile P0, frames carry no tag");
	}
	if (!IS_ENABLED(CONFIG_ZLRS_APP_FHSS)) {
		LOG_WRN("isolation: hopping off, every frame stays on the sync channel");
	}
	if (power_minimum(subg) > power_maximum(subg)) {
		LOG_ERR("region %s is not supported by this board", facts.region->name);
		return -EINVAL;
	}
	zlrs_duty_init(&duty, facts.region, 0U);
	zlrs_band_policy_init(&band_policy, settings->role, facts.region->duty_window_ms,
			      zlrs_slot_timer_now_ms());
	facts.band_switches = 0U;
	/* The status LED comes up on its own thread and reads the state it renders: nothing to
	 * init here (app/src/ui.c, rf.h) */

	int rc = zlrs_slot_timer_init();
	if (rc != 0) {
		LOG_ERR("hardware slot timer init failed (%d)", rc);
		return rc;
	}

	struct rf_plan plan;

	link_config(settings->role, settings->profile, automatic, subg, ZLRS_PHY_FLRC_2600, &plan);
	facts.frame_period_us = plan.period_us;
	facts.slot_prepare_us = plan.prepare_us;
	build_links(&plan.config);

	rc = zlrs_lr2021_init(&owner_wake);
	if (rc != 0) {
		LOG_ERR("radio driver init failed (%d)", rc);
		return rc;
	}

	facts.radio_ready = true;
	/* Initial snapshot before the timer starts; later refreshes use management. */
	refresh_hardware_status();
	return 0;
}

void zlrs_rf_start(void)
{
	LOG_INF("entering data loop: %u Hz (%s)", (unsigned int)(1000000U / facts.frame_period_us),
		link_data.status.cfg.role == ZLRS_ROLE_LEADER ? "Leader" : "Follower");
	zlrs_slot_timer_start(facts.frame_period_us * CONFIG_ZLRS_APP_PACING_SCALE,
			      facts.slot_prepare_us, &owner_wake);
	consumed_frame = 0U;
	BUILD_ASSERT(CONFIG_NUM_METAIRQ_PRIORITIES >= 1);
	k_thread_priority_set(k_current_get(), K_PRIO_COOP(0));
}

void zlrs_rf_frame(void)
{
	uint32_t start_us;
	uint32_t timer_frame = consumed_frame;
	zlrs_slot_timer_wait_frame(&start_us, &timer_frame);

	/* Frames elapsed during maintenance still advance the original hop cadence. */
	while (++consumed_frame < timer_frame) {
		facts.skipped_frames++;
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
		if (maintenance_gap) {
			facts.maintenance.skipped_after_maintenance++;
		} else {
			facts.maintenance.skipped_without_maintenance++;
		}
#endif
		for (uint8_t slot = 0U; slot < ZLRS_SLOT_COUNT; slot++) {
			zlrs_link_skip_slot(&link_data, slot);
		}
	}
#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG)
	maintenance_gap = false;
#endif
	phase_adj_set = false;
	const uint32_t period = facts.frame_period_us * CONFIG_ZLRS_APP_PACING_SCALE;
	transition_reserved = false;

	for (uint8_t slot = 0U; slot < ZLRS_SLOT_COUNT; slot++) {
		const zlrs_link_status_t *info = &link_data.status;
		if (link_data.status.cfg.automatic_band && !transition_reserved &&
		    info->band_plan.switch_frame != 0U &&
		    info->band_plan.switch_frame == info->frame_index + (slot == 0U ? 2U : 1U)) {
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
		slot_deadline_us = next_us - facts.slot_prepare_us - 100U;
		run_slot(slot, next_us);
	}

	/* Re-read time after radio completions and any preceding maintenance window. */
	const uint32_t now_ms = zlrs_slot_timer_now_ms();
	update_power(now_ms);
	report(now_ms);

	const zlrs_link_status_t *info = &link_data.status;
	if (info->cfg.role != ZLRS_ROLE_LEADER) {
		const zlrs_link_cfg_t *const cfg = &info->cfg;
		const bool acquiring = info->radio_acquiring || info->state == ZLRS_STATE_SEARCHING;
		const uint32_t capture_wait_ms =
			info->radio_sync_seen
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

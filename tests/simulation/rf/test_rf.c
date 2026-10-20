/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/ztest.h>
#include <zephyr/random/random.h>
#include <zephyr/drivers/entropy.h>
#include "rf.h"
#include "persist.h"
#include "diagnostics.h"
#include "serial.h"
#include "platform/lr2021.h"
#include "platform/slot_timer.h"
#include "zlrs/link/link.h"
#include "band.h"
#include "power.h"

/* Real RF and two real protocol nodes; only the clock, LR2021 and serial edges are fake. */
static zlrs_link_t *local;
static zlrs_link_t peer;
static uint8_t peer_next_slot;
static uint32_t clock_us, period_us, frame, boundary_us;
static const zlrs_radio_req_t *active;
static bool pending;

static enum {
	NORMAL,
	TX_FAIL,
	RX_FAIL,
	NO_IRQ,
	PREPARE_FAIL,
	LAUNCH_FAIL
} mode;

static unsigned int tx_ok, tx_fail, rx_fail, rx_ok, aborts;
static unsigned int radio_reads;
static bool entropy_failed;
static int entropy_result;
static uint32_t entropy_sequence;
static unsigned int blocking_random_requests;
static unsigned int secure_random_failures, plain_random_requests;
static uint32_t scan_delay_override_ms, phase_shift_events;
static int32_t packet_phase_error_us;
static uint16_t source_left, delivered;
static uint16_t peer_delivered;
static bool block_24;
static int16_t rssi_24 = -42;
static int16_t local_rx_offset, peer_rx_offset;
static zlrs_band_policy_t peer_band_policy;
static zlrs_power_t peer_power;
static uint32_t retime_boundary_us;
static uint32_t transition_guard_us;
static const uint8_t outgoing[] = "host bytes";
static const uint8_t incoming[] = "peer bytes";

static int entropy_fill(const struct device *dev, uint8_t *dst, uint16_t len)
{
	ARG_UNUSED(dev);
	blocking_random_requests++;
	for (uint16_t i = 0U; i < len; i++) {
		dst[i] = (uint8_t)(++entropy_sequence);
	}
	return 0;
}

static int entropy_ready(const struct device *dev, uint8_t *dst, uint16_t len, uint32_t flags)
{
	ARG_UNUSED(dev);
	zassert_equal(flags, 0U, "RF must not request busy-wait entropy");
	if (entropy_failed) {
		secure_random_failures++;
		/* Failed providers can leave bytes behind; the application must ignore them. */
		memset(dst, 0xa5, len);
		return entropy_result;
	}
	for (uint16_t i = 0U; i < len; i++) {
		dst[i] = (uint8_t)(++entropy_sequence);
	}
	return len;
}

static DEVICE_API(entropy, entropy_api) = {
	.get_entropy = entropy_fill,
	.get_entropy_isr = entropy_ready,
};

DEVICE_DT_DEFINE(DT_NODELABEL(rf_entropy), NULL, NULL, NULL, NULL, PRE_KERNEL_1,
		 CONFIG_ENTROPY_INIT_PRIORITY, &entropy_api);

void __real_z_impl_sys_rand_get(void *dst, size_t len);

void __wrap_z_impl_sys_rand_get(void *dst, size_t len)
{
	plain_random_requests++;
	__real_z_impl_sys_rand_get(dst, len);
}

static const zlrs_persist_settings_t saved_settings = {
	.band = RF_TEST_BAND_AUTO ? ZLRS_DATA_BAND_AUTO
				  : (RF_TEST_BAND_SUBG ? ZLRS_DATA_BAND_SUBG : ZLRS_DATA_BAND_24),
	.region = ZLRS_REGION_CN433,
	.role = RF_TEST_ROLE_FOLLOWER ? ZLRS_ROLE_FOLLOWER : ZLRS_ROLE_LEADER,
	.profile = ZLRS_PROFILE_P1,
};
static const uint8_t saved_id[ZLRS_DEV_ID_LEN] = {1, 2, 3, 4};
static const uint8_t saved_key[ZLRS_KEY_LEN] = {0x42};
static const zlrs_persist_pairing_t saved_pairing = {
	.peer_id = {5, 6, 7, 8},
	.pair_salt = {1, 0, 0, 0, 2, 0, 0, 0},
};
static bool identity_valid, configured = true, paired = true;

const zlrs_persist_settings_t *zlrs_persist_settings(void)
{
	return &saved_settings;
}

bool zlrs_persist_get_device_id(uint8_t out[ZLRS_DEV_ID_LEN])
{
	if (!identity_valid) {
		return false;
	}
	memcpy(out, saved_id, sizeof(saved_id));
	return true;
}

bool zlrs_persist_get_root_key(uint8_t out[ZLRS_KEY_LEN])
{
	if (!configured) {
		return false;
	}
	memcpy(out, saved_key, sizeof(saved_key));
	return true;
}

bool zlrs_persist_get_pairing(zlrs_persist_pairing_t *out)
{
	if (!paired) {
		return false;
	}
	*out = saved_pairing;
	return true;
}

void __real_zlrs_link_init(zlrs_link_t *, const zlrs_link_cfg_t *, bool (*)(void *, uint32_t *),
			   void *, const zlrs_link_events_t *);

void __wrap_zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg,
			   bool (*random)(void *, uint32_t *), void *ctx,
			   const zlrs_link_events_t *events)
{
	__real_zlrs_link_init(link, cfg, random, ctx, events);
	local = link;
}

uint32_t zlrs_slot_timer_now_us(void)
{
	return clock_us;
}

uint32_t zlrs_slot_timer_now_ms(void)
{
	return clock_us / 1000U;
}

uint32_t zlrs_slot_timer_uptime_ms(uint32_t us)
{
	return us / 1000U;
}

int zlrs_slot_timer_init(void)
{
	return 0;
}

void zlrs_slot_timer_start(uint32_t us, uint32_t prepare, void (*wake)(void))
{
	period_us = us;
}

void zlrs_slot_timer_reconfigure(uint32_t us, uint32_t prepare, uint32_t guard)
{
	retime_boundary_us = boundary_us + period_us + guard + transition_guard_us;
	transition_guard_us = 0U;
	period_us = us;
}

void zlrs_slot_timer_hold_next_frame(uint32_t guard)
{
	transition_guard_us = guard;
}

static void peer_update_band(void)
{
	if (!RF_TEST_BAND_AUTO || !peer.configured) {
		return;
	}
	const uint32_t now = clock_us / 1000U;
	const zlrs_link_info_t info = zlrs_link_get_info(&peer);
	const int8_t next_power =
		info.state != ZLRS_STATE_CONNECTED || info.radio_acquiring ||
				peer_band_policy.power_boosted
			? zlrs_power_recover(&peer_power, now)
			: zlrs_power_update(&peer_power, info.peer_rssi_dbm, info.peer_lq_percent,
					    info.last_peer_feedback_ms, now);
	zlrs_link_set_power(&peer, next_power);
	bool change = false, subg = false, probe = false;
	zlrs_phy_t phy = ZLRS_PHY_FLRC_2600;
	if (info.band_plan.switch_frame && info.frame_index >= info.band_plan.switch_frame - 1U) {
		subg = info.band_plan.target_subg;
		phy = info.band_plan.target_phy;
		probe = !subg && (peer_band_policy.subg || phy < peer_band_policy.phy);
		change = true;
	} else {
		const zlrs_band_observation_t observation = {
			.now_ms = now,
			.last_rx_ms = info.last_rx_ms,
			.feedback_ms = info.last_peer_feedback_ms,
			.paired = info.paired,
			.pairing = info.state == ZLRS_STATE_PAIRING,
			.connected = info.session_valid && info.state == ZLRS_STATE_CONNECTED,
			.acquiring = info.radio_acquiring,
			.planned = info.band_plan.switch_frame != 0,
			.power_at_max = peer.cfg.tx_power_dbm >= peer_power.ceiling_dbm,
			.rssi_dbm = peer.stats.last_rssi,
			.peer_rssi_dbm = info.peer_rssi_dbm,
			.lq_percent = peer.stats.lq_percent,
			.peer_lq_percent = info.peer_lq_percent};
		const zlrs_band_action_t action =
			zlrs_band_policy_update(&peer_band_policy, &observation);
		if (action == ZLRS_BAND_BOOST) {
			zlrs_link_set_power(&peer, zlrs_power_recover(&peer_power, now));
		} else if (action == ZLRS_BAND_SEARCH) {
			subg = peer_band_policy.target_subg;
			phy = peer_band_policy.target_phy;
			change = true;
		} else if (action == ZLRS_BAND_PLAN) {
			const uint32_t lead =
				peer.cfg.resync_every_n + DIV_ROUND_UP(500000U, period_us);
			zassert_true(zlrs_link_plan_radio(&peer, peer_band_policy.target_subg,
							  peer_band_policy.target_phy,
							  info.frame_index + lead));
		}
	}
	if (change) {
		zlrs_link_cfg_t cfg = peer.cfg;
		cfg.domain = subg ? ZLRS_DOMAIN_CN433 : ZLRS_DOMAIN_ISM2400;
		cfg.mod = subg ? zlrs_mod_flrc(325000U) : zlrs_mod_phy(phy);
		cfg.tx_power_dbm = subg ? 10 : 12;
		zlrs_power_init(&peer_power, cfg.tx_power_dbm, subg ? -10 : -17, now);
		const uint32_t prepare =
			subg ? CONFIG_ZLRS_APP_PREPARE_US : CONFIG_ZLRS_APP_DATA_24_PREPARE_US;
		const uint32_t cadence =
			2U * (prepare + CONFIG_ZLRS_APP_TX_GUARD_US + ZLRS_LR2021_STARTUP_US +
			      zlrs_mod_airtime_us(&cfg.mod, ZLRS_LORA_FRAME_MAX_LEN) + 200U);
		cfg.sync_period_ms = cfg.mod.lora_sf != 0U          ? 300U
				     : cfg.mod.rate_bps >= 2600000U ? 100U
				     : cfg.mod.rate_bps >= 650000U  ? 300U
								    : 600U;
		cfg.resync_every_n = MIN(CONFIG_ZLRS_APP_RESYNC_PERIOD_MS, 1000U) * 1000U / cadence;
		cfg.arq_timeout_ms = MAX(10U, DIV_ROUND_UP(cadence, 1000U));
		zassert_true(zlrs_link_reconfigure_radio(&peer, &cfg));
		zlrs_link_acquire_radio(&peer, now);
		zlrs_band_policy_changed(&peer_band_policy, subg, phy, probe, now, info.last_rx_ms);
	}
}

void zlrs_slot_timer_wait_frame(uint32_t *start, uint32_t *number)
{
	/* The peer's clock advances even when local RF chose no hardware request. */
	while (peer_next_slot < ZLRS_SLOT_COUNT && frame > 0U) {
		zlrs_link_skip_slot(&peer, peer_next_slot++);
	}
	peer_next_slot = 0U;
	peer_update_band();
	if (retime_boundary_us != 0U) {
		boundary_us = retime_boundary_us;
		retime_boundary_us = 0U;
	} else {
		boundary_us += period_us;
	}
	clock_us = boundary_us;
	*start = boundary_us;
	*number = ++frame;
}

uint32_t zlrs_slot_timer_next_frame_us(void)
{
	return boundary_us + period_us;
}

void zlrs_slot_timer_wait_until(uint32_t us, struct zlrs_slot_timer_wait *timing)
{
	if ((int32_t)(us - clock_us) > 0) {
		clock_us = us;
	}
	*timing = (struct zlrs_slot_timer_wait){.wake_us = clock_us};
}

void zlrs_slot_timer_cancel_launch(void)
{
}

void zlrs_slot_timer_shift(int32_t us)
{
	phase_shift_events += us != 0;
}

uint32_t zlrs_slot_timer_scan_delay_ms(uint32_t us, uint16_t sync, bool acquired)
{
	return scan_delay_override_ms != 0U ? scan_delay_override_ms : 2000U;
}

void zlrs_watchdog_feed(void)
{
}

void zlrs_slot_timer_stats(struct zlrs_slot_timer_stats *out)
{
	*out = (struct zlrs_slot_timer_stats){.boundaries = frame};
}

void zlrs_lr2021_diag(zlrs_lr2021_diag_t *out)
{
	*out = (zlrs_lr2021_diag_t){0};
}

int zlrs_lr2021_init(void)
{
	return 0;
}

void zlrs_lr2021_read_status(zlrs_lr2021_status_t *out)
{
	zassert_false(pending);
	radio_reads++;
	*out = (zlrs_lr2021_status_t){
		.temperature_centi_c = 2500,
		.valid = ZLRS_LR2021_STATUS_TEMP,
		.sampled_ms = radio_reads,
	};
}

uint32_t zlrs_lr2021_airtime_us(const zlrs_mod_cfg_t *mod, uint16_t len)
{
	return zlrs_mod_airtime_us(mod, len);
}

void zlrs_lr2021_slot(uint32_t start, uint32_t end)
{
}

void zlrs_lr2021_wake(void)
{
}

int zlrs_lr2021_prepare(const zlrs_radio_req_t *req)
{
	if (req->is_tx) {
		const bool subg = req->freq_hz < 1500000000U;
		const int8_t maximum = subg ? 10 : 12;
		zassert_true(req->tx_power_dbm >= (subg ? -10 : -17));
		zassert_true(req->tx_power_dbm <= maximum);
		const uint8_t type = req->buf[0] & ZLRS_FRAME_TYPE_MASK;
		if (type >= ZLRS_PKT_PAIR_REQ && type <= ZLRS_PKT_PAIR_CFM) {
			zassert_equal(req->tx_power_dbm, -10, "pair packet power");
		} else if (local->state != ZLRS_STATE_CONNECTED || local->radio_acquiring) {
			zassert_equal(req->tx_power_dbm, maximum, "recovery transmission power");
		}
	}
	active = req;
	return mode == PREPARE_FAIL ? -EIO : 0;
}

int zlrs_lr2021_launch(void)
{
	if (mode == LAUNCH_FAIL) {
		return -ETIME;
	}
	pending = true;
	return 0;
}

int zlrs_lr2021_launch_at(uint32_t us)
{
	clock_us = us;
	return zlrs_lr2021_launch();
}

int zlrs_lr2021_preload_tx(const uint8_t *bytes, uint16_t size)
{
	return 0;
}

bool zlrs_lr2021_irq_pending(void)
{
	return pending && mode != NO_IRQ;
}

int zlrs_lr2021_wait_us(uint32_t us)
{
	clock_us += us;
	return -EAGAIN;
}

void zlrs_lr2021_abort(void)
{
	pending = false;
	aborts++;
}

bool zlrs_lr2021_process_irq(zlrs_lr2021_result_t *out)
{
	if (!pending || mode == NO_IRQ) {
		return false;
	}
	pending = false;
	clock_us += 100U;
	*out = (zlrs_lr2021_result_t){.completed_ms = clock_us / 1000U,
				      .done_us = clock_us,
				      .rssi_dbm = active->freq_hz > 1500000000U ? rssi_24 : -42};
	if (active->is_tx && mode == TX_FAIL) {
		out->status = -EIO;
		tx_fail++;
		return true;
	}
	if (!active->is_tx && mode == RX_FAIL) {
		out->status = -ETIMEDOUT;
		rx_fail++;
		return true;
	}
	while (peer_next_slot < local->slot) {
		zlrs_link_skip_slot(&peer, peer_next_slot++);
	}
	const zlrs_radio_req_t *other = zlrs_link_slot(&peer, local->slot, out->completed_ms);
	peer_next_slot = local->slot + 1U;
	if (active->is_tx) {
		tx_ok++;
		if (other && !other->is_tx && other->freq_hz == active->freq_hz &&
		    other->mod.rate_bps == active->mod.rate_bps &&
		    other->mod.lora_sf == active->mod.lora_sf &&
		    !(block_24 && active->freq_hz > 1000000000U)) {
			memcpy(other->buf, active->buf, active->size);
			const int16_t rssi = out->rssi_dbm +
					     (RF_TEST_BAND_AUTO && active->freq_hz > 1500000000U
						      ? active->tx_power_dbm - 12 + peer_rx_offset
						      : 0);
			zlrs_link_complete(&peer, ZLRS_RADIO_OK, active->size, rssi, 0,
					   out->completed_ms);
			/* The other node's fake hardware clock is aligned with its received packet. */
			(void)zlrs_link_radio_captured(&peer);
		} else if (other) {
			zlrs_link_complete(&peer, ZLRS_RADIO_OK, 0, 0, 0, out->completed_ms);
		}
	} else {
		rx_ok++;
		if (other && other->is_tx && other->freq_hz == active->freq_hz &&
		    other->mod.rate_bps == active->mod.rate_bps &&
		    other->mod.lora_sf == active->mod.lora_sf &&
		    !(block_24 && active->freq_hz > 1000000000U)) {
			out->size = other->size;
			if (RF_TEST_BAND_AUTO && active->freq_hz > 1500000000U) {
				out->rssi_dbm += other->tx_power_dbm - 12 + local_rx_offset;
			}
			memcpy(active->buf, other->buf, other->size);
			out->airtime_us = zlrs_mod_airtime_us(&active->mod, out->size);
			out->done_us = boundary_us + period_us * local->slot / ZLRS_SLOT_COUNT +
				       CONFIG_ZLRS_APP_TX_GUARD_US + ZLRS_LR2021_PHASE_STARTUP_US +
				       out->airtime_us + packet_phase_error_us;
		}
		if (other) {
			zlrs_link_complete(&peer, ZLRS_RADIO_OK, 0, 0, 0, out->completed_ms);
		}
	}
	return true;
}

uint16_t zlrs_serial_read(uint8_t *out, uint16_t capacity)
{
	const uint16_t take = MIN(source_left, capacity);
	memcpy(out, outgoing + sizeof(outgoing) - 1U - source_left, take);
	source_left -= take;
	return take;
}

bool zlrs_serial_receive(const uint8_t *bytes, uint16_t size)
{
	zassert_equal(size, sizeof(incoming) - 1U);
	zassert_mem_equal(bytes, incoming, size);
	delivered += size;
	return true;
}

uint32_t zlrs_serial_dropped(void)
{
	return 0;
}

static bool receive_peer(void *ctx, const uint8_t *bytes, uint16_t size)
{
	zassert_equal(size, sizeof(outgoing) - 1U);
	zassert_mem_equal(bytes, outgoing, size);
	peer_delivered += size;
	return true;
}

static void frames(unsigned int count)
{
	while (count--) {
		const zlrs_data_band_t previous_band = zlrs_rf_status().data_band;
		zlrs_rf_frame();
		zassert_false(zlrs_link_is_busy(local));
		if (RF_TEST_BAND_AUTO && zlrs_rf_status().data_band != previous_band) {
			/* A transition must reach UI without waiting for the metrics timer. */
			zlrs_rf_publish_state();
			zlrs_rf_status_t published;
			zassert_ok(zbus_chan_read(&zlrs_rf_chan, &published, K_NO_WAIT));
			zassert_equal(published.data_band, zlrs_rf_status().data_band);
			zassert_equal(published.band_mode, ZLRS_DATA_BAND_AUTO);
		}
	}
}

static bool peer_random(void *ctx, uint32_t *out)
{
	ARG_UNUSED(ctx);
	/* Deterministic healthy peer, independent of the application's simulated source. */
	*out = ++entropy_sequence;
	return true;
}

static bool both_on_band(bool subg)
{
	const zlrs_link_info_t other = zlrs_link_get_info(&peer);
	const zlrs_rf_status_t status = zlrs_rf_status();
	return status.data_band == (subg ? ZLRS_DATA_BAND_SUBG : ZLRS_DATA_BAND_24) &&
	       peer.cfg.domain == (subg ? ZLRS_DOMAIN_CN433 : ZLRS_DOMAIN_ISM2400) &&
	       status.session_valid && other.session_valid && !status.band_acquiring &&
	       !other.radio_acquiring;
}

static bool both_on_phy(zlrs_phy_t phy)
{
	return both_on_band(false) && zlrs_rf_status().data_phy == phy &&
	       zlrs_mod_phy_id(&peer.cfg.mod) == phy;
}

static void debug_power_acceptance(void)
{
	frames(DIV_ROUND_UP(300000U, period_us));
	zlrs_rf_publish_state();
	const zlrs_rf_status_t before = zlrs_rf_status();
	zassert_ok(zlrs_rf_power_request(false, before.power_floor_dbm));
	frames(1);
	zassert_true(zlrs_rf_status().power_fixed);
	zassert_equal(zlrs_rf_status().tx_power_dbm, before.power_floor_dbm);
	zlrs_rf_publish_state();
	zassert_equal(zlrs_rf_power_request(false, before.power_ceiling_dbm + 1), -EINVAL);
	/* Capture can retain CONNECTED and a session; it must still restore maximum. */
	zlrs_link_acquire_radio(local, clock_us / 1000U);
	zlrs_link_acquire_radio(&peer, clock_us / 1000U);
	frames(1);
	zassert_equal(zlrs_rf_status().tx_power_dbm, before.power_ceiling_dbm);
	zassert_false(zlrs_rf_status().power_fixed);
	frames(150);
	zlrs_rf_publish_state();
	zassert_ok(zlrs_rf_power_request(false, before.power_floor_dbm));
	frames(1);
	zassert_true(zlrs_rf_status().power_fixed);
	zassert_ok(zlrs_rf_power_request(true, 0));
	frames(1);
	zassert_false(zlrs_rf_status().power_fixed);
	zassert_equal(zlrs_rf_status().tx_power_dbm, before.power_ceiling_dbm);
}

static void publication_acceptance(void)
{
	zlrs_rf_publish_state();
	zlrs_rf_status_t before, after;
	zassert_ok(zbus_chan_read(&zlrs_rf_chan, &before, K_NO_WAIT));
	/* Counters refresh at 1 Hz even without an LED input change. */
	local->stats.tx_frames++;
	zlrs_rf_publish_state();
	zassert_ok(zbus_chan_read(&zlrs_rf_chan, &after, K_NO_WAIT));
	zassert_equal(after.tx_frames, before.tx_frames);
	k_msleep(1001);
	zlrs_rf_publish_state();
	zassert_ok(zbus_chan_read(&zlrs_rf_chan, &after, K_NO_WAIT));
	zassert_equal(after.tx_frames, local->stats.tx_frames);
	/* Failed publication must retry the same change at the next boundary. */
	const uint8_t quality = local->stats.lq_percent;
	local->stats.lq_percent = quality == 1U ? 2U : 1U;
	zassert_ok(zbus_chan_claim(&zlrs_rf_chan, K_NO_WAIT));
	zlrs_rf_publish_state();
	zassert_equal(((const zlrs_rf_status_t *)zbus_chan_const_msg(&zlrs_rf_chan))->lq_percent,
		      quality);
	zassert_ok(zbus_chan_finish(&zlrs_rf_chan));
	zlrs_rf_publish_state();
	zassert_ok(zbus_chan_read(&zlrs_rf_chan, &after, K_NO_WAIT));
	zassert_equal(after.lq_percent, local->stats.lq_percent);
	local->stats.lq_percent = quality;
}

static void power_before_profile_acceptance(void)
{
	/* Simulate a wall after both ends saved power at close range. Only one
	 * direction weakens at a time; the weak transmitter learns through DATA. */
	for (unsigned int direction = 0; direction < 2; direction++) {
		for (unsigned int n = 0;
		     n < 16000 && (zlrs_rf_status().tx_power_dbm > 3 || peer.cfg.tx_power_dbm > 3);
		     n++) {
			frames(1);
		}
		zassert_true(zlrs_rf_status().tx_power_dbm <= 3 && peer.cfg.tx_power_dbm <= 3,
			     "both endpoints did not qualify normal power saving");
		const uint32_t switches = zlrs_rf_status().band_switches;
		local_rx_offset = direction == 0 ? -40 : 0;
		peer_rx_offset = direction == 1 ? -40 : 0;
		for (unsigned int n = 0; n < DIV_ROUND_UP(5000000U, period_us); n++) {
			frames(1);
			zassert_true(both_on_phy(ZLRS_PHY_FLRC_2600));
			zassert_equal(zlrs_rf_status().band_switches, switches,
				      "link downgraded before using its power headroom");
			zassert_equal(local->band_plan.switch_frame, 0);
			zassert_equal(peer.band_plan.switch_frame, 0);
		}
		zassert_equal(zlrs_rf_status().tx_power_dbm, 12);
		zassert_equal(peer.cfg.tx_power_dbm, 12);
		local_rx_offset = peer_rx_offset = 0;
		frames(DIV_ROUND_UP(500000U, period_us));
	}
	/* Recovery also takes priority over an in-range debug power override. */
	zassert_ok(zlrs_rf_power_request(false, 3));
	frames(2);
	zassert_true(zlrs_rf_status().power_fixed);
	peer_rx_offset = -40;
	frames(DIV_ROUND_UP(1500000U, period_us));
	zassert_false(zlrs_rf_status().power_fixed);
	zassert_equal(zlrs_rf_status().tx_power_dbm, 12);
	zassert_true(both_on_phy(ZLRS_PHY_FLRC_2600));
	peer_rx_offset = 0;
	frames(DIV_ROUND_UP(500000U, period_us));
}

static void automatic_acceptance(void)
{
	identity_valid = true;
	zassert_ok(zlrs_rf_init());
	zlrs_link_cfg_t cfg = *zlrs_link_get_config(local);
	cfg.role = RF_TEST_ROLE_FOLLOWER ? ZLRS_ROLE_LEADER : ZLRS_ROLE_FOLLOWER;
	const zlrs_link_events_t events = {.on_data = receive_peer};
	__real_zlrs_link_init(&peer, &cfg, peer_random, NULL, &events);
	zlrs_link_set_dev_id(&peer, saved_pairing.peer_id);
	zlrs_link_set_root_key(&peer, saved_key, clock_us / 1000U);
	zlrs_link_set_peer(&peer, saved_id, saved_pairing.pair_salt, clock_us / 1000U);
	zlrs_band_policy_init(&peer_band_policy, true, cfg.role == ZLRS_ROLE_LEADER, false, 0,
			      clock_us / 1000U);
	zlrs_power_init(&peer_power, 12, -17, clock_us / 1000U);
	zlrs_rf_start();
	frames(150);
	zassert_true(both_on_band(false));
	zassert_equal(zlrs_rf_status().power_floor_dbm, -17);
	zassert_equal(zlrs_rf_status().power_ceiling_dbm, 12);
	debug_power_acceptance();
	if (RF_TEST_ROLE_FOLLOWER) {
		/* A legal SYNC with a large physical phase error is not DATA readiness. */
		zlrs_rf_pause();
		zlrs_rf_resume();
		zlrs_link_acquire_radio(local, clock_us / 1000U);
		zlrs_link_acquire_radio(&peer, clock_us / 1000U);
		packet_phase_error_us = period_us / 8U;
		frames(30);
		zassert_true(local->radio_sync_seen && local->radio_acquiring);
		zassert_true(peer.radio_acquiring);
		packet_phase_error_us = 0;
		frames(50);
		zassert_true(both_on_band(false));
	}
	const uint32_t nonce = local->sess_nonce_leader;
	power_before_profile_acceptance();

	/* Wall-like attenuation should select a usable 2.4 GHz profile, not SubG. */
	static const struct {
		int16_t rssi;
		zlrs_phy_t phy;
	} margins[] = {
		{-97, ZLRS_PHY_FLRC_325}, {-102, ZLRS_PHY_LORA_SF6}, {-105, ZLRS_PHY_LORA_SF7}};

	for (size_t i = 0; i < ARRAY_SIZE(margins); i++) {
		rssi_24 = margins[i].rssi;
		for (unsigned int n = 0; n < 8000 && !both_on_phy(margins[i].phy); n++) {
			frames(1);
		}
		zassert_true(both_on_phy(margins[i].phy), "2.4 GHz profile %u not captured",
			     margins[i].phy);
		frames(DIV_ROUND_UP(10000000U, period_us));
		zassert_true(both_on_phy(margins[i].phy), "usable slow profile left 2.4 GHz");
	}
	rssi_24 = -42;
	block_24 = true;
	source_left = sizeof(outgoing) - 1U;
	zassert_equal(zlrs_link_write(&peer, incoming, sizeof(incoming) - 1U),
		      sizeof(incoming) - 1U);
	for (unsigned int i = 0; i < 30000 && (!both_on_band(true) || source_left ||
					       delivered != sizeof(incoming) - 1U ||
					       peer_delivered != sizeof(outgoing) - 1U);
	     i++) {
		frames(1);
	}
	zassert_true(both_on_band(true));
	zassert_equal(zlrs_rf_status().power_floor_dbm, -10);
	zassert_equal(zlrs_rf_status().power_ceiling_dbm, 10);
	zassert_equal(delivered, sizeof(incoming) - 1U);
	zassert_equal(peer_delivered, sizeof(outgoing) - 1U);
	zassert_equal(local->sess_nonce_leader, nonce);
	bool tried = false, returned = false;
	for (unsigned int i = 0; i < 18000; i++) {
		frames(1);
		tried |= zlrs_rf_status().data_band == ZLRS_DATA_BAND_24;
		if (tried && both_on_band(true)) {
			returned = true;
			break;
		}
	}
	zassert_true(tried && returned);
	block_24 = false;
	bool preferred = false;
	for (unsigned int i = 0; i < 18000; i++) {
		frames(1);
		if (both_on_band(false)) {
			preferred = true;
			break;
		}
	}
	zassert_true(preferred);
	for (unsigned int i = 0; i < 60000 && !both_on_phy(ZLRS_PHY_FLRC_2600); i++) {
		frames(1);
	}
	zassert_true(both_on_phy(ZLRS_PHY_FLRC_2600), "healthy link did not recover fast FLRC");
	zassert_equal(zlrs_rf_phy_request(ZLRS_PHY_COUNT), -EINVAL);
	if (RF_TEST_ROLE_FOLLOWER) {
		zassert_equal(zlrs_rf_phy_request(ZLRS_PHY_LORA_SF6), -EPERM);
	} else {
		zassert_ok(zlrs_rf_phy_request(ZLRS_PHY_LORA_SF6));
		for (unsigned int i = 0; i < 8000 && !both_on_phy(ZLRS_PHY_LORA_SF6); i++) {
			frames(1);
		}
		zassert_true(both_on_phy(ZLRS_PHY_LORA_SF6));
		zassert_ok(zlrs_rf_phy_request(ZLRS_PHY_FLRC_2600));
		for (unsigned int i = 0; i < 8000 && !both_on_phy(ZLRS_PHY_FLRC_2600); i++) {
			frames(1);
		}
		zassert_true(both_on_phy(ZLRS_PHY_FLRC_2600));
	}
	zassert_equal(local->sess_nonce_leader, nonce);
	zassert_equal(local->stats.data_drop_bytes, 0);
	zassert_equal(peer.stats.data_drop_bytes, 0);
	zassert_true(zlrs_rf_status().band_switches >= 4U);
	zlrs_rf_pause();
	zlrs_rf_status_refresh();
	zlrs_rf_resume();
	frames(1000);
	zassert_true(both_on_band(false));
	k_thread_priority_set(k_current_get(), 0);
}

static void start_peers(bool fail_entropy)
{
	if (RF_TEST_BAND_AUTO) {
		ztest_test_skip();
	}
	identity_valid = true;
	entropy_failed = fail_entropy;
	zassert_ok(zlrs_rf_init());
	zlrs_link_cfg_t cfg = *zlrs_link_get_config(local);
	cfg.role = RF_TEST_ROLE_FOLLOWER ? ZLRS_ROLE_LEADER : ZLRS_ROLE_FOLLOWER;
	const zlrs_link_events_t events = {.on_data = receive_peer};
	__real_zlrs_link_init(&peer, &cfg, peer_random, NULL, &events);
	zlrs_link_set_dev_id(&peer, saved_pairing.peer_id);
	zlrs_link_set_root_key(&peer, saved_key, clock_us / 1000U);
	zlrs_link_set_peer(&peer, saved_id, saved_pairing.pair_salt, clock_us / 1000U);
	zlrs_rf_start();
}

static void connected_peers(void)
{
	start_peers(false);
	frames(150);
	zassert_equal(local->state, ZLRS_STATE_CONNECTED);
	zassert_equal(peer.state, ZLRS_STATE_CONNECTED);
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	memset(&peer, 0, sizeof(peer));
	peer_next_slot = 0;
	clock_us = boundary_us = period_us = 0;
	frame = 0;
	active = NULL;
	pending = false;
	mode = NORMAL;
	tx_ok = tx_fail = rx_fail = rx_ok = aborts = radio_reads = 0;
	entropy_failed = false;
	entropy_result = 0;
	entropy_sequence = blocking_random_requests = 0U;
	secure_random_failures = plain_random_requests = 0;
	scan_delay_override_ms = phase_shift_events = 0;
	packet_phase_error_us = 0;
	source_left = delivered = peer_delivered = 0;
	block_24 = false;
	rssi_24 = -42;
	local_rx_offset = peer_rx_offset = 0;
	retime_boundary_us = transition_guard_us = 0;
	identity_valid = configured = paired = true;
}

static void after(void *unused)
{
	ARG_UNUSED(unused);
	k_thread_priority_set(k_current_get(), 0);
}

ZTEST(rf_results, test_initialization_failure_stays_off_radio)
{

	identity_valid = false;
	zassert_equal(zlrs_rf_init(), -ENODEV);
	zlrs_rf_pause();
	zlrs_rf_status_refresh();
	zassert_equal(radio_reads, 0);
	zassert_false(zlrs_rf_status().radio.valid);
	zlrs_rf_resume();
}

ZTEST(rf_results, test_entropy_failure_and_recovery)
{
	start_peers(true);

	if (!RF_TEST_ROLE_FOLLOWER) {
		zassert_equal(zlrs_rf_pair_start(), -EAGAIN);
		zassert_not_equal(zlrs_rf_status().state, ZLRS_STATE_PAIRING);
	}
	const unsigned int plain_before = plain_random_requests;
	frames(150);
	zassert_true(secure_random_failures > 0U);
	zassert_false(local->session_valid || local->session_pending);
	zassert_equal(local->sess_nonce_leader, 0U);
	zassert_equal(local->sess_nonce_follower, 0U);
	zassert_equal(tx_ok, 0U, "failed entropy must not produce a handshake transmission");
	zassert_equal(plain_random_requests, plain_before, "noncryptographic RNG fallback");
	zassert_equal(blocking_random_requests, 0U, "RF called the blocking entropy API");
	entropy_failed = false;
	frames(150);
	zassert_equal(zlrs_link_get_state(local), ZLRS_STATE_CONNECTED);
	zassert_equal(zlrs_link_get_state(&peer), ZLRS_STATE_CONNECTED);
}

ZTEST(rf_results, test_partial_nonce_is_not_used)
{
	entropy_result = 3;
	start_peers(true);
	frames(150);
	zassert_true(secure_random_failures > 0U);
	zassert_false(local->session_valid || local->session_pending);
	zassert_equal(local->sess_nonce_leader, 0U);
	zassert_equal(local->sess_nonce_follower, 0U);
	zassert_equal(tx_ok, 0U);
	zassert_equal(blocking_random_requests, 0U);
	entropy_failed = false;
	frames(150);
	zassert_equal(local->state, ZLRS_STATE_CONNECTED);
	zassert_equal(peer.state, ZLRS_STATE_CONNECTED);
}

ZTEST(rf_results, test_entropy_error_is_not_a_nonce)
{
	entropy_result = -EIO;
	start_peers(true);
	if (!RF_TEST_ROLE_FOLLOWER) {
		zassert_equal(zlrs_rf_pair_start(), -EIO);
	}
	frames(150);
	zassert_true(secure_random_failures > 0U);
	zassert_false(local->session_valid || local->session_pending);
	zassert_equal(tx_ok, 0U);
	zassert_equal(blocking_random_requests, 0U);
}

ZTEST(rf_results, test_connected_data_does_not_need_more_entropy)
{
	connected_peers();
	entropy_failed = true;
	entropy_result = -EAGAIN;
	const unsigned int requests = secure_random_failures;
	source_left = sizeof(outgoing) - 1U;
	frames(150);
	zassert_equal(local->state, ZLRS_STATE_CONNECTED);
	zassert_equal(peer.state, ZLRS_STATE_CONNECTED);
	zassert_true(peer_delivered > 0U);
	zassert_equal(secure_random_failures, requests);
	zassert_equal(blocking_random_requests, 0U);
}

ZTEST(rf_results, test_periodic_sync_cadence)
{
	connected_peers();
	zlrs_link_cfg_t cfg = *zlrs_link_get_config(local);
	/* A five-second SYNC cadence must remain five seconds above 255 frames. */
	if (!RF_TEST_BAND_SUBG && CONFIG_ZLRS_APP_PACING_SCALE == 1) {
		zassert_true(cfg.resync_every_n > UINT8_MAX);
	}
	zlrs_link_t *const leader = RF_TEST_ROLE_FOLLOWER ? &peer : local;
	const uint32_t syncs = zlrs_link_get_stats(leader)->tx_syncs;
	frames(12000000U / period_us);
	const uint32_t periodic = zlrs_link_get_stats(leader)->tx_syncs - syncs;
	zassert_true(periodic == 2U || periodic == 3U);
}

ZTEST(rf_results, test_rebooted_peer_clock_reacquires)
{
	connected_peers();

	zlrs_link_cfg_t cfg = *zlrs_link_get_config(&peer);
	const zlrs_link_events_t events = {.on_data = receive_peer};

	if (RF_TEST_ROLE_FOLLOWER) {
		/* A new leader clock can differ beyond the locked outlier limit before
		 * silence is long enough to enable scanning. Valid new-session packets
		 * must restart phase acquisition even when they continue arriving. */
		scan_delay_override_ms = UINT32_MAX;
		const uint32_t previous_nonce = local->sess_nonce_leader;
		mode = RX_FAIL;
		frames(DIV_ROUND_UP((cfg.sync_period_ms + 50U) * 1000U, period_us));
		mode = NORMAL;
		__real_zlrs_link_init(&peer, &cfg, peer_random, NULL, &events);
		zlrs_link_set_dev_id(&peer, saved_pairing.peer_id);
		zlrs_link_set_root_key(&peer, saved_key, clock_us / 1000U);
		zlrs_link_set_peer(&peer, saved_id, saved_pairing.pair_salt, clock_us / 1000U);
		peer_next_slot = 0U;
		packet_phase_error_us = period_us / 8U;
		const uint32_t shifts_before = phase_shift_events;
		frames(2000);
		zassert_equal(local->state, ZLRS_STATE_CONNECTED);
		zassert_equal(peer.state, ZLRS_STATE_CONNECTED);
		zassert_not_equal(local->sess_nonce_leader, previous_nonce);
		zassert_true(phase_shift_events > shifts_before,
			     "new session was rejected by the previous clock's outlier gate");
		packet_phase_error_us = 0;
		scan_delay_override_ms = 0U;
	}
}

ZTEST(rf_results, test_duplex_delivery_and_counters)
{
	connected_peers();

	source_left = sizeof(outgoing) - 1U;
	zassert_equal(zlrs_link_write(&peer, incoming, sizeof(incoming) - 1U),
		      sizeof(incoming) - 1U);
	frames(100);
	zassert_equal(delivered, sizeof(incoming) - 1U);
	zassert_equal(source_left, 0);
	zassert_equal(zlrs_link_get_stats(local)->last_rssi, -42);
	zassert_equal(zlrs_link_get_stats(local)->tx_frames, tx_ok);
	zassert_equal(zlrs_link_get_stats(local)->slot_rx[0] +
			      zlrs_link_get_stats(local)->slot_rx[1],
		      rx_ok);
}

ZTEST(rf_results, test_power_saving_and_loss_recovery)
{
	connected_peers();

	debug_power_acceptance();
	const uint32_t power_wait_frames = 60000000U / period_us;
	for (uint32_t i = 0U; i < power_wait_frames &&
			      zlrs_rf_status().tx_power_dbm == (RF_TEST_BAND_SUBG ? 10 : 12);
	     ++i) {
		frames(1);
	}
	zassert_true(zlrs_rf_status().tx_power_dbm < (RF_TEST_BAND_SUBG ? 10 : 12));
	mode = RX_FAIL;
	frames(DIV_ROUND_UP(1100000U, period_us));
	zassert_equal(zlrs_rf_status().tx_power_dbm, RF_TEST_BAND_SUBG ? 10 : 12);
	zassert_false(zlrs_rf_status().power_fixed);
}

ZTEST(rf_results, test_rx_failure)
{
	connected_peers();
	mode = RX_FAIL;
	frames(50);
	zassert_true(local->stats.radio_fail > 0);
	zassert_equal(radio_reads, 1);
}

ZTEST(rf_results, test_tx_failure)
{
	connected_peers();
	mode = TX_FAIL;
	frames(50);
	zassert_true(local->stats.radio_fail > 0);
	zassert_equal(radio_reads, 1);
}

ZTEST(rf_results, test_prepare_failure)
{
	connected_peers();
	mode = PREPARE_FAIL;
	frames(50);
	zassert_true(local->stats.radio_fail > 0);
	zassert_equal(radio_reads, 1);
}

ZTEST(rf_results, test_launch_failure)
{
	connected_peers();
	mode = LAUNCH_FAIL;
	frames(50);
	zassert_true(local->stats.radio_fail > 0);
	zassert_equal(radio_reads, 1);
}

ZTEST(rf_results, test_missing_irq)
{
	connected_peers();
	mode = NO_IRQ;
	frames(50);
	zassert_true(aborts > 0);
	zassert_equal(radio_reads, 1);
}

ZTEST(rf_results, test_pair_cancel_restores_power)
{
	connected_peers();

	zassert_ok(zlrs_rf_pair_start());
	zassert_equal(zlrs_rf_status().tx_power_dbm, -10);
	frames(DIV_ROUND_UP(250000U, period_us));
	zassert_ok(zlrs_rf_pair_cancel());
	zassert_equal(zlrs_rf_status().tx_power_dbm, RF_TEST_BAND_SUBG ? 10 : 12);
}

ZTEST(rf_results, test_pair_timeout_restores_power)
{
	connected_peers();

	zassert_ok(zlrs_rf_pair_start());
	mode = RX_FAIL;
	frames(DIV_ROUND_UP(30100000U, period_us));
	zassert_not_equal(zlrs_rf_status().state, ZLRS_STATE_PAIRING);
	zassert_equal(zlrs_rf_status().tx_power_dbm, RF_TEST_BAND_SUBG ? 10 : 12);
}

ZTEST(rf_results, test_pair_confirmation_restores_power)
{
	connected_peers();
	zassert_ok(zlrs_rf_pair_start());
	zlrs_link_press(&peer, clock_us / 1000U);
	frames(DIV_ROUND_UP(3000000U, period_us));
	zassert_equal(zlrs_rf_status().state, ZLRS_STATE_CONNECTED);
	zassert_equal(zlrs_rf_status().tx_power_dbm, RF_TEST_BAND_SUBG ? 10 : 12);
	zassert_true(local->stats.pair_req_tx + local->stats.pair_resp_tx > 0U);
	zassert_true(local->stats.pair_cfm_tx + peer.stats.pair_cfm_tx > 0U);
}

ZTEST(rf_results, test_state_publication_retries_and_refreshes)
{
	connected_peers();
	publication_acceptance();
}

ZTEST(rf_results, test_diagnostics_snapshot_and_pagination)
{
	connected_peers();
	zlrs_rf_pause();
	zlrs_rf_status_refresh();
	zassert_equal(radio_reads, 2);
	zlrs_rf_status_t published;
	zassert_ok(zbus_chan_read(&zlrs_rf_chan, &published, K_NO_WAIT));
	zassert_equal(published.radio.temperature_centi_c, 2500);
	zassert_equal(published.radio.valid, ZLRS_LR2021_STATUS_TEMP);
	zassert_equal(published.radio.sampled_ms, 2);
	uint8_t chunk[80];
	uint16_t total;
	uint8_t written;
	for (unsigned int report = ZLRS_DIAGNOSTIC_LINK; report <= ZLRS_DIAGNOSTIC_POWER;
	     ++report) {
		uint16_t offset = 0U;
		do {
			zassert_ok(zlrs_diagnostics_read((zlrs_diagnostic_report_t)report, offset,
							 chunk, sizeof(chunk), &total, &written));
			zassert_true(written > 0U);
			offset += written;
		} while (offset < total);
		zassert_equal(offset, total);
		zassert_ok(zlrs_diagnostics_read((zlrs_diagnostic_report_t)report, total, chunk,
						 sizeof(chunk), &total, &written));
		zassert_equal(written, 0U);
	}
	zassert_equal(zlrs_diagnostics_read(ZLRS_DIAGNOSTIC_LINK, 1U, chunk, sizeof(chunk), &total,
					    &written),
		      -EINVAL);
	zassert_equal(zlrs_diagnostics_read((zlrs_diagnostic_report_t)255, 0U, chunk, sizeof(chunk),
					    &total, &written),
		      -EINVAL);
	zlrs_rf_resume();
}

ZTEST(rf_results, test_credential_removal_retires_active_session)
{
	connected_peers();
	zlrs_rf_pause();

	/* Deleting the saved pairing clears the active peer/session in the same window. */
	paired = false;
	zlrs_rf_credentials();
	uint8_t peer_id[ZLRS_DEV_ID_LEN], salt[ZLRS_PAIR_SALT_LEN];
	zassert_false(zlrs_rf_pairing(peer_id, salt));
	zassert_true(zlrs_rf_status().configured);
	zassert_false(zlrs_rf_status().paired || zlrs_rf_status().session_valid);
	/* Factory reset removes the key and ID until persist initializes again at boot. */
	configured = identity_valid = false;
	zlrs_rf_credentials();
	zassert_false(zlrs_rf_status().configured);
	const uint8_t empty_id[ZLRS_DEV_ID_LEN] = {0};
	zassert_mem_equal(zlrs_rf_status().dev_id, empty_id, sizeof(empty_id));
	zlrs_rf_resume();
}

ZTEST(rf_results, test_automatic_band_recovery_and_session_retention)
{
	if (!RF_TEST_BAND_AUTO) {
		ztest_test_skip();
	}
	automatic_acceptance();
}

ZTEST_SUITE(rf_results, NULL, NULL, before, after, NULL);

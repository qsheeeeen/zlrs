/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/ztest.h>
#include <zephyr/random/random.h>
#include "rf.h"
#include "persist.h"
#include "diagnostics.h"
#include "serial.h"
#include "platform/lr2021.h"
#include "platform/slot_timer.h"
#include "zlrs/link/link.h"

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
static uint16_t source_left, delivered;
static const uint8_t outgoing[] = "host bytes";
static const uint8_t incoming[] = "peer bytes";

static const zlrs_persist_settings_t saved_settings = {
	.band = RF_TEST_BAND_SUBG ? ZLRS_DATA_BAND_SUBG : ZLRS_DATA_BAND_24,
	.region = ZLRS_REGION_CN433,
	.role = RF_TEST_ROLE_RX ? ZLRS_ROLE_RX : ZLRS_ROLE_TX,
	.profile = ZLRS_PROFILE_P1,
	.power_dbm = 10,
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

void __real_zlrs_link_init(zlrs_link_t *, const zlrs_link_cfg_t *, bool (*)(void *, uint32_t *), void *,
			   const zlrs_link_events_t *);
void __wrap_zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg,
			   bool (*random)(void *, uint32_t *), void *ctx, const zlrs_link_events_t *events)
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
void zlrs_slot_timer_wait_frame(uint32_t *start, uint32_t *number)
{
	/* The peer's clock advances even when local RF chose no hardware request. */
	while (peer_next_slot < ZLRS_SLOT_COUNT && frame > 0U) {
		zlrs_link_skip_slot(&peer, peer_next_slot++);
	}
	peer_next_slot = 0U;
	boundary_us += period_us;
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
}
uint32_t zlrs_slot_timer_scan_delay_ms(uint32_t us, uint16_t sync, bool acquired)
{
	return 2000;
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
	*out = (zlrs_lr2021_result_t){
		.completed_ms = clock_us / 1000U, .done_us = clock_us, .rssi_dbm = -42};
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
		if (other && !other->is_tx && other->freq_hz == active->freq_hz) {
			memcpy(other->buf, active->buf, active->size);
			zlrs_link_complete(&peer, ZLRS_RADIO_OK, active->size, -42, 0,
					   out->completed_ms);
		} else if (other) {
			zlrs_link_complete(&peer, ZLRS_RADIO_OK, 0, 0, 0, out->completed_ms);
		}
	} else {
		rx_ok++;
		if (other && other->is_tx && other->freq_hz == active->freq_hz) {
			out->size = other->size;
			memcpy(active->buf, other->buf, other->size);
			out->airtime_us = zlrs_mod_airtime_us(&active->mod, out->size);
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
	return true;
}
static void frames(unsigned int count)
{
	while (count--) {
		zlrs_rf_frame();
		zassert_false(zlrs_link_is_busy(local));
	}
}
static bool peer_random(void *ctx, uint32_t *out)
{
	ARG_UNUSED(ctx);
	*out = sys_rand32_get();
	return true;
}
ZTEST(rf_results, test_protocol_completion_and_timeouts)
{
	zassert_equal(zlrs_rf_init(), -ENODEV);
	zlrs_rf_pause();
	zlrs_rf_status_refresh();
	zassert_equal(radio_reads, 0); /* Failed init remains manageable without chip access. */
	zassert_equal(zlrs_rf_status().radio.valid, 0);
	zlrs_rf_resume();
	identity_valid = true;
	zassert_ok(zlrs_rf_init());
	zassert_equal(radio_reads, 1);
	zlrs_link_cfg_t cfg = *zlrs_link_get_config(local);
	cfg.role = RF_TEST_ROLE_RX ? ZLRS_ROLE_TX : ZLRS_ROLE_RX;
	const zlrs_link_events_t events = {.on_data = receive_peer};
	__real_zlrs_link_init(&peer, &cfg, peer_random, NULL, &events);
	zlrs_link_set_dev_id(&peer, saved_pairing.peer_id);
	zlrs_link_set_root_key(&peer, saved_key, 0);
	zlrs_link_set_peer(&peer, saved_id, saved_pairing.pair_salt, 0);
	zlrs_rf_start();
	frames(150);
	zassert_equal(zlrs_link_get_state(local), ZLRS_STATE_CONNECTED);
	zassert_equal(zlrs_link_get_state(&peer), ZLRS_STATE_CONNECTED);
	/* A five-second SYNC cadence must remain five seconds above 255 frames. */
	if (!RF_TEST_BAND_SUBG && CONFIG_ZLRS_APP_PACING_SCALE == 1) {
		zassert_true(cfg.resync_every_n > UINT8_MAX);
	}
	zlrs_link_t *const master = RF_TEST_ROLE_RX ? &peer : local;
	const uint32_t syncs = zlrs_link_get_stats(master)->tx_syncs;
	frames(12000000U / period_us);
	const uint32_t periodic = zlrs_link_get_stats(master)->tx_syncs - syncs;
	zassert_true(periodic == 2U || periodic == 3U);

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
	const uint32_t power_wait_frames = 60000000U / period_us;
	for (uint32_t i = 0U; i < power_wait_frames && zlrs_rf_status().tx_power_dbm == 10; ++i) {
		frames(1);
	}
	zassert_true(zlrs_rf_status().tx_power_dbm < 10);
	mode = RX_FAIL;
	frames(1000);
	zassert_equal(zlrs_rf_status().tx_power_dbm, 10);
	zassert_true(rx_fail > 0);
	zassert_equal(zlrs_link_get_stats(local)->radio_fail, rx_fail);
	mode = TX_FAIL;
	frames(50);
	zassert_true(tx_fail > 0);
	zassert_equal(zlrs_link_get_stats(local)->radio_fail, rx_fail + tx_fail);
	const uint32_t failed = zlrs_link_get_stats(local)->radio_fail;
	mode = PREPARE_FAIL;
	frames(30);
	zassert_true(zlrs_link_get_stats(local)->radio_fail > failed);
	mode = LAUNCH_FAIL;
	frames(30);
	mode = NO_IRQ;
	frames(30);
	zassert_true(aborts > 0);
	zassert_equal(radio_reads, 1); /* Ordinary frames/status publication never use SPI. */
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
	k_thread_priority_set(k_current_get(), 0);
}
ZTEST_SUITE(rf_results, NULL, NULL, NULL, NULL, NULL);

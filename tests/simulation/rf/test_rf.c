/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/ztest.h>
#include "rf/rf.h"
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
static uint16_t source_left, delivered;
static const uint8_t outgoing[] = "host bytes";
static const uint8_t incoming[] = "peer bytes";

void __real_zlrs_link_init(zlrs_link_t *, const zlrs_link_cfg_t *, uint32_t (*)(void *), void *,
			   const zlrs_link_events_t *);
void __wrap_zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg,
			   uint32_t (*random)(void *), void *ctx, const zlrs_link_events_t *events)
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
uint32_t zlrs_slot_timer_scan_delay_ms(uint32_t us, uint8_t sync, bool acquired)
{
	return 2000;
}
void zlrs_watchdog_feed(void)
{
}
int zlrs_lr2021_init(void)
{
	return 0;
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
bool zlrs_lr2021_poll(zlrs_lr2021_result_t *out)
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
ZTEST(rf_results, test_protocol_completion_and_timeouts)
{
	zlrs_persist_state_t saved = {
		.settings = {.band = ZLRS_DATA_BAND_24,
			     .region = ZLRS_REGION_CN433,
			     .role = RF_TEST_ROLE_RX ? ZLRS_ROLE_RX : ZLRS_ROLE_TX,
			     .profile = ZLRS_PROFILE_P1,
			     .power_dbm = -10},
		.dev_id = {1, 2, 3, 4},
		.root_key = {0x42},
		.configured = true,
		.identity_valid = true,
		.paired = true,
		.pairing = {.peer_id = {5, 6, 7, 8}, .pair_salt = {1, 0, 0, 0, 2, 0, 0, 0}},
	};
	zassert_ok(zlrs_rf_init(&saved));
	zlrs_link_cfg_t cfg = *zlrs_link_get_config(local);
	cfg.role = RF_TEST_ROLE_RX ? ZLRS_ROLE_TX : ZLRS_ROLE_RX;
	const zlrs_link_events_t events = {.on_data = receive_peer};
	__real_zlrs_link_init(&peer, &cfg, NULL, NULL, &events);
	zlrs_link_set_dev_id(&peer, saved.pairing.peer_id);
	zlrs_link_set_root_key(&peer, saved.root_key, 0);
	zlrs_link_set_peer(&peer, saved.dev_id, saved.pairing.pair_salt, 0);
	zlrs_rf_start();
	frames(150);
	zassert_equal(zlrs_link_get_state(local), ZLRS_STATE_CONNECTED);
	zassert_equal(zlrs_link_get_state(&peer), ZLRS_STATE_CONNECTED);
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
	mode = RX_FAIL;
	frames(30);
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
	k_thread_priority_set(k_current_get(), 0);
}
ZTEST_SUITE(rf_results, NULL, NULL, NULL, NULL, NULL);

/* SPDX-License-Identifier: Apache-2.0 */
/* Test image only. Real RF/CRC/auth/ARQ stay active; faults alter accepted RX facts. */
#include "link/link.h"
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/sys/atomic.h>

static atomic_t drop_every, rssi_override, expires_ms;
static atomic_t dropped, received, tx_feedback, toggles, identical_toggles;
static uint8_t previous_quality;
static bool quality_started;

void __real_zlrs_link_complete(zlrs_link_t *link, int status, uint16_t rx_len, int16_t rssi,
			       int8_t snr, uint32_t now_ms);

void __wrap_zlrs_link_complete(zlrs_link_t *link, int status, uint16_t rx_len, int16_t rssi,
			       int8_t snr, uint32_t now_ms)
{
	if (link->status.busy && link->req.is_tx && status == ZLRS_RADIO_OK &&
	    (link->req.buf[0] & ZLRS_FRAME_TYPE_MASK) <= ZLRS_PKT_DATA_F2L &&
	    link->req.size >= ZLRS_FRAME_HDR_LEN + ZLRS_DATA_PREFIX_LEN) {
		const uint8_t quality = link->req.buf[ZLRS_FRAME_HDR_LEN + 2U];
		atomic_inc(&tx_feedback);
		if (quality_started && ((quality ^ previous_quality) & ZLRS_LQ_TOGGLE_MASK)) {
			atomic_inc(&toggles);
			if ((quality & ZLRS_LQ_VALUE_MASK) ==
			    (previous_quality & ZLRS_LQ_VALUE_MASK)) {
				atomic_inc(&identical_toggles);
			}
		}
		quality_started = true;
		previous_quality = quality;
	}
	if (link->status.busy && !link->req.is_tx && status == ZLRS_RADIO_OK && rx_len > 0U) {
		const uint32_t count = (uint32_t)atomic_inc(&received) + 1U;
		const bool active =
			(int32_t)((uint32_t)atomic_get(&expires_ms) - k_uptime_get_32()) > 0;
		if (active) {
			const int override = atomic_get(&rssi_override);
			if (override != 0) {
				rssi = override;
			}
			const int32_t every = atomic_get(&drop_every);
			const bool drop =
				every > 0 ? count % every == 0U : every < 0 && count % -every != 0U;
			/* Preserve SYNC/session traffic so quality faults exercise a live link. */
			if (drop && (link->rx_buf[0] & ZLRS_FRAME_TYPE_MASK) <= ZLRS_PKT_DATA_F2L) {
				rx_len = 0U;
				atomic_inc(&dropped);
			}
		}
	}
	__real_zlrs_link_complete(link, status, rx_len, rssi, snr, now_ms);
}

static int read_status(struct smp_streamer *ctxt)
{
	zcbor_state_t *zs = ctxt->writer->zs;
	bool ok = zcbor_tstr_put_lit(zs, "result") && zcbor_int32_put(zs, 0);
#define FIELD(name)                                                                                \
	ok = ok && zcbor_tstr_put_lit(zs, #name) && zcbor_int32_put(zs, atomic_get(&name))
	FIELD(dropped);
	FIELD(received);
	FIELD(tx_feedback);
	FIELD(toggles);
	FIELD(identical_toggles);
#undef FIELD
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int configure(struct smp_streamer *ctxt)
{
	uint32_t duration = 0U;
	int32_t every = 0;
	int32_t rssi = 0;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("drop_every", zcbor_int32_decode, &every),
		ZCBOR_MAP_DECODE_KEY_DECODER("rssi_dbm", zcbor_int32_decode, &rssi),
		ZCBOR_MAP_DECODE_KEY_DECODER("duration_ms", zcbor_uint32_decode, &duration),
	};
	if (zcbor_map_decode_bulk(ctxt->reader->zs, fields, ARRAY_SIZE(fields), &decoded) != 0 ||
	    every == 1 || every == -1 || every > 1000 || every < -1000 || rssi > 0 || rssi < -255 ||
	    duration > 120000U) {
		return MGMT_ERR_EINVAL;
	}
	atomic_set(&drop_every, every);
	atomic_set(&rssi_override, rssi);
	atomic_set(&expires_ms, k_uptime_get_32() + duration);
	return read_status(ctxt);
}

static const struct mgmt_handler handlers[] = {{.mh_read = read_status, .mh_write = configure}};
static struct mgmt_group group = {
	.mg_handlers = handlers, .mg_handlers_count = ARRAY_SIZE(handlers), .mg_group_id = 67};

static void register_control(void)
{
	mgmt_register_group(&group);
}

MCUMGR_HANDLER_DEFINE(zlrs_power_test, register_control);

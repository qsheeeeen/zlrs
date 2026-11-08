/* Test peer: generate wire input and execute the production transaction API.
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "link/internal.h"
#ifdef __cplusplus
}
#endif

static inline uint16_t zlrs_test_encode(const zlrs_frame_t *frame, uint8_t *buf, uint16_t cap,
                                       uint16_t len, uint8_t tag_len)
{
	if (frame->type > ZLRS_PKT_DATA_F2L) {
		return zlrs_frame_encode(frame, buf, cap, len, tag_len);
	}
	if (len > cap || len < ZLRS_DATA_HEADROOM + tag_len) {
		return 0;
	}
	const uint16_t payload = len - ZLRS_DATA_HEADROOM - tag_len;
	const uint16_t total = zlrs_frame_encode_data(buf, cap, (zlrs_pkt_type_t)frame->type, frame->seq,
	                                           &frame->body.data.header, payload, tag_len);
	if (total != 0) {
		if (payload != 0) {
			memcpy(buf + ZLRS_DATA_HEADROOM, frame->body.data.bytes, payload);
		}
		memcpy(buf + total - tag_len, frame->tag, tag_len);
	}
	return total;
}

static inline const zlrs_radio_req_t *zlrs_test_slot(zlrs_link_t *link, uint8_t slot, uint32_t now_ms)
{
	if (!zlrs_link_advance_slot(link, slot, now_ms)) {
		return NULL;
	}
	const zlrs_link_plan_t plan = zlrs_link_plan(link);
	return zlrs_link_finalize(link, &plan);
}

static inline void zlrs_test_rx(zlrs_link_t *link, const uint8_t *buf, uint16_t len,
                               int16_t rssi, int8_t snr, uint32_t now_ms)
{
	if (buf == NULL || len == 0 || (link->req_pending && link->req.is_tx)) {
		return;
	}
	if (len > sizeof(link->rx_buf)) {
		link->stats.rx_bad_frame++;
		return;
	}
	if (!link->req_pending) {
		(void)link_submit_radio(link, false, link->rx_buf, 0U, link_sync_freq_hz(link),
					ZLRS_SYNC_SLOT_DATA, link->sync_word,
					link->cfg.rx_timeout_ms);
	}
	memcpy(link->req.buf, buf, len);
	zlrs_link_complete(link, ZLRS_RADIO_OK, len, rssi, snr, now_ms);
}

static inline uint16_t zlrs_test_write(zlrs_link_t *link, const uint8_t *data, uint16_t len)
{
	uint16_t written = 0;
	while (data != NULL && written < len) {
		uint16_t capacity;
		uint8_t *dst = zlrs_link_tx_reserve(link, &capacity);
		if (dst == NULL) {
			break;
		}
		const uint16_t take = capacity < len - written ? capacity : len - written;
		memcpy(dst, data + written, take);
		zlrs_link_tx_commit(link, take);
		written += take;
	}
	if (data != NULL) {
		link->stats.data_drop_bytes += len - written;
	}
	return written;
}

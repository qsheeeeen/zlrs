/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* Packet storage owns user bytes from admission until ACK (or best-effort TX).
 * Headers and tags are rebuilt in place; retrying never copies the payload. */

static bool receive_ack_bit(const zlrs_link_t *link)
{
	return link->arq_rx_started && ((link->arq_rx_seq + 1U) & 1U) != 0U;
}

static void arq_commit_ack(zlrs_link_t *link)
{
	link_release_data(link);
	link->arq_used = 0U;
	link->arq_launched = false;
	link->arq_local_failed = false;
	link->arq_seq = (uint8_t)((link->arq_seq + 1U) & ZLRS_DATA_SEQ_MASK);
	link->arq_retries = 0U;
}

/* Payload has already been written at HEADROOM. Only mutable wire fields are
 * touched here. The complete frame remains frozen until radio completion. */
static uint16_t encode_data(zlrs_link_t *link, uint8_t *buf, uint16_t cap, uint16_t len,
			    uint8_t seq)
{
	if (link->cfg.profile > ZLRS_PROFILE_P1 || link->tx_seq == 0U ||
	    link->tx_seq == UINT32_MAX || len > link->cfg.data_mtu) {
		return 0U;
	}
	const int16_t rssi = link->stats.last_rssi;
	const zlrs_data_header_t header = {
		.data_seq = seq,
		.ack = link->cfg.arq && receive_ack_bit(link),
		.rssi_magnitude = rssi < 0 ? (uint8_t)(-rssi > 255 ? 255 : -rssi) : 0U,
		.lq_percent = link->stats.lq_percent,
		.lq_toggle = link->stats.lq_generation & 1U,
	};
	const uint8_t type =
		link->cfg.role == ZLRS_ROLE_LEADER ? ZLRS_PKT_DATA_L2F : ZLRS_PKT_DATA_F2L;
	const uint8_t tag_len = zlrs_profile_tag_len(link->cfg.profile);
	const uint16_t total =
		zlrs_frame_encode_data(buf, cap, type, link->tx_seq, &header, len, tag_len);
	if (total == 0U) {
		return 0U;
	}
	const uint16_t size = total - tag_len;
	if (tag_len != 0U) {
		zlrs_tag(link_tag_key_for(link, buf[0], true), ZLRS_KEY_LEN, buf, size, buf + size);
	}
	link->stats.tx_frame_builds++;
	link->stats.tx_auth_tags += tag_len != 0U;
	return total;
}

bool link_arq_retry_due(const zlrs_link_t *link, uint32_t now_ms)
{
	const uint16_t timeout =
		link->cfg.arq_timeout_ms > 0U ? link->cfg.arq_timeout_ms : ZLRS_ARQ_TIMEOUT_MS;
	return link->arq_used != 0U &&
	       (link->arq_local_failed ||
		(link->arq_launched && (int32_t)(now_ms - link->arq_sent_ms) >= timeout));
}

static void arq_commit_data(zlrs_link_t *link, uint16_t len)
{
	link->arq_len = len;
	link->arq_launched = false;
	link->arq_used = 1U;
	link->arq_retries = 0U;

}

static const zlrs_radio_req_t *send_packet(zlrs_link_t *link, bool retry)
{
	if (link->tx_packet_count == 0U) {
		return NULL;
	}
	const uint16_t index = link->queue_head;
	zlrs_tx_packet_t *packet = &link->tx_packets[index];
	uint8_t *buf = link_packet_bytes(link, index);
	const uint8_t seq = link->cfg.arq ? link->arq_seq : 0U;
	const uint16_t size = encode_data(link, buf, ZLRS_FRAME_MAX_LEN, packet->len, seq);
	if (size == 0U) {
		return NULL;
	}
	packet->sealed = true;
	const zlrs_radio_req_t *req = link_submit_tx(link, buf, size, link_slot_freq_hz(link),
						     ZLRS_SYNC_SLOT_DATA, link->sync_word);
	if (req == NULL) {
		return NULL;
	}
	link->arq_local_failed = false;
	link->tx_seq++;
	link->pending_data_len = link->cfg.arq ? 0U : packet->len;
	if (link->cfg.arq && !retry) {
		arq_commit_data(link, packet->len);
	}
	return req;
}

static const zlrs_radio_req_t *send_data_arq(zlrs_link_t *link, uint32_t now_ms)
{
	if (link_arq_retry_due(link, now_ms)) {
		if (link->arq_retries >= ZLRS_ARQ_MAX_RETRIES) {
			link->arq_retries = 0U;
			link->stats.tx_arq_retry_runs++;
		}
		link->arq_retries++;
		link->stats.tx_retries++;
		const uint32_t age = link->arq_launched ? now_ms - link->arq_sent_ms : 0U;
		link->stats.tx_retry_age_ms_last = age;
		if (age > link->stats.tx_retry_age_ms_max) {
			link->stats.tx_retry_age_ms_max = age;
		}
		return send_packet(link, true);
	}
	if (link->arq_used != 0U) {
		link->stats.tx_arq_wait_stalls++;
		return NULL;
	}
	return send_packet(link, false);
}

const zlrs_radio_req_t *link_send_empty_data(zlrs_link_t *link)
{
	const uint16_t size =
		encode_data(link, link->tx_buf, sizeof(link->tx_buf), 0U, link->arq_seq);
	if (size == 0U) {
		return NULL;
	}
	link->tx_seq++;
	link->pending_data_len = 0U;
	return link_submit_tx(link, link->tx_buf, size, link_slot_freq_hz(link),
			      ZLRS_SYNC_SLOT_DATA, link->sync_word);
}

static void handle_ack_bit(zlrs_link_t *link, bool bit, uint32_t now_ms)
{
	if (link->arq_used == 0U || !link->arq_launched ||
	    bit != (((link->arq_seq + 1U) & 1U) != 0U)) {
		return;
	}
	const int32_t elapsed = (int32_t)(now_ms - link->arq_first_sent_ms);
	const uint32_t delay = elapsed > 0 ? (uint32_t)elapsed : 0U;
	link->stats.tx_ack_delay_ms_last = delay;
	if (delay > link->stats.tx_ack_delay_ms_max) {
		link->stats.tx_ack_delay_ms_max = delay;
	}
	link->stats.tx_ack_delay_ms_sum += delay;
	link->stats.tx_ack_entries++;
	arq_commit_ack(link);
	link->arq_sent_ms = now_ms;
}

const zlrs_radio_req_t *link_send_data(zlrs_link_t *link, uint32_t now_ms)
{
	return link->cfg.arq ? send_data_arq(link, now_ms) : send_packet(link, false);
}

static bool deliver_data(zlrs_link_t *link, const uint8_t *bytes, uint16_t len)
{
	return link->ev.on_data != NULL && len != 0U && link->ev.on_data(link->ev.ctx, bytes, len);
}

void link_handle_data(zlrs_link_t *link, const zlrs_data_view_t *view, uint32_t now_ms)
{
	const zlrs_data_header_t *const data = &view->header;
	const uint16_t data_len = view->len;
	if (link->cfg.arq) {
		handle_ack_bit(link, data->ack != 0U, now_ms);
	}
	if (data->rssi_magnitude != 0U) {
		const uint32_t gap_ms =
			link->cfg.lq_feedback_gap_ms != 0U ? link->cfg.lq_feedback_gap_ms : 1000U;
		const bool baseline = link->peer_lq_percent == ZLRS_LQ_UNAVAILABLE ||
				      (uint32_t)(now_ms - link->last_peer_feedback_ms) >= gap_ms;
		const uint8_t quality = data->lq_percent;
		const bool toggle = data->lq_toggle != 0U;

		link->peer_rssi_dbm = -(int16_t)data->rssi_magnitude;
		link->last_peer_feedback_ms = now_ms;
		if (quality <= 100U && (baseline || toggle != link->peer_lq_toggle)) {
			link->peer_lq_percent = quality;
			link->peer_lq_toggle = toggle;
			/* A skipped RAM generation denotes an uncertain baseline, not a new window. */
			link->peer_lq_generation +=
				baseline && link->peer_lq_generation != 0U ? 2U : 1U;
		} else if (quality > 100U) {
			link->peer_lq_percent = ZLRS_LQ_UNAVAILABLE;
		}
	}
	if (data_len == 0U) {
		return;
	}
	if (link->cfg.arq) {
		const uint8_t seq = data->data_seq;
		const uint8_t next =
			link->arq_rx_started ? (link->arq_rx_seq + 1U) & ZLRS_DATA_SEQ_MASK : 0U;
		link->arq_ack_pending = true;
		if (seq != next) {
			if (link->arq_rx_started && seq == link->arq_rx_seq) {
				link->stats.rx_dup++;
			} else {
				link->stats.rx_out_of_order++;
			}
			return;
		}
		if (deliver_data(link, view->bytes, data_len)) {
			link->arq_rx_started = true;
			link->arq_rx_seq = seq;
		}
		return;
	}
	if (!deliver_data(link, view->bytes, data_len)) {
		link->stats.data_drop_bytes += data_len;
	}
}

void link_quality_reset(zlrs_link_t *link)
{
	link->lq_last_seq = 0U;
	link->lq_started = false;
	link->stats.lq_rx = 0U;
	link->stats.lq_expected = 0U;
	link->stats.lq_percent = ZLRS_LQ_UNAVAILABLE;
	link->stats.lq_generation = 0U;
	link->stats.last_rssi = 0;
	link->peer_rssi_dbm = 0;
	link->peer_lq_percent = ZLRS_LQ_UNAVAILABLE;
	link->peer_lq_generation = 0U;
	link->peer_lq_toggle = false;
	link->last_peer_feedback_ms = 0U;
}

void link_lq_account(zlrs_link_t *link, uint32_t seq)
{
	uint32_t delta = seq - link->lq_last_seq;

	if (!link->lq_started) {
		/* Only the first sample of a session has no preceding sequence to compare. */
		delta = 1U;
		link->lq_started = true;
	}

	link->lq_last_seq = seq;
	link->stats.lq_rx++;
	const uint64_t expected = (uint64_t)link->stats.lq_expected + delta;

	if (expected >= ZLRS_LQ_WINDOW_FRAMES) {
		link->stats.lq_percent = (uint8_t)(((uint64_t)link->stats.lq_rx * 100U) / expected);
		link->stats.lq_generation++;
		link->stats.lq_rx = 0U;
		link->stats.lq_expected = 0U;
	} else {
		link->stats.lq_expected = (uint16_t)expected;
	}
}

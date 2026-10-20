/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

#define ZLRS_LQ_WINDOW_FRAMES 128U

/* Byte queue, peer quality feedback, backpressure, ARQ and prepared DATA packets. */

static bool receive_ack_bit(const zlrs_link_t *link)
{
	return link->arq_rx_started && ((link->arq_rx_seq + 1U) & 1U) != 0U;
}

static uint16_t arq_inflight_bytes(const zlrs_link_t *link)
{
	return link->arq_used != 0U ? link->arq_len : 0U;
}

static void arq_commit_ack(zlrs_link_t *link)
{
	link->queue_head = (uint16_t)((link->queue_head + link->arq_len) % ZLRS_LINK_TX_QUEUE_LEN);
	link->queue_count -= link->arq_len;
	link->arq_used = 0U;
	link->arq_seq = (uint8_t)((link->arq_seq + 1U) & ZLRS_DATA_SEQ_MASK);
	link->arq_retries = 0U;
}

static void queue_copy(const zlrs_link_t *link, uint16_t offset, uint8_t *out, uint16_t len)
{
	const uint16_t start = (link->queue_head + offset) % ZLRS_LINK_TX_QUEUE_LEN;
	const uint16_t contiguous = ZLRS_LINK_TX_QUEUE_LEN - start;
	const uint16_t first = len < contiguous ? len : contiguous;

	memcpy(out, &link->tx_queue[start], first);
	memcpy(out + first, link->tx_queue, len - first);
}

static zlrs_data_frame_t *data_frame(const zlrs_link_t *link, uint8_t seq, zlrs_frame_t *frame)
{
	const int16_t rssi = link->stats.last_rssi;
	const bool ack = link->cfg.arq && receive_ack_bit(link);
	*frame = (zlrs_frame_t){
		.type = link->cfg.role == ZLRS_ROLE_LEADER ? ZLRS_PKT_DATA_L2F : ZLRS_PKT_DATA_F2L,
	};
	zlrs_data_frame_t *const data = &frame->body.data;
	data->data_seq = seq;
	data->ack = ack;
	data->rssi_magnitude = rssi < 0 ? (uint8_t)(-rssi > 255 ? 255 : -rssi) : 0U;
	data->lq_percent = link->stats.lq_percent;
	return data;
}

static const zlrs_radio_req_t *arq_send_slot(zlrs_link_t *link, uint8_t seq, uint16_t len)
{
	zlrs_frame_t frame;
	zlrs_data_frame_t *const data = data_frame(link, seq, &frame);
	queue_copy(link, 0U, data->bytes, len);
	const uint16_t frame_len = link_build_frame(link, &frame, ZLRS_DATA_PREFIX_LEN + len,
						    link_tag_key_for(link, frame.type, true));
	/* Keep bytes queued until ACK, independently of radio completion. */
	link->pending_data_len = 0U;
	link->arq_ack_pending = false;
	return link_submit_tx(link, frame_len, link_slot_freq_hz(link), ZLRS_SYNC_SLOT_DATA,
			      link->sync_word);
}

static bool arq_timed_out(const zlrs_link_t *link, uint32_t now_ms)
{
	const uint16_t timeout_ms =
		link->cfg.arq_timeout_ms > 0U ? link->cfg.arq_timeout_ms : ZLRS_ARQ_TIMEOUT_MS;
	/* An actual ACK timestamp can be just ahead of the planned slot timestamp.
	 * Signed elapsed time handles that ordering and the uint32_t clock wrap. */
	const int32_t age_ms = (int32_t)(now_ms - link->arq_sent_ms);

	return link->arq_used > 0U && age_ms >= (int32_t)timeout_ms;
}

static bool arq_next_data(const zlrs_link_t *link, uint16_t *take)
{
	const uint16_t inflight = arq_inflight_bytes(link);
	if (link->queue_count <= inflight || link->cfg.data_mtu == 0U) {
		return false;
	}
	*take = link->queue_count - inflight;
	if (*take > link->cfg.data_mtu) {
		*take = link->cfg.data_mtu;
	}
	return true;
}

static void arq_commit_data(zlrs_link_t *link, uint16_t len, uint32_t now_ms)
{
	link->arq_len = len;
	link->arq_first_sent_ms = now_ms;
	link->arq_used = 1U;
	link->arq_retries = 0U;
	link->arq_sent_ms = now_ms;
}

const uint8_t *zlrs_link_prepare_data(zlrs_link_t *link, uint8_t slot, uint32_t now_ms,
				      uint16_t *size)
{
	*size = 0U;
	if (link->radio_acquiring) {
		return NULL;
	}
	zlrs_prepared_data_t *packet = &link->prepared_data;
	const uint8_t type =
		link->cfg.role == ZLRS_ROLE_LEADER ? ZLRS_PKT_DATA_L2F : ZLRS_PKT_DATA_F2L;
	const uint32_t frame = link->frame_index + (slot == ZLRS_SLOT_DATA_A ? 1U : 0U);

	/* The TX request may still refer to packet->buf; never overwrite it while on air. */
	if (link->req_pending && link->req_is_tx) {
		return NULL;
	}
	packet->valid = false;
	if (!link->cfg.arq || link->cfg.data_mtu == 0U || link->state != ZLRS_STATE_CONNECTED ||
	    !link->session_valid ||
	    slot != (link->cfg.role == ZLRS_ROLE_LEADER ? ZLRS_SLOT_DATA_A : ZLRS_SLOT_DATA_B) ||
	    link->tx_seq == UINT32_MAX || link->sess_cfm_due ||
	    (link->cfg.resync_every_n > 0U && frame % link->cfg.resync_every_n == 0U)) {
		return NULL;
	}
	packet->retry = arq_timed_out(link, now_ms);
	if (packet->retry) {
		packet->data_len = link->arq_len;
	} else {
		if (link->arq_used != 0U || !arq_next_data(link, &packet->data_len)) {
			return NULL;
		}
	}
	packet->ack_bit = receive_ack_bit(link);
	packet->frame_seq = link->tx_seq;
	packet->data_seq = link->arq_seq;
	const uint16_t offset = packet->retry ? 0U : arq_inflight_bytes(link);

	zlrs_frame_t air_frame;
	zlrs_data_frame_t *const data = data_frame(link, packet->data_seq, &air_frame);
	queue_copy(link, offset, data->bytes, packet->data_len);
	air_frame.seq = packet->frame_seq;
	memcpy(packet->key, link_tag_key_for(link, type, true), ZLRS_KEY_LEN);
	packet->size = link_encode_frame(link, packet->buf, &air_frame,
					 ZLRS_DATA_PREFIX_LEN + packet->data_len, packet->key);
	if (packet->size == 0U) {
		return NULL;
	}
	packet->frame_index = frame;
	packet->data_mtu = link->cfg.data_mtu;
	packet->profile = link->cfg.profile;
	packet->valid = true;
	*size = packet->size;
	return packet->buf;
}

void zlrs_link_discard_prepared_data(zlrs_link_t *link)
{
	link->prepared_data.valid = false;
}

static const zlrs_radio_req_t *send_prepared_data(zlrs_link_t *link, uint32_t now_ms, bool retry)
{
	zlrs_prepared_data_t *packet = &link->prepared_data;
	const uint8_t type =
		link->cfg.role == ZLRS_ROLE_LEADER ? ZLRS_PKT_DATA_L2F : ZLRS_PKT_DATA_F2L;
	const uint16_t inflight = arq_inflight_bytes(link);
	const uint8_t data_seq = link->arq_seq;
	const bool data_valid =
		retry ? link->arq_used > 0U && packet->data_len == link->arq_len
		      : link->arq_used == 0U && link->queue_count >= inflight + packet->data_len;

	if (!packet->valid || packet->retry != retry || !data_valid ||
	    packet->frame_index != link->frame_index || packet->frame_seq != link->tx_seq ||
	    packet->data_seq != data_seq || packet->data_mtu != link->cfg.data_mtu ||
	    packet->profile != link->cfg.profile || packet->buf[0] != type ||
	    !zlrs_ct_equal(packet->key, link_tag_key_for(link, type, true), ZLRS_KEY_LEN)) {
		packet->valid = false;
		return NULL;
	}
	const zlrs_radio_req_t *req = link_submit_tx(link, packet->size, link_slot_freq_hz(link),
						     ZLRS_SYNC_SLOT_DATA, link->sync_word);

	if (req == NULL) {
		return NULL;
	}
	link->req.buf = packet->buf;
	link->tx_seq++;
	if (!retry) {
		arq_commit_data(link, packet->data_len, now_ms);
	}
	link->pending_data_len = 0U;
	link->stats.tx_staged_stale_ack_frames += packet->ack_bit != receive_ack_bit(link);
	if (packet->ack_bit == receive_ack_bit(link)) {
		link->arq_ack_pending = false;
	}
	packet->valid = false;
	return req;
}

static const zlrs_radio_req_t *send_data_arq(zlrs_link_t *link, uint32_t now_ms)
{
	if (arq_timed_out(link, now_ms)) {
		if (link->arq_retries >= ZLRS_ARQ_MAX_RETRIES) {
			/* Keep retrying the same packet; neither side discards stream progress. */
			link->arq_retries = 0U;
			link->stats.tx_arq_retry_runs++;
		}

		link->arq_retries++;
		link->stats.tx_retries++;
		link->stats.tx_retry_age_ms_last = now_ms - link->arq_sent_ms;
		if (link->stats.tx_retry_age_ms_last > link->stats.tx_retry_age_ms_max) {
			link->stats.tx_retry_age_ms_max = link->stats.tx_retry_age_ms_last;
		}
		link->arq_sent_ms = now_ms;
		const zlrs_radio_req_t *req = send_prepared_data(link, now_ms, true);

		if (req != NULL) {
			return req;
		}

		/* Retransmit the head frame: same sequence number and payload as the original */
		return arq_send_slot(link, link->arq_seq, link->arq_len);
	}

	const zlrs_radio_req_t *req = send_prepared_data(link, now_ms, false);
	uint16_t take;

	if (req == NULL && link->arq_used != 0U) {
		link->stats.tx_arq_wait_stalls++;
	}
	if (req != NULL || link->arq_used != 0U || !arq_next_data(link, &take)) {
		return req;
	}
	req = arq_send_slot(link, link->arq_seq, take);
	if (req != NULL) {
		arq_commit_data(link, take, now_ms);
	}
	return req;
}

const zlrs_radio_req_t *link_send_empty_data(zlrs_link_t *link)
{
	zlrs_frame_t frame;
	data_frame(link, link->arq_seq, &frame);
	const uint16_t len = link_build_frame(link, &frame, ZLRS_DATA_PREFIX_LEN,
					      link_tag_key_for(link, frame.type, true));
	if (len == 0U) {
		return NULL;
	}
	link->arq_ack_pending = false;
	link->pending_data_len = 0U;
	link->last_feedback_tx_ms = link->slot_ms;
	return link_submit_tx(link, len, link_slot_freq_hz(link), ZLRS_SYNC_SLOT_DATA,
			      link->sync_word);
}

static void handle_ack_bit(zlrs_link_t *link, bool bit, uint32_t now_ms)
{
	if (link->arq_used == 0U || bit != (((link->arq_seq + 1U) & 1U) != 0U)) {
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
	if (link->cfg.arq) {
		return send_data_arq(link, now_ms);
	}
	uint16_t take = link->queue_count;
	if (take == 0U) {
		return NULL;
	}
	if (take > link->cfg.data_mtu) {
		take = link->cfg.data_mtu;
	}
	zlrs_frame_t frame;
	zlrs_data_frame_t *const data = data_frame(link, 0U, &frame);
	queue_copy(link, 0U, data->bytes, take);
	const uint16_t len = link_build_frame(link, &frame, ZLRS_DATA_PREFIX_LEN + take,
					      link_tag_key_for(link, frame.type, true));
	const zlrs_radio_req_t *req = link_submit_tx(link, len, link_slot_freq_hz(link),
						     ZLRS_SYNC_SLOT_DATA, link->sync_word);
	link->pending_data_len = req != NULL ? take : 0U;
	return req;
}

static bool deliver_data(zlrs_link_t *link, const zlrs_data_frame_t *data, uint16_t len)
{
	return link->ev.on_data != NULL && len != 0U &&
	       link->ev.on_data(link->ev.ctx, data->bytes, len);
}

void link_handle_data(zlrs_link_t *link, const zlrs_frame_t *frame, uint16_t data_len,
		      uint32_t now_ms)
{
	const zlrs_data_frame_t *const data = &frame->body.data;
	if (link->cfg.arq) {
		handle_ack_bit(link, data->ack != 0U, now_ms);
	}
	if (data->rssi_magnitude != 0U && data->lq_percent <= 100U) {
		link->peer_rssi_dbm = -(int16_t)data->rssi_magnitude;
		link->peer_lq_percent = data->lq_percent;
		link->last_peer_feedback_ms = now_ms;
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
		if (deliver_data(link, data, data_len)) {
			link->arq_rx_started = true;
			link->arq_rx_seq = seq;
		}
		return;
	}
	if (!deliver_data(link, data, data_len)) {
		link->stats.data_drop_bytes += data_len;
	}
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
		link->stats.lq_rx = 0U;
		link->stats.lq_expected = 0U;
	} else {
		link->stats.lq_expected = (uint16_t)expected;
	}
}

uint16_t zlrs_link_tx_space(const zlrs_link_t *link)
{
	return (uint16_t)(ZLRS_LINK_TX_QUEUE_LEN - link->queue_count);
}

uint16_t zlrs_link_write(zlrs_link_t *link, const uint8_t *data, uint16_t len)
{
	if (data == NULL) {
		return 0U;
	}
	const uint16_t room = zlrs_link_tx_space(link);
	const uint16_t written = len < room ? len : room;
	const uint16_t tail = (link->queue_head + link->queue_count) % ZLRS_LINK_TX_QUEUE_LEN;
	const uint16_t contiguous = ZLRS_LINK_TX_QUEUE_LEN - tail;
	const uint16_t first = written < contiguous ? written : contiguous;

	memcpy(&link->tx_queue[tail], data, first);
	memcpy(link->tx_queue, data + first, written - first);
	link->queue_count += written;

	if (written < len) {
		link->stats.data_drop_bytes =
			(uint32_t)(link->stats.data_drop_bytes + (len - written));
	}

	return written;
}

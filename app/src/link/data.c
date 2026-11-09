/* SPDX-License-Identifier: Apache-2.0 */
#include "link/data.h"
#include "link/link.h"
#include <string.h>
#include <zephyr/sys/__assert.h>

/* Single-owner packet FIFO and direct payload admission. */

static uint16_t packet_stride(uint16_t mtu)
{
	return (uint16_t)((ZLRS_DATA_HEADROOM + mtu + ZLRS_TAG_LEN + 3U) & ~3U);
}

static uint16_t packet_limit(uint16_t mtu)
{
	if (mtu == 0U || mtu > ZLRS_DATA_MAX_LEN) {
		return 0U;
	}
	const uint16_t count = ZLRS_LINK_TX_STORAGE_LEN / packet_stride(mtu);
	return count < ZLRS_LINK_TX_PACKETS ? count : ZLRS_LINK_TX_PACKETS;
}

static uint16_t packet_index(const zlrs_link_t *link, uint16_t offset)
{
	return (uint16_t)((link->queue_head + offset) % packet_limit(link->tx_pool_mtu));
}

static uint8_t *link_packet_bytes(zlrs_link_t *link, uint16_t index)
{
	return &link->tx_storage[index * packet_stride(link->tx_pool_mtu)];
}

static void link_release_data(zlrs_link_t *link)
{
	__ASSERT_NO_MSG(link->tx_packet_count != 0U);
	zlrs_tx_packet_t *packet = &link->tx_packets[link->queue_head];
	link->status.queue_count -= packet->len;
	packet->len = 0U;
	packet->sealed = false;
	link->queue_head = packet_index(link, 1U);
	link->tx_packet_count--;
}

uint16_t zlrs_link_tx_space(const zlrs_link_t *link)
{
	const uint16_t mtu = link->status.cfg.data_mtu;
	const uint16_t limit = packet_limit(mtu);
	if (limit == 0U || (link->tx_packet_count && link->tx_pool_mtu != mtu)) {
		return 0U;
	}
	uint32_t space = (limit - link->tx_packet_count) * mtu;
	if (link->tx_packet_count != 0U) {
		const zlrs_tx_packet_t *tail =
			&link->tx_packets[packet_index(link, link->tx_packet_count - 1U)];
		if (!tail->sealed) {
			space += mtu - tail->len;
		}
	}
	const uint16_t budget = ZLRS_LINK_TX_QUEUE_LEN - link->status.queue_count;
	return space < budget ? (uint16_t)space : budget;
}

uint8_t *zlrs_link_tx_reserve(zlrs_link_t *link, uint16_t *capacity)
{
	*capacity = 0U;
	__ASSERT_NO_MSG(link->tx_claim_len == 0U);
	const uint16_t space = zlrs_link_tx_space(link);
	if (space == 0U) {
		return NULL;
	}
	if (link->tx_packet_count == 0U) {
		/* Empty pools can adopt a different MTU (diagnostics/tests). */
		link->tx_pool_mtu = link->status.cfg.data_mtu;
		link->queue_head %= packet_limit(link->tx_pool_mtu);
	}
	uint16_t index = packet_index(link, link->tx_packet_count);
	if (link->tx_packet_count != 0U) {
		const uint16_t tail = packet_index(link, link->tx_packet_count - 1U);
		if (!link->tx_packets[tail].sealed &&
		    link->tx_packets[tail].len < link->status.cfg.data_mtu) {
			index = tail;
		}
	}
	zlrs_tx_packet_t *packet = &link->tx_packets[index];
	const uint16_t room = link->status.cfg.data_mtu - packet->len;
	link->tx_claim_len = room < space ? room : space;
	link->tx_claim_index = index;
	*capacity = link->tx_claim_len;
	return link_packet_bytes(link, index) + ZLRS_DATA_HEADROOM + packet->len;
}

void zlrs_link_tx_commit(zlrs_link_t *link, uint16_t written)
{
	__ASSERT_NO_MSG(written <= link->tx_claim_len);
	if (written != 0U) {
		zlrs_tx_packet_t *packet = &link->tx_packets[link->tx_claim_index];
		if (packet->len == 0U) {
			link->tx_packet_count++;
		}
		packet->len += written;
		link->status.queue_count += written;
	}
	link->tx_claim_len = 0U;
}

/* Packet storage owns user bytes from admission until ACK (or best-effort TX).
 * Headers and tags are rebuilt in place; retrying never copies the payload. */

void link_complete_data(zlrs_link_t *link, bool success, uint32_t now_ms)
{
	/* TX bytes and configuration remain fixed until this completion. */
	if ((link->req.buf[0] & ZLRS_FRAME_TYPE_MASK) > ZLRS_PKT_DATA_F2L) {
		return;
	}
	const uint16_t data_len = link->req.size - ZLRS_DATA_HEADROOM -
				  zlrs_profile_tag_len(link->status.cfg.profile);
	if (!success) {
		if (link->status.cfg.arq && data_len != 0U) {
			link->arq_local_failed = true;
		}
		return;
	}
	link->status.arq_ack_pending = false;
	link->sess_ack_due = false;
	link->last_feedback_tx_ms = now_ms;
	if (data_len == 0U) {
		link->status.stats.tx_ack_only_frames++;
	} else {
		link->status.stats.tx_data_frames++;
		link->status.stats.tx_data_ack_frames += link->status.cfg.arq != 0U;
		link->status.stats.tx_data_bytes += data_len;
		link->status.stats.tx_full_data_frames += data_len == link->status.cfg.data_mtu;
	}
	if (!link->status.cfg.arq && data_len != 0U) {
		link_release_data(link);
	}
}

static void arq_commit_ack(zlrs_link_t *link)
{
	link_release_data(link);
	link->status.arq_used = 0U;
	link->arq_launched = false;
	link->arq_local_failed = false;
	link->status.arq_seq = (uint8_t)((link->status.arq_seq + 1U) & ZLRS_DATA_SEQ_MASK);
	link->arq_retries = 0U;
}

/* Payload has already been written at HEADROOM. Only mutable wire fields are
 * touched here. The complete frame remains frozen until radio completion. */
static uint16_t encode_data(zlrs_link_t *link, uint8_t *buf, uint16_t len, uint8_t seq)
{
	const int16_t rssi = link->status.stats.last_rssi;
	zlrs_frame_t frame = {
		.type = link->status.cfg.role == ZLRS_ROLE_LEADER ? ZLRS_PKT_DATA_L2F
								  : ZLRS_PKT_DATA_F2L,
		.body.data.header =
			{
				.data_seq = seq,
				.ack = link->status.cfg.arq && link->arq_rx_started &&
				       ((link->status.arq_rx_seq + 1U) & 1U) != 0U,
				.rssi_magnitude =
					rssi < 0 ? (uint8_t)(-rssi > 255 ? 255 : -rssi) : 0U,
				.lq_percent = link->status.stats.lq_percent,
				.lq_toggle = link->status.stats.lq_generation & 1U,
			},
		.seq = link->tx_seq,
	};
	return link_build_frame(link, &frame, buf, len);
}

bool link_arq_retry_due(const zlrs_link_t *link, uint32_t now_ms)
{
	const uint16_t timeout = link->status.cfg.arq_timeout_ms > 0U
					 ? link->status.cfg.arq_timeout_ms
					 : ZLRS_ARQ_TIMEOUT_MS;
	return link->status.arq_used != 0U &&
	       (link->arq_local_failed ||
		(link->arq_launched && (int32_t)(now_ms - link->arq_sent_ms) >= timeout));
}

uint8_t *link_build_data(zlrs_link_t *link, uint16_t data_len, uint16_t *size)
{
	uint8_t *buf = link->tx_buf;
	if (data_len != 0U) {
		if (link->tx_packet_count == 0U ||
		    data_len != link->tx_packets[link->queue_head].len) {
			return NULL;
		}
		if (link->status.cfg.arq && link->status.arq_used != 0U) {
			if (link->arq_retries >= ZLRS_ARQ_MAX_RETRIES) {
				link->arq_retries = 0U;
				link->status.stats.tx_arq_retry_runs++;
			}
			link->arq_retries++;
			link->status.stats.tx_retries++;
			zlrs_timing_record(&link->status.stats.tx_retry_age_ms,
					   link->arq_launched ? link->slot_ms - link->arq_sent_ms
							      : 0U);
		}
		buf = link_packet_bytes(link, link->queue_head);
	}
	*size = encode_data(link, buf, data_len,
			    link->status.cfg.arq || data_len == 0U ? link->status.arq_seq : 0U);
	if (*size == 0U) {
		return NULL;
	}
	if (data_len != 0U) {
		link->tx_packets[link->queue_head].sealed = true;
		link->arq_local_failed = false;
		if (link->status.cfg.arq && link->status.arq_used == 0U) {
			link->arq_launched = false;
			link->status.arq_used = 1U;
			link->arq_retries = 0U;
		}
	}
	return buf;
}

static void handle_ack_bit(zlrs_link_t *link, bool bit, uint32_t now_ms)
{
	if (link->status.arq_used == 0U || !link->arq_launched ||
	    bit != (((link->status.arq_seq + 1U) & 1U) != 0U)) {
		return;
	}
	const int32_t elapsed = (int32_t)(now_ms - link->arq_first_sent_ms);
	const uint32_t delay = elapsed > 0 ? (uint32_t)elapsed : 0U;
	zlrs_timing_record(&link->status.stats.tx_ack_delay_ms, delay);
	link->status.stats.tx_ack_delay_ms_sum += delay;
	link->status.stats.tx_ack_entries++;
	arq_commit_ack(link);
	link->arq_sent_ms = now_ms;
}

static bool deliver_data(zlrs_link_t *link, const uint8_t *bytes, uint16_t len)
{
	return link->ev.on_data != NULL && len != 0U && link->ev.on_data(link->ev.ctx, bytes, len);
}

void link_handle_data(zlrs_link_t *link, const zlrs_data_view_t *view, uint32_t now_ms)
{
	const zlrs_data_header_t *const data = &view->header;
	const uint16_t data_len = view->len;
	if (link->status.cfg.arq) {
		handle_ack_bit(link, data->ack != 0U, now_ms);
	}
	if (data->rssi_magnitude != 0U) {
		const uint32_t gap_ms = link->status.cfg.lq_feedback_gap_ms != 0U
						? link->status.cfg.lq_feedback_gap_ms
						: 1000U;
		const bool baseline =
			link->status.peer_lq_percent == ZLRS_LQ_UNAVAILABLE ||
			(uint32_t)(now_ms - link->status.last_peer_feedback_ms) >= gap_ms;
		const uint8_t quality = data->lq_percent;
		const bool toggle = data->lq_toggle != 0U;

		link->status.peer_rssi_dbm = -(int16_t)data->rssi_magnitude;
		link->status.last_peer_feedback_ms = now_ms;
		if (quality <= 100U && (baseline || toggle != link->peer_lq_toggle)) {
			link->status.peer_lq_percent = quality;
			link->peer_lq_toggle = toggle;
			/* A skipped RAM generation denotes an uncertain baseline, not a new window.
			 */
			link->status.peer_lq_generation +=
				baseline && link->status.peer_lq_generation != 0U ? 2U : 1U;
		} else if (quality > 100U) {
			link->status.peer_lq_percent = ZLRS_LQ_UNAVAILABLE;
		}
	}
	if (data_len == 0U) {
		return;
	}
	if (link->status.cfg.arq) {
		const uint8_t seq = data->data_seq;
		const uint8_t next = link->arq_rx_started
					     ? (link->status.arq_rx_seq + 1U) & ZLRS_DATA_SEQ_MASK
					     : 0U;
		link->status.arq_ack_pending = true;
		if (seq != next) {
			if (link->arq_rx_started && seq == link->status.arq_rx_seq) {
				link->status.stats.rx_dup++;
			} else {
				link->status.stats.rx_out_of_order++;
			}
			return;
		}
		if (deliver_data(link, view->bytes, data_len)) {
			link->arq_rx_started = true;
			link->status.arq_rx_seq = seq;
		}
		return;
	}
	if (!deliver_data(link, view->bytes, data_len)) {
		link->status.stats.data_drop_bytes += data_len;
	}
}

void link_quality_reset(zlrs_link_t *link)
{
	link->lq_last_seq = 0U;
	link->status.stats.lq_rx = 0U;
	link->status.stats.lq_expected = 0U;
	link->status.stats.lq_percent = ZLRS_LQ_UNAVAILABLE;
	link->status.stats.lq_generation = 0U;
	link->status.stats.last_rssi = 0;
	link->status.peer_rssi_dbm = 0;
	link->status.peer_lq_percent = ZLRS_LQ_UNAVAILABLE;
	link->status.peer_lq_generation = 0U;
	link->peer_lq_toggle = false;
	link->status.last_peer_feedback_ms = 0U;
}

void link_lq_account(zlrs_link_t *link, uint32_t seq)
{
	uint32_t delta = seq - link->lq_last_seq;

	if (link->lq_last_seq == 0U) {
		/* Only the first sample of a session has no preceding sequence to compare. */
		delta = 1U;
	}

	link->lq_last_seq = seq;
	link->status.stats.lq_rx++;
	const uint64_t expected = (uint64_t)link->status.stats.lq_expected + delta;

	if (expected >= ZLRS_LQ_WINDOW_FRAMES) {
		link->status.stats.lq_percent =
			(uint8_t)(((uint64_t)link->status.stats.lq_rx * 100U) / expected);
		link->status.stats.lq_generation++;
		link->status.stats.lq_rx = 0U;
		link->status.stats.lq_expected = 0U;
	} else {
		link->status.stats.lq_expected = (uint16_t)expected;
	}
}

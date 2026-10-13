/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* Link lifecycle, slot scheduling, frame authentication and radio completion. */

bool link_new_nonce(const zlrs_link_t *link, uint32_t previous, bool have_previous,
		    uint32_t *out)
{
	if (link->random == NULL) {
		return false;
	}
	for (unsigned int i = 0U; i < 4U; i++) {
		uint32_t nonce;
		if (!link->random(link->random_ctx, &nonce)) {
			return false;
		}
		if (!have_previous || nonce != previous) {
			*out = nonce;
			return true;
		}
	}
	return false;
}

uint32_t link_sync_freq_hz(const zlrs_link_t *link)
{
	const zlrs_fhss_domain_t *domain = &zlrs_fhss_domains[link->cfg.domain];
	const uint8_t channel = (uint8_t)(domain->channel_count / 2U);

	return domain->start_hz + (uint32_t)channel * domain->step_hz;
}

static bool sync_frame(const zlrs_link_t *link)
{
	return link->cfg.resync_every_n > 0U &&
	       (link->frame_index % link->cfg.resync_every_n) == 0U;
}

uint32_t link_slot_freq_hz(const zlrs_link_t *link)
{
	if (sync_frame(link) || link->sess_cfm_due || link->sess_ack_due) {
		return link_sync_freq_hz(link);
	}
	/* Both ends count from the confirmed frame, including repeated confirms after lost ACKs.
	 * Keep initial DATA on the same channel where the peer is waiting for confirmation. */
	if (link->session_valid &&
	    (uint32_t)(link->frame_index - link->sess_sync_frame) < ZLRS_DATA_CONFIRM_FRAMES) {
		return link_sync_freq_hz(link);
	}
	/* Both ends keep the fresh-session uplink on the confirmation channel. */
	if ((link->slot == ZLRS_SLOT_DATA_B) &&
	    (uint32_t)(link->slot_ms - link->state_ms) < ZLRS_SESS_CONFIRM_MS) {
		return link_sync_freq_hz(link);
	}

	return zlrs_fhss_current_hz(&link->fhss);
}

void link_set_state(zlrs_link_t *link, zlrs_link_state_t state, uint32_t now_ms)
{
	if (link->state == state) {
		return;
	}

	link->state = state;
	link->state_ms = now_ms;
	if (link->ev.on_state != NULL) {
		link->ev.on_state(link->ev.ctx, state);
	}

	/* The TX is the authority: every return to SEARCHING rolls a new session nonce */
	if (state == ZLRS_STATE_SEARCHING && link->cfg.role == ZLRS_ROLE_TX) {
		link_roll_session(link, now_ms);
		/* Send a SYNC right away instead of waiting a full period, to shorten the reconnect */
		link->last_sync_ms = (uint32_t)(now_ms - link->cfg.sync_period_ms);
	}
}

uint16_t link_encode_frame(const zlrs_link_t *link, uint8_t *buf,
			   const zlrs_frame_t *frame, uint16_t payload_len, const uint8_t *tag_key)
{
	if (link->cfg.profile > ZLRS_PROFILE_P1 ||
	    payload_len > ZLRS_PAYLOAD_MAX_LEN - zlrs_profile_tag_len(link->cfg.profile)) {
		return 0U;
	}
	const uint8_t tag_len = zlrs_profile_tag_len(link->cfg.profile);
	const uint16_t len = zlrs_frame_len(payload_len, tag_len);
	const uint16_t total = zlrs_frame_encode(frame, buf, ZLRS_FRAME_MAX_LEN, len, tag_len);
	if (total != 0U && tag_len != 0U) {
		const uint16_t mac_len = total - tag_len;
		zlrs_tag(tag_key, ZLRS_KEY_LEN, buf, mac_len, buf + mac_len);
	}
	return total;
}

uint16_t link_build_frame(zlrs_link_t *link, zlrs_frame_t *frame,
			  uint16_t payload_len, const uint8_t *tag_key)
{
	link->prepared_data.valid = false;
	uint32_t *const counter = frame->type >= ZLRS_PKT_PAIR_REQ &&
		frame->type <= ZLRS_PKT_PAIR_CFM ? &link->pair_tx_seq : &link->tx_seq;
	if (*counter == 0U || *counter == UINT32_MAX) {
		return 0U;
	}
	frame->seq = *counter;
	const uint16_t total = link_encode_frame(link, link->tx_buf, frame, payload_len, tag_key);
	if (total > 0U) {
		(*counter)++;
	}
	return total;
}

const uint8_t *link_tag_key_for(const zlrs_link_t *link, uint8_t type, bool outbound)
{
	const uint8_t *link_auth = outbound ? link->link_auth_out : link->link_auth_in;
	const uint8_t *session = outbound ? link->session_key_out : link->session_key_in;

	switch (type) {
	case ZLRS_PKT_PAIR_REQ:
	case ZLRS_PKT_PAIR_RESP:
	case ZLRS_PKT_PAIR_CFM:
		return link->auth_key;
	case ZLRS_PKT_SYNC:
	/* SESS_RESP must verify before the session key exists, so it uses the pairing-level key */
	case ZLRS_PKT_SESS_RESP:
		return link_auth;
	/* The confirm frame must be tagged with the very session key it confirms */
	case ZLRS_PKT_SESS_CFM:
		return session;
	default:
		return link->session_valid ? session : link_auth;
	}
}

static const zlrs_radio_req_t *make_req(zlrs_link_t *link, bool is_tx, uint16_t size,
					uint32_t freq_hz, uint8_t sync_slot,
					const uint8_t sync_word[ZLRS_SYNC_WORD_LEN],
					uint16_t timeout_ms)
{
	if (link->req_pending) {
		return NULL;
	}

	link->req.freq_hz = freq_hz;
	link->req.mod = link->cfg.mod;
	link->req.sync_slot = sync_slot;
	memcpy(link->req.sync_word, sync_word, ZLRS_SYNC_WORD_LEN);
	link->req.is_tx = is_tx;
	link->req.tx_power_dbm = link->cfg.tx_power_dbm;
	link->req.size = size;
	link->req.max_rx_size = is_tx ? 0U : ZLRS_FRAME_MAX_LEN;
	link->req.timeout_ms = timeout_ms;
	link->req.buf = is_tx ? link->tx_buf : link->rx_buf;
	link->req_is_tx = is_tx;
	link->req_pending = true;

	return &link->req;
}

const zlrs_radio_req_t *link_submit_tx(zlrs_link_t *link, uint16_t len, uint32_t freq_hz,
				       uint8_t sync_slot,
				       const uint8_t sync_word[ZLRS_SYNC_WORD_LEN])
{
	if (len == 0U) {
		return NULL;
	}

	return make_req(link, true, len, freq_hz, sync_slot, sync_word, 0U);
}

const zlrs_radio_req_t *link_submit_rx(zlrs_link_t *link, uint32_t freq_hz, uint8_t sync_slot,
				       const uint8_t sync_word[ZLRS_SYNC_WORD_LEN],
				       uint16_t timeout_ms)
{
	return make_req(link, false, 0U, freq_hz, sync_slot, sync_word, timeout_ms);
}

uint16_t link_acq_timeout_ms(const zlrs_link_t *link)
{
	return (link->cfg.acq_timeout_ms != 0U) ? link->cfg.acq_timeout_ms
						: link->cfg.rx_timeout_ms;
}

static int handle_rx(zlrs_link_t *link, uint16_t len, uint32_t now_ms)
{
	const uint8_t tag_len = zlrs_profile_tag_len(link->cfg.profile);
	zlrs_frame_t frame;
	const uint8_t *key;

	if (!zlrs_frame_decode(&frame, link->rx_buf, len, tag_len)) {
		link->stats.rx_bad_frame++;
		return -1;
	}

	/* The confirm verifies with the candidate key first, not the state-based path below */
	if (frame.type == ZLRS_PKT_SESS_CFM) {
		return link_handle_sess_cfm(link, &frame, len, now_ms);
	}

	key = link_tag_key_for(link, frame.type, false);
	if (tag_len > 0U) {
		uint8_t expect[ZLRS_TAG_LEN];

		zlrs_tag(key, ZLRS_KEY_LEN, link->rx_buf, len - tag_len, expect);
		if (!zlrs_ct_equal(expect, frame.tag, ZLRS_TAG_LEN)) {
			link->stats.rx_bad_tag++;
			return -1;
		}
	}

	link->stats.rx_frames++;
	link->stats.rx_bytes = (uint32_t)(link->stats.rx_bytes + len);

	if (frame.type == ZLRS_PKT_PAIR_REQ || frame.type == ZLRS_PKT_PAIR_RESP ||
	    frame.type == ZLRS_PKT_PAIR_CFM) {
		/*
		 * SEARCHING as well: the responder repeats its answer until the confirmation arrives, and
		 * an initiator that already moved on has to be able to answer that repeat with another
		 * confirmation (the handshake has no other acknowledgement).
		 */
		if (link->state != ZLRS_STATE_PAIRING && link->state != ZLRS_STATE_SEARCHING) {
			return -1;
		}
		return link_handle_pair_frame(link, &frame, now_ms) ? 0 : -1;
	}

	if (frame.type == ZLRS_PKT_SYNC) {
		link_handle_sync(link, &frame, now_ms);
		return 0;
	}

	if (frame.type == ZLRS_PKT_SESS_RESP) {
		return link_handle_sess_resp(link, &frame, now_ms) ? 0 : -1;
	}

	if (link->state != ZLRS_STATE_CONNECTED || !link->session_valid ||
	    frame.type != (link->cfg.role == ZLRS_ROLE_TX ? ZLRS_PKT_DATA_DOWN : ZLRS_PKT_DATA_UP)) {
		return -1;
	}

	/* Replay protection: sequence numbers must strictly increase within a session */
	if (frame.seq <= link->last_rx_seq) {
		link->stats.rx_dup++;
		return -1;
	}
	link->last_rx_seq = frame.seq;
	link->last_rx_ms = now_ms;
	link->last_rx_frame = link->frame_index;
	if (link->cfg.role == ZLRS_ROLE_TX) {
		link->sess_cfm_due = false;
	}
	link_lq_account(link, frame.seq);

	switch (frame.type) {
	case ZLRS_PKT_DATA_UP:
	case ZLRS_PKT_DATA_DOWN:
		link_handle_data(link, &frame,
				 len - ZLRS_FRAME_HDR_LEN - tag_len - ZLRS_DATA_PREFIX_LEN, now_ms);
		break;
	default:
		break;
	}

	return 0;
}

static void advance_hop(zlrs_link_t *link)
{
	if (link->cfg.hopping == 0U) {
		/* Pinned to the sync channel (entry 0 of the hop table) */
		return;
	}

	if (link->state == ZLRS_STATE_CONNECTED) {
		link->stats.hop_advances++;
		(void)zlrs_fhss_next_hz(&link->fhss);
	}
}

static const zlrs_radio_req_t *data_slot(zlrs_link_t *link, uint8_t slot, uint32_t now_ms)
{
	const bool a = (slot == ZLRS_SLOT_DATA_A);
	const bool tx_side = (link->cfg.role == ZLRS_ROLE_TX);

	/* Pairing uses the data slots; repeat confirms until the responder hears one. */
	if (link->state == ZLRS_STATE_PAIRING ||
	    (link->state == ZLRS_STATE_SEARCHING && link->pair_cfm_pending)) {
		return link_pair_slot(link, a, now_ms);
	}

	/* The main slot (A) counts once per frame */
	if (a) {
		link->frame_index++;
	}

	if (link->state == ZLRS_STATE_SEARCHING) {
		if (tx_side) {
			if (!link->session_valid && !link->session_pending) {
				link_roll_session(link, now_ms);
				if (!link->session_pending) {
					return NULL;
				}
			}
			if (a) {
				if (link->sess_cfm_due) {
					/* The responder may have rebooted while this confirm was pending. */
					if ((uint32_t)(now_ms - link->last_sync_ms) >=
					    link->cfg.sync_period_ms) {
						return link_send_sync(link, now_ms);
					}
					/*
					 * Repeat the confirm (the peer leaves SEARCHING on one of them) and only then count the
					 * session as established.
					 */
					const zlrs_radio_req_t *req = link_send_sess_cfm(link);

					if (req != NULL) {
						link_set_state(link, ZLRS_STATE_CONNECTED, now_ms);
					}
					return req;
				}
				/*
				 * Send when the period is up, and add one on every resync frame: a peer that is still
				 * CONNECTED only opens a window on the sync channel for those, so otherwise it would never
				 * hear that we changed sessions.
				 */
				if ((uint32_t)(now_ms - link->last_sync_ms) <
					    link->cfg.sync_period_ms &&
				    !sync_frame(link)) {
					return NULL;
				}
				return link_send_sync(link, now_ms);
			}
			/* The initiator waits for the peer's first frame in the uplink slot */
			return link_submit_rx(link, link_sync_freq_hz(link), ZLRS_SYNC_SLOT_DATA,
					      link->sync_word, link_acq_timeout_ms(link));
		}
		if (a) {
			return link_submit_rx(link, link_sync_freq_hz(link), ZLRS_SYNC_SLOT_DATA,
					      link->sync_word, link_acq_timeout_ms(link));
		}
		/* Keep answering until the confirm arrives. */
		if (link->sess_resp_due || link->session_pending) {
			return link_send_sess_resp(link);
		}
		return NULL;
	}

	if (link->state != ZLRS_STATE_CONNECTED) {
		return NULL;
	}

	advance_hop(link);

	if (link->sess_ack_due && tx_side == a) {
		const zlrs_radio_req_t *req = link_send_empty_data(link);

		if (req != NULL) {
			link->sess_ack_due = false;
		}
		return req;
	}

	/* Legal idle DATA can stay silent until the next periodic SYNC. Only reacquire
	 * after two missed SYNC periods; the SEARCHING repeat interval is unrelated. */
	if (!tx_side && a && link->cfg.resync_every_n > 0U &&
	    (uint32_t)(link->frame_index - link->last_rx_frame) > 2U * link->cfg.resync_every_n) {
		return link_submit_rx(link, link_sync_freq_hz(link), ZLRS_SYNC_SLOT_DATA,
				      link->sync_word, link->cfg.rx_timeout_ms);
	}

	if (tx_side && a) {
		/* Session confirm first, resync SYNC second (both use the sync channel) */
		if (link->sess_cfm_due) {
			if ((uint32_t)(now_ms - link->last_sync_ms) >= link->cfg.sync_period_ms) {
				return link_send_sync(link, now_ms);
			}
			return link_send_sess_cfm(link);
		}
		if (sync_frame(link)) {
			return link_send_sync(link, now_ms);
		}
	}

	if (tx_side == a) {
		const zlrs_radio_req_t *req;

		req = link_send_data(link, now_ms);

		if (req != NULL) {
			return req;
		}
		/* Report the receive toggle in an empty DATA when no user DATA can be sent. */
		if (link->cfg.arq &&
		    (link->arq_ack_pending || (link->cfg.feedback_period_ms > 0U &&
					       (uint32_t)(now_ms - link->last_feedback_tx_ms) >=
						       link->cfg.feedback_period_ms))) {
			return link_send_empty_data(link);
		}

		/* An idle transmit slot stays idle; receiving the opposite slot loses hop/phase alignment. */
		return NULL;
	}

	/*
	 * Fresh session on the data link as well: keep a window on the sync channel where a peer that
	 * missed the confirm is still repeating its answer.
	 */
	if (tx_side && (uint32_t)(now_ms - link->state_ms) < ZLRS_SESS_CONFIRM_MS) {
		return link_submit_rx(link, link_sync_freq_hz(link), ZLRS_SYNC_SLOT_DATA,
				      link->sync_word, link->cfg.rx_timeout_ms);
	}

	return link_submit_rx(link, link_slot_freq_hz(link), ZLRS_SYNC_SLOT_DATA, link->sync_word,
			      link->cfg.rx_timeout_ms);
}

static void limit_data_mtu(zlrs_link_t *link)
{
	const uint16_t mtu = ZLRS_DATA_MAX_LEN - zlrs_profile_tag_len(link->cfg.profile);

	if (link->cfg.data_mtu > mtu) {
		link->cfg.data_mtu = mtu;
	}
}

void zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg,
		    bool (*random)(void *ctx, uint32_t *out),
		    void *random_ctx, const zlrs_link_events_t *events)
{
	memset(link, 0, sizeof(*link));
	link->cfg = *cfg;
	limit_data_mtu(link);
	link->random = random;
	link->random_ctx = random_ctx;
	if (events != NULL) {
		link->ev = *events;
	}

	link->tx_seq = 1U;
	link->pair_tx_seq = 1U;
	link->stats.lq_percent = 100U;
	link->state = ZLRS_STATE_UNCONFIGURED;
}

void zlrs_link_set_power(zlrs_link_t *link, int8_t tx_power_dbm)
{
	link->cfg.tx_power_dbm = tx_power_dbm;
}

const zlrs_radio_req_t *zlrs_link_slot(zlrs_link_t *link, uint8_t slot, uint32_t now_ms)
{
	link->slot = slot;
	link->slot_ms = now_ms;
	if (link->tx_seq == UINT32_MAX &&
	    (link->state == ZLRS_STATE_CONNECTED || link->state == ZLRS_STATE_SEARCHING)) {
		/* Rekey before exhausting the counter; keep all queued/unacknowledged bytes. */
		link->session_valid = false;
		link->session_pending = false;
		link->prepared_data.valid = false;
		const bool was_searching = link->state == ZLRS_STATE_SEARCHING;
		link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
		if (was_searching && link->cfg.role == ZLRS_ROLE_TX) {
			link_roll_session(link, now_ms);
		}
		link->last_sync_ms = now_ms - link->cfg.sync_period_ms;
	}

	return data_slot(link, slot, now_ms);
}

void zlrs_link_skip_slot(zlrs_link_t *link, uint8_t slot)
{
	link->prepared_data.valid = false;
	link->slot = slot;
	if (slot == ZLRS_SLOT_DATA_A) {
		link->frame_index++;
	}
	advance_hop(link);
}

void zlrs_link_complete(zlrs_link_t *link, int status, uint16_t rx_len, int16_t rssi, int8_t snr,
			uint32_t now_ms)
{
	const bool was_tx = link->req_is_tx;
	const uint8_t slot = (link->slot < ZLRS_SLOT_COUNT) ? link->slot : 0U;

	link->req_pending = false;

	if (status != ZLRS_RADIO_OK) {
		link->stats.radio_fail++;
		link->stats.slot_fail[slot]++;
		if (was_tx) {
			/* On a failed send, push the bytes we already took back into the queue */
			link->queue_count = (uint16_t)(link->queue_count + link->pending_data_len);
			link->pending_data_len = 0U;
		}
		return;
	}

	if (was_tx) {
		link->stats.slot_tx[slot]++;
		link->stats.tx_frames++;
		link->stats.tx_bytes = (uint32_t)(link->stats.tx_bytes + link->req.size);
		zlrs_frame_t frame;

		if (zlrs_frame_decode(&frame, link->req.buf, link->req.size,
				      zlrs_profile_tag_len(link->cfg.profile)) &&
		    (frame.type == ZLRS_PKT_DATA_UP || frame.type == ZLRS_PKT_DATA_DOWN)) {
			const uint16_t data_len = link->req.size - ZLRS_FRAME_HDR_LEN -
				zlrs_profile_tag_len(link->cfg.profile) - ZLRS_DATA_PREFIX_LEN;
			if (data_len == 0U) {
				link->stats.tx_ack_only_frames++;
			} else {
				link->stats.tx_data_frames++;
				link->stats.tx_data_ack_frames += link->cfg.arq != 0U;
				link->stats.tx_data_bytes += data_len;
				link->stats.tx_full_data_frames += link->req.size == ZLRS_FRAME_MAX_LEN;
			}
		}
		link->queue_head = (uint16_t)((link->queue_head + link->pending_data_len) %
					      ZLRS_LINK_TX_QUEUE_LEN);
		link->queue_count = (uint16_t)(link->queue_count - link->pending_data_len);
		link->pending_data_len = 0U;
		return;
	}

	link->stats.slot_rx[slot]++;
	if (rx_len > 0U) {
		/* A TX completion or empty RX window has no signal measurement. Preserve the
		 * last packet rather than overwriting received signal information. */
		link->stats.last_rssi = rssi;
		link->stats.last_snr = snr;
	}
	if (rx_len > 0U && rssi != 0) {
		if (link->stats.rssi_min == 0 || rssi < link->stats.rssi_min) {
			link->stats.rssi_min = rssi;
		}
		if (link->stats.rssi_max == 0 || rssi > link->stats.rssi_max) {
			link->stats.rssi_max = rssi;
		}
	}
	if (rx_len > 0U) {
		link->stats.slot_hit[slot]++;
		(void)handle_rx(link, rx_len, now_ms);
	}
}

void zlrs_link_rx(zlrs_link_t *link, const uint8_t *buf, uint16_t len, int16_t rssi, int8_t snr,
		  uint32_t now_ms)
{
	if (buf == NULL || len == 0U) {
		return;
	}
	if (len > sizeof(link->rx_buf)) {
		len = sizeof(link->rx_buf);
	}

	memcpy(link->rx_buf, buf, len);
	link->req_pending = false;
	link->stats.last_rssi = rssi;
	link->stats.last_snr = snr;
	(void)handle_rx(link, len, now_ms);
}

void zlrs_link_tick(zlrs_link_t *link, uint32_t now_ms)
{

	switch (link->state) {
	case ZLRS_STATE_UNCONFIGURED:
		if (link->configured) {
			link_set_state(link, ZLRS_STATE_UNPAIRED, now_ms);
		}
		break;

	case ZLRS_STATE_PAIRING:
		if ((uint32_t)(now_ms - link->state_ms) > link->cfg.pair_window_ms) {
			link_set_state(link,
				       link->paired ? ZLRS_STATE_SEARCHING : ZLRS_STATE_UNPAIRED,
				       now_ms);
		}
		break;

	case ZLRS_STATE_CONNECTED:
		/*
		 * Grace after entering CONNECTED: the peer may still be leaving SEARCHING (it needs to
		 * hear one of our confirms), and its first data frame cannot arrive until it does. Running
		 * the peer-loss check inside that window would roll a new session and restart the
		 * handshake before it ever had a chance to finish.
		 */
		if ((uint32_t)(now_ms - link->state_ms) <= ZLRS_SESS_CONFIRM_MS) {
			break;
		}
		if (link->cfg.peer_timeout_ms > 0U &&
		    (uint32_t)(now_ms - link->last_rx_ms) > link->cfg.peer_timeout_ms) {
			link->session_valid = false;
			link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
		}
		break;

	default:
		break;
	}
}

const zlrs_link_cfg_t *zlrs_link_get_config(const zlrs_link_t *link)
{
	return &link->cfg;
}

const zlrs_link_stats_t *zlrs_link_get_stats(const zlrs_link_t *link)
{
	return &link->stats;
}

zlrs_link_state_t zlrs_link_get_state(const zlrs_link_t *link)
{
	return link->state;
}

bool zlrs_link_is_busy(const zlrs_link_t *link)
{
	return link->req_pending;
}

zlrs_link_info_t zlrs_link_get_info(const zlrs_link_t *link)
{
	zlrs_link_info_t info = {
		.state = link->state,
		.configured = link->configured,
		.paired = link->paired,
		.session_valid = link->session_valid,
		.peer_rssi_dbm = link->peer_rssi_dbm,
		.peer_lq_percent = link->peer_lq_percent,
		.last_peer_feedback_ms = link->last_peer_feedback_ms,
	};

	memcpy(info.dev_id, link->dev_id, sizeof(info.dev_id));
	memcpy(info.peer_id, link->peer_id, sizeof(info.peer_id));
	return info;
}

bool zlrs_link_get_pairing(const zlrs_link_t *link, uint8_t peer_id[ZLRS_DEV_ID_LEN],
			   uint8_t pair_salt[ZLRS_PAIR_SALT_LEN])
{
	if (!link->paired) {
		return false;
	}
	memcpy(peer_id, link->peer_id, ZLRS_DEV_ID_LEN);
	memcpy(pair_salt, link->pair_salt, ZLRS_PAIR_SALT_LEN);
	return true;
}

static uint32_t fingerprint(const uint8_t *data, size_t len)
{
	uint32_t hash = 2166136261U;

	for (size_t i = 0U; i < len; i++) {
		hash = (hash ^ data[i]) * 16777619U;
	}
	return hash;
}

zlrs_link_diagnostics_t zlrs_link_get_diagnostics(const zlrs_link_t *link)
{
	return (zlrs_link_diagnostics_t){
		.frequency_hz = link->fhss.domain != NULL ? zlrs_fhss_current_hz(&link->fhss) : 0U,
		.frame_index = link->frame_index,
		.last_rx_frame = link->last_rx_frame,
		.sess_nonce_t = link->sess_nonce_t,
		.sess_nonce_r = link->sess_nonce_r,
		.sess_cfm_due = link->sess_cfm_due,
		.hop_index = link->fhss.index,
		.queue_count = link->queue_count,
		.arq_used = link->arq_used,
		.arq_seq = link->arq_seq,
		.arq_rx_seq = link->arq_rx_seq,
		.arq_ack_pending = link->arq_ack_pending,
		.pair_wait_cfm = link->pair_wait_cfm,
		.pair_cfm_pending = link->pair_cfm_pending,
		.pair_cfm_sent = link->pair_cfm_sent,
		.pair_nonce = link->pair_nonce,
		.pair_nonce_b = link->pair_nonce_b,
		.root_fingerprint = fingerprint(link->root_key, sizeof(link->root_key)),
		.pair_fingerprint = fingerprint(link->pair_key, sizeof(link->pair_key)),
		.sync_fingerprint = fingerprint(link->sync_word, sizeof(link->sync_word)),
	};
}

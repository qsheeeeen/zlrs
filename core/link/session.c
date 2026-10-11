/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* Session key rotation, confirmation, synchronization and replay-safe reacquisition. */
#define LABEL_SESSION "zlrs/session"

void link_roll_session(zlrs_link_t *link, uint32_t now_ms)
{
	link->sess_nonce_t = link_random(link, now_ms);
	link->session_valid = false;
	link->session_pending = false;
	link->sess_cfm_due = false;
	link->sess_resp_due = false;
	link->sess_ack_due = false;
	link->announce = false;
}

void link_derive_by_endpoints(const zlrs_link_t *link, const char *label, const uint8_t *ctx,
			      size_t ctx_len, uint8_t out[ZLRS_KEY_LEN], uint8_t in[ZLRS_KEY_LEN])
{
	uint8_t ids_ctx[2U * sizeof(uint32_t) + 2U * ZLRS_DEV_ID_LEN];
	const size_t len = ctx_len + 2U * ZLRS_DEV_ID_LEN;

	assert(len <= sizeof(ids_ctx));
	if (ctx_len > 0U) {
		memcpy(ids_ctx, ctx, ctx_len);
	}

	/* Outgoing: local id first */
	memcpy(&ids_ctx[ctx_len], link->dev_id, ZLRS_DEV_ID_LEN);
	memcpy(&ids_ctx[ctx_len + ZLRS_DEV_ID_LEN], link->peer_id, ZLRS_DEV_ID_LEN);
	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, label, ids_ctx, len, out, ZLRS_KEY_LEN);

	/* Incoming: peer id first */
	memcpy(&ids_ctx[ctx_len], link->peer_id, ZLRS_DEV_ID_LEN);
	memcpy(&ids_ctx[ctx_len + ZLRS_DEV_ID_LEN], link->dev_id, ZLRS_DEV_ID_LEN);
	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, label, ids_ctx, len, in, ZLRS_KEY_LEN);
}

static void derive_session_keys(zlrs_link_t *link, uint32_t nonce_t, uint32_t nonce_r)
{
	uint8_t ctx[2U * sizeof(uint32_t)];

	wr32(&ctx[0], nonce_t);
	wr32(&ctx[4], nonce_r);
	link_derive_by_endpoints(link, LABEL_SESSION, ctx, sizeof(ctx), link->session_key_out,
				 link->session_key_in);
}

static void session_activate(zlrs_link_t *link, uint16_t hop_index)
{
	link->prepared_data.valid = false;
	link->session_valid = true;
	link->session_pending = false;
	link->sess_resp_due = false;
	link->last_rx_seq = 0xFFFFU;
	link->sess_ack_due = false;
	link->tx_seq = 0U;
	link->sess_sync_frame = link->frame_index;
	/* Sequence numbers restart per session; the LQ window must not span sessions */
	link->lq_last_seq = 0U;
	link->lq_started = false;
	link->stats.lq_rx = 0U;
	link->stats.lq_expected = 0U;
	/* Session reset releases the outstanding packet; its bytes stay queued for retransmission. */
	link->arq_used = 0U;
	link->arq_retries = 0U;
	link->arq_seq = 0U;
	link->arq_rx_started = false;
	link->arq_ack_pending = false;
	zlrs_fhss_set_index(&link->fhss, hop_index);
}

const zlrs_radio_req_t *link_send_sync(zlrs_link_t *link, uint32_t now_ms)
{
	uint8_t payload[12];
	uint16_t len;

	payload[0] = ZLRS_PROTO_VERSION;
	payload[1] = (uint8_t)link->cfg.profile;
	payload[2] = (uint8_t)(link->fhss.index & 0xFFU);
	payload[3] = (uint8_t)(link->fhss.index >> 8);
	wr32(&payload[4], link->sess_nonce_t);
	wr32(&payload[8], link->frame_index);

	len = link_build_frame(link, ZLRS_PKT_SYNC, 0U, payload, sizeof(payload),
			       link_tag_key_for(link, ZLRS_PKT_SYNC, true));
	link->last_sync_ms = now_ms;
	link->stats.tx_syncs++;

	/* SYNC always goes out on the sync channel */
	return link_submit_tx(link, len, link_sync_freq_hz(link), ZLRS_SYNC_SLOT_DATA,
			      link->sync_word);
}

const zlrs_radio_req_t *link_send_sess_resp(zlrs_link_t *link)
{
	uint8_t payload[3U * sizeof(uint32_t) + sizeof(uint16_t)];
	uint16_t len;

	wr32(&payload[0], link->sess_nonce_t);
	wr32(&payload[4], link->sess_nonce_r);
	payload[8] = (uint8_t)link->fhss.index;
	payload[9] = (uint8_t)(link->fhss.index >> 8);
	wr32(&payload[10], link->frame_index);
	len = link_build_frame(link, ZLRS_PKT_SESS_RESP, 0U, payload, sizeof(payload),
			       link_tag_key_for(link, ZLRS_PKT_SESS_RESP, true));
	link->sess_resp_due = false;
	link->stats.sess_resp_tx++;

	/* Still on the sync channel: the TX is in SEARCHING and listens there */
	return link_submit_tx(link, len, link_sync_freq_hz(link), ZLRS_SYNC_SLOT_DATA,
			      link->sync_word);
}

const zlrs_radio_req_t *link_send_sess_cfm(zlrs_link_t *link)
{
	uint8_t payload[2U * sizeof(uint32_t) + sizeof(uint16_t)];
	uint16_t len;

	wr32(&payload[0], link->sess_nonce_r);
	/*
	 * Echo the current hop index: a re-handshake can happen mid-session (peer reboot), and then
	 * the peer gets exactly the index this end is using instead of guessing from SYNC.
	 */
	payload[4] = (uint8_t)(link->fhss.index & 0xFFU);
	payload[5] = (uint8_t)(link->fhss.index >> 8);
	wr32(&payload[6], link->frame_index);
	link->sess_sync_frame = link->frame_index;
	len = link_build_frame(link, ZLRS_PKT_SESS_CFM, 0U, payload, sizeof(payload),
			       link_tag_key_for(link, ZLRS_PKT_SESS_CFM, true));
	/* DATA confirmation remains due until the peer proves that it has this session key.
	 * Building or even transmitting the confirm does not establish that it arrived. */
	link->stats.sess_cfm_tx++;

	return link_submit_tx(link, len, link_sync_freq_hz(link), ZLRS_SYNC_SLOT_DATA,
			      link->sync_word);
}

void link_handle_sync(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	uint32_t nonce;
	uint16_t hop_index;

	/* The initiator owns SYNC and nonce_t, including after a reboot. */
	if (!link->paired || link->cfg.role != ZLRS_ROLE_RX) {
		return;
	}
	if (frame->payload_len != 12U) {
		return;
	}
	if (frame->payload[0] != ZLRS_PROTO_VERSION ||
	    frame->payload[1] != (uint8_t)link->cfg.profile) {
		return;
	}

	hop_index = (uint16_t)(frame->payload[2] | ((uint16_t)frame->payload[3] << 8));
	nonce = rd32(&frame->payload[4]);

	if (link->state == ZLRS_STATE_CONNECTED) {
		/*
		 * Periodic SYNC while connected (doc/08-protocol.md).
		 */
		if (link->cfg.resync_every_n == 0U) {
			/* Periodic resync off: no SYNC is expected once the session is up */
			return;
		}
		link->stats.rx_syncs++;

		if (link->session_valid && nonce == link->sess_nonce_t) {
			/*
			 * Same session: only re-align the hop index, erasing crystal drift between the ends.
			 * The sequence number must be updated: SYNC shares the same sequence space, and a
			 * recorded frame must not be able to drag the index off.
			 */
			if ((int16_t)(frame->seq - link->last_rx_seq) <= 0) {
				link->stats.rx_dup++;
				return;
			}
			link->last_rx_seq = frame->seq;
			link_lq_account(link, frame->seq);
			link->last_rx_ms = now_ms;
			if (link->cfg.hopping != 0U && link->fhss.index != hop_index) {
				link->stats.hop_resync++;
			}
			zlrs_fhss_set_index(&link->fhss, hop_index);
			link->frame_index = rd32(&frame->payload[8]);
			link->last_rx_frame = link->frame_index;
			return;
		}

		/*
		 * A different session (peer reboot or re-pair). Only trust this when the peer has gone
		 * completely quiet: the SYNC tag is pairing-level, so a recorded SYNC still verifies and
		 * must not drag a working link back to SEARCHING.
		 */
		if ((uint32_t)(now_ms - link->last_rx_ms) <= link->cfg.sync_period_ms) {
			link->stats.rx_dup++;
			return;
		}

		link->session_valid = false;
		link->session_pending = false;
		link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
	}

	if (link->state != ZLRS_STATE_SEARCHING) {
		return;
	}

	if (link->cfg.handshake == 0U) {
		link->frame_index = rd32(&frame->payload[8]);
		/*
		 * Simplified handshake: this SYNC already verified with the pairing key, so adopting its hop
		 * index is enough to count the session as up (the key is static, no nonces to exchange).
		 * SESS_RESP keeps reporting until the peer's first frame arrives, see send_announce().
		 */
		link->sess_nonce_t = nonce;
		link->sess_hop = hop_index;
		session_activate(link, hop_index);
		link->last_rx_seq = frame->seq;
		link->last_rx_ms = now_ms;
		link->last_rx_frame = link->frame_index;
		link->announce = true;
		link_set_state(link, ZLRS_STATE_CONNECTED, now_ms);
		return;
	}

	/*
	 * A SYNC with the same nonce_t is a retransmit (the TX never got our answer): repeat the
	 * previous answer. nonce_r must stay put or the peer derives a different key than we do.
	 */
	if (link->session_pending && nonce == link->sess_nonce_t) {
		link->sess_resp_due = true;
		return;
	}

	/*
	 * Anyone can record a SYNC and replay it, so this step only agrees to talk: draw a nonce_r,
	 * prepare the key, and only let the TX's SESS_CFM make the session real.
	 */
	link->sess_nonce_t = nonce;
	link->sess_nonce_r = link_random(link, now_ms);
	link->sess_hop = hop_index;
	derive_session_keys(link, link->sess_nonce_t, link->sess_nonce_r);
	link->session_pending = true;
	link->sess_resp_due = true;
	link->last_rx_ms = now_ms;
	link->stats.last_rssi = link->stats.last_rssi; /* rssi is written by the port layer */
}

int link_handle_sess_cfm(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	uint8_t expect[ZLRS_TAG_LEN];
	const uint8_t tag_len = zlrs_profile_tag_len(link->cfg.profile);
	const bool repeated = link->cfg.role == ZLRS_ROLE_RX &&
			      link->state == ZLRS_STATE_CONNECTED && link->session_valid;

	if (!repeated && (!link->session_pending || link->state != ZLRS_STATE_SEARCHING)) {
		return -1;
	}
	if (frame->payload_len != 10U) {
		return -1;
	}

	/*
	 * Under P0 (no tag) there is nothing to verify: P0 is the no-authentication debug profile
	 * and only the echoed nonce_r can be checked. Production builds never take this branch.
	 */
	if (tag_len > 0U) {
		zlrs_tag(link->session_key_in, ZLRS_KEY_LEN, link->rx_buf,
			 zlrs_frame_mac_len(frame), expect);
		if (!zlrs_ct_equal(expect, frame->tag, ZLRS_TAG_LEN)) {
			link->stats.rx_bad_tag++;
			return -1;
		}
	}
	if (rd32(frame->payload) != link->sess_nonce_r) {
		return -1;
	}
	if (repeated && (int16_t)(frame->seq - link->last_rx_seq) <= 0) {
		link->stats.rx_dup++;
		return -1;
	}

	link->stats.rx_frames++;
	/* The fixed confirm carries the current hop index, including mid-session re-handshakes. */
	link->stats.sess_cfm_rx++;
	const uint16_t hop = (uint16_t)(frame->payload[4] | ((uint16_t)frame->payload[5] << 8));

	if (repeated) {
		/* Lost session ACK: answer the newer confirm without discarding stream progress. */
		zlrs_fhss_set_index(&link->fhss, hop);
	} else {
		session_activate(link, hop);
	}
	link->frame_index = rd32(&frame->payload[6]);
	link->last_rx_seq = frame->seq; /* the confirm frame itself takes the first slot from 0 */
	link->last_rx_ms = now_ms;
	link->last_rx_frame = link->frame_index;
	link->sess_ack_due = true;
	link->sess_sync_frame = link->frame_index;
	link_set_state(link, ZLRS_STATE_CONNECTED, now_ms);

	return 0;
}

bool link_handle_sess_resp(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	uint32_t nonce_t;
	uint32_t nonce_r;

	/* SESS_RESP goes responder -> initiator; drop it if the role does not fit */
	if (link->cfg.role != ZLRS_ROLE_TX || frame->payload_len != 14U) {
		return false;
	}

	nonce_t = rd32(&frame->payload[0]);
	nonce_r = rd32(&frame->payload[4]);
	if (nonce_t != link->sess_nonce_t) {
		return false;
	}
	link->stats.sess_resp_rx++;

	/*
	 * Simplified handshake: the peer is saying "I heard you". The key is static and there is no
	 * confirm to send — our next data frame is it. While CONNECTED it only means "still there".
	 */
	if (link->cfg.handshake == 0U) {
		link->last_rx_ms = now_ms;
		if (link->state == ZLRS_STATE_SEARCHING) {
			zlrs_fhss_set_index(
				&link->fhss,
				(uint16_t)(frame->payload[8] | ((uint16_t)frame->payload[9] << 8)));
			link->frame_index = rd32(&frame->payload[10]);
			link_set_state(link, ZLRS_STATE_CONNECTED, now_ms);
		}
		return true;
	}

	/*
	 * A SESS_RESP while CONNECTED means the peer rebooted, heard our periodic SYNC and is now
	 * asking to re-handshake: adopt its nonce_r, derive a fresh session key and confirm.
	 */
	if (link->state != ZLRS_STATE_SEARCHING && link->state != ZLRS_STATE_CONNECTED) {
		return false;
	}

	if (!link->session_valid || nonce_r != link->sess_nonce_r) {
		link->sess_nonce_r = nonce_r;
		derive_session_keys(link, nonce_t, nonce_r);
		session_activate(link, link->fhss.index);
	} else if (!link->sess_cfm_due) {
		/* A delayed pairing-level response cannot reopen a confirmed DATA session. */
		return true;
	}
	link->sess_cfm_due = true;
	/* Session just came up: keep the peer-loss check from kicking us out on a stale timestamp */
	link->last_rx_ms = now_ms;

	return true;
}

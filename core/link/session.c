/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* Session key derivation, three-step confirmation and schedule synchronization. */
#define LABEL_SESSION "zlrs/session"

static bool fresh_nonce(zlrs_link_t *link, uint32_t previous, uint32_t *out)
{
	if (link_new_nonce(link, previous, link->sess_nonce_valid, out)) {
		link->sess_nonce_valid = true;
		return true;
	}
	return false;
}

void link_roll_session(zlrs_link_t *link, uint32_t now_ms)
{
	(void)now_ms;
	link->session_valid = false;
	link->session_pending = false;
	link->sess_cfm_due = false;
	link->sess_resp_due = false;
	link->sess_ack_due = false;
	link->prepared_data.valid = false;
	link->sess_rx_seq = 0U;
	link->band_plan.switch_frame = 0U;
	link->band_announced = false;
	if (link->cfg.automatic_band) {
		link->radio_acquiring = true;
		link->radio_sync_seen = false;
	}
	link->last_rx_seq = 0U;
	if (fresh_nonce(link, link->sess_nonce_leader, &link->sess_nonce_leader)) {
		link->tx_seq = 1U;
		link->session_pending = true;
	}
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
	memcpy(&ids_ctx[ctx_len], link->dev_id, ZLRS_DEV_ID_LEN);
	memcpy(&ids_ctx[ctx_len + ZLRS_DEV_ID_LEN], link->peer_id, ZLRS_DEV_ID_LEN);
	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, label, ids_ctx, len, out, ZLRS_KEY_LEN);
	memcpy(&ids_ctx[ctx_len], link->peer_id, ZLRS_DEV_ID_LEN);
	memcpy(&ids_ctx[ctx_len + ZLRS_DEV_ID_LEN], link->dev_id, ZLRS_DEV_ID_LEN);
	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, label, ids_ctx, len, in, ZLRS_KEY_LEN);
}

static void derive_session_keys(zlrs_link_t *link)
{
	uint8_t ctx[2U * sizeof(uint32_t)];
	wr32(ctx, link->sess_nonce_leader);
	wr32(ctx + 4U, link->sess_nonce_follower);
	link_derive_by_endpoints(link, LABEL_SESSION, ctx, sizeof(ctx), link->session_key_out,
				 link->session_key_in);
}

static void session_activate(zlrs_link_t *link, uint8_t hop_index)
{
	link->prepared_data.valid = false;
	link->session_valid = true;
	link->session_pending = false;
	link->sess_resp_due = false;
	link->last_rx_seq = link->sess_rx_seq;
	link->sess_ack_due = false;
	link->sess_sync_frame = link->frame_index;
	/* Keep the handshake's TX counter; reset only the new session's DATA/LQ state. */
	link->lq_last_seq = 0U;
	link->lq_started = false;
	link->stats.lq_rx = 0U;
	link->stats.lq_expected = 0U;
	link->arq_used = 0U;
	link->arq_retries = 0U;
	link->arq_seq = 0U;
	link->arq_rx_started = false;
	link->arq_ack_pending = false;
	zlrs_fhss_set_index(&link->fhss, hop_index);
}

const zlrs_radio_req_t *link_send_sync(zlrs_link_t *link, uint32_t now_ms)
{
	link->band_plan.domain = link->cfg.domain;
	link->band_plan.automatic = link->cfg.automatic_band;
	link->band_announced = link->band_plan.switch_frame != 0U;
	zlrs_frame_t frame = {
		.type = ZLRS_PKT_SYNC,
		.body.sync = {.version = ZLRS_PROTO_VERSION,
			      .profile = link->cfg.profile,
			      .hop_index = link->fhss.index,
			      .nonce_leader = link->sess_nonce_leader,
			      .frame_index = link->frame_index,
			      .band_plan = link->band_plan},
	};
	const uint16_t len = link_build_frame(link, &frame, ZLRS_SYNC_PAYLOAD_LEN,
					     link_tag_key_for(link, frame.type, true));
	link->last_sync_ms = now_ms;
	link->stats.tx_syncs++;
	return link_submit_tx(link, len, link_sync_freq_hz(link), ZLRS_SYNC_SLOT_DATA,
			      link->sync_word);
}

const zlrs_radio_req_t *link_send_sess_resp(zlrs_link_t *link)
{
	zlrs_frame_t frame = {
		.type = ZLRS_PKT_SESS_RESP,
		.body.sess_resp = {.nonce_leader = link->sess_nonce_leader,
				   .nonce_follower = link->sess_nonce_follower},
	};
	const uint16_t len = link_build_frame(link, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN,
					     link_tag_key_for(link, frame.type, true));
	link->sess_resp_due = false;
	link->stats.sess_resp_tx++;
	return link_submit_tx(link, len, link_sync_freq_hz(link), ZLRS_SYNC_SLOT_DATA,
			      link->sync_word);
}

const zlrs_radio_req_t *link_send_sess_cfm(zlrs_link_t *link)
{
	link->band_plan.domain = link->cfg.domain;
	link->band_plan.automatic = link->cfg.automatic_band;
	zlrs_frame_t frame = {
		.type = ZLRS_PKT_SESS_CFM,
		.body.sess_cfm = {.nonce_follower = link->sess_nonce_follower,
				  .hop_index = link->fhss.index,
				  .frame_index = link->frame_index,
				  .band_plan = link->band_plan},
	};
	link->sess_sync_frame = link->frame_index;
	const uint16_t len = link_build_frame(link, &frame, ZLRS_SESS_CFM_PAYLOAD_LEN,
					     link_tag_key_for(link, frame.type, true));
	link->stats.sess_cfm_tx++;
	return link_submit_tx(link, len, link_sync_freq_hz(link), ZLRS_SYNC_SLOT_DATA,
			      link->sync_word);
}

bool link_handle_sync(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	const uint32_t nonce = frame->body.sync.nonce_leader;
	const uint8_t hop = frame->body.sync.hop_index;
	if (!link->paired || link->cfg.role != ZLRS_ROLE_FOLLOWER ||
	    frame->body.sync.version != ZLRS_PROTO_VERSION ||
	    frame->body.sync.profile != link->cfg.profile || hop >= link->fhss.sequence_count ||
	    !link_band_plan_valid(link, &frame->body.sync.band_plan,
				  frame->body.sync.frame_index)) {
		return false;
	}
	if (link->state == ZLRS_STATE_CONNECTED) {
		if (link->cfg.resync_every_n == 0U) {
			return false;
		}
		if (link->session_valid && nonce == link->sess_nonce_leader) {
			if (frame->seq <= link->last_rx_seq) {
				link->stats.rx_dup++;
				return false;
			}
			link->last_rx_seq = frame->seq;
			link_lq_account(link, frame->seq);
			link->last_rx_ms = now_ms;
			link->stats.rx_syncs++;
			link->stats.hop_resync += link->cfg.hopping && link->fhss.index != hop;
			zlrs_fhss_set_index(&link->fhss, hop);
			link->frame_index = frame->body.sync.frame_index;
			link->last_rx_frame = link->frame_index;
			if (link->cfg.automatic_band || link->radio_acquiring) {
				/* The leader still waits on the capture channel for proof of return. */
				link->sess_ack_due = !link->radio_acquiring;
			}
			link->radio_sync_seen = true;
			link_adopt_band_plan(link, &frame->body.sync.band_plan);
			return true;
		}
		if ((uint32_t)(now_ms - link->last_rx_ms) <= link->cfg.sync_period_ms) {
			link->stats.rx_dup++;
			return false;
		}
		link->session_valid = false;
		link->session_pending = false;
		link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
		if (link->cfg.automatic_band) {
			link->radio_acquiring = true;
			link->radio_sync_seen = false;
		}
	}
	if (link->state != ZLRS_STATE_SEARCHING) {
		return false;
	}
	if (link->session_pending && nonce == link->sess_nonce_leader) {
		if (frame->seq <= link->sess_rx_seq) {
			link->stats.rx_dup++;
			return false;
		}
	} else {
		uint32_t nonce_follower;
		if (!fresh_nonce(link, link->sess_nonce_follower, &nonce_follower)) {
			return false;
		}
		link->sess_nonce_leader = nonce;
		link->sess_nonce_follower = nonce_follower;
		link->sess_hop = hop;
		link->tx_seq = 1U;
		derive_session_keys(link);
		link->session_pending = true;
	}
	link->sess_rx_seq = frame->seq;
	link->sess_resp_due = true;
	link->radio_sync_seen = true;
	link->stats.rx_syncs++;
	return true;
}

int link_handle_sess_cfm(zlrs_link_t *link, const zlrs_frame_t *frame,
		       uint16_t len, uint32_t now_ms)
{
	const uint8_t tag_len = zlrs_profile_tag_len(link->cfg.profile);
	const bool repeated = link->state == ZLRS_STATE_CONNECTED && link->session_valid;
	if (link->cfg.role != ZLRS_ROLE_FOLLOWER ||
	    (!repeated && (!link->session_pending || link->state != ZLRS_STATE_SEARCHING)) ||
	    frame->body.sess_cfm.hop_index >= link->fhss.sequence_count ||
	    !link_band_plan_valid(link, &frame->body.sess_cfm.band_plan,
				  frame->body.sess_cfm.frame_index)) {
		return -1;
	}
	if (tag_len > 0U) {
		uint8_t expect[ZLRS_TAG_LEN];
		zlrs_tag(link->session_key_in, ZLRS_KEY_LEN, link->rx_buf, len - tag_len, expect);
		if (!zlrs_ct_equal(expect, frame->tag, ZLRS_TAG_LEN)) {
			link->stats.rx_bad_tag++;
			return -1;
		}
	}
	if (frame->body.sess_cfm.nonce_follower != link->sess_nonce_follower) {
		return -1;
	}
	const uint32_t baseline = repeated ? link->last_rx_seq : link->sess_rx_seq;
	if (frame->seq <= baseline) {
		link->stats.rx_dup++;
		return -1;
	}
	link->stats.rx_frames++;
	link->stats.rx_bytes += len;
	link->stats.sess_cfm_rx++;
	if (!repeated) {
		session_activate(link, frame->body.sess_cfm.hop_index);
	} else {
		zlrs_fhss_set_index(&link->fhss, frame->body.sess_cfm.hop_index);
	}
	link->frame_index = frame->body.sess_cfm.frame_index;
	link->last_rx_seq = frame->seq;
	link_lq_account(link, frame->seq);
	link->last_rx_ms = now_ms;
	link->last_rx_frame = link->frame_index;
	link->sess_ack_due = !link->radio_acquiring;
	link->sess_sync_frame = link->frame_index;
	link_adopt_band_plan(link, &frame->body.sess_cfm.band_plan);
	link->radio_sync_seen = true;
	link_set_state(link, ZLRS_STATE_CONNECTED, now_ms);
	return 0;
}

bool link_handle_sess_resp(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	const uint32_t nonce_leader = frame->body.sess_resp.nonce_leader;
	const uint32_t nonce_follower = frame->body.sess_resp.nonce_follower;
	if (link->cfg.role != ZLRS_ROLE_LEADER || nonce_leader != link->sess_nonce_leader) {
		return false;
	}
	if (link->session_valid) {
		if (nonce_follower != link->sess_nonce_follower) {
			/* A silent/rebooted peer is only a hint to issue a new local challenge.
			 * Never install the key from a response to an already completed exchange. */
			if (link->state == ZLRS_STATE_CONNECTED &&
			    (uint32_t)(now_ms - link->last_rx_ms) > link->cfg.sync_period_ms) {
				link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
			}
			return false;
		}
		if (!link->sess_cfm_due || frame->seq <= link->last_rx_seq) {
			return false;
		}
		/* A newer repeated response may prompt confirmation, but never resets DATA. */
		link->last_rx_seq = frame->seq;
	} else {
		if (link->state != ZLRS_STATE_SEARCHING || !link->session_pending ||
		    frame->seq <= link->sess_rx_seq) {
			return false;
		}
		link->sess_nonce_follower = nonce_follower;
		link->sess_rx_seq = frame->seq;
		derive_session_keys(link);
		session_activate(link, (uint8_t)link->fhss.index);
	}
	link->stats.sess_resp_rx++;
	link->sess_cfm_due = true;
	return true;
}

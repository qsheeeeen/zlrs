/* SPDX-License-Identifier: Apache-2.0 */
#include "link/handshake.h"
#include "link/data.h"
#include <string.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

/* Session key derivation, three-step confirmation and schedule synchronization. */
#define LABEL_SESSION "zlrs/session"

void link_roll_session(zlrs_link_t *link)
{
	link->status.session_valid = false;
	link->session_pending = false;
	link->status.sess_cfm_due = false;
	link->sess_ack_due = false;
	link->sess_rx_seq = 0U;
	link->status.band_plan.switch_frame = 0U;
	link->band_announced = false;
	if (link->status.cfg.automatic_band) {
		link->status.radio_acquiring = true;
		link->status.radio_sync_seen = false;
	}
	link->last_rx_seq = 0U;
	if (link_new_nonce(link, link->status.sess_nonce_leader, &link->sess_nonce_valid,
			   &link->status.sess_nonce_leader)) {
		link->tx_seq = 1U;
		link->session_pending = true;
	}
}

static void link_derive_by_endpoints(const zlrs_link_t *link, const char *label, const uint8_t *ctx,
				     size_t ctx_len, uint8_t out[ZLRS_KEY_LEN],
				     uint8_t in[ZLRS_KEY_LEN])
{
	uint8_t ids_ctx[2U * sizeof(uint32_t) + 2U * ZLRS_DEV_ID_LEN];
	const size_t len = ctx_len + 2U * ZLRS_DEV_ID_LEN;

	__ASSERT_NO_MSG(len <= sizeof(ids_ctx));
	if (ctx_len > 0U) {
		memcpy(ids_ctx, ctx, ctx_len);
	}
	memcpy(&ids_ctx[ctx_len], link->status.dev_id, ZLRS_DEV_ID_LEN);
	memcpy(&ids_ctx[ctx_len + ZLRS_DEV_ID_LEN], link->status.pairing.peer_id, ZLRS_DEV_ID_LEN);
	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, label, ids_ctx, len, out, ZLRS_KEY_LEN);
	memcpy(&ids_ctx[ctx_len], link->status.pairing.peer_id, ZLRS_DEV_ID_LEN);
	memcpy(&ids_ctx[ctx_len + ZLRS_DEV_ID_LEN], link->status.dev_id, ZLRS_DEV_ID_LEN);
	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, label, ids_ctx, len, in, ZLRS_KEY_LEN);
}

static void derive_session_keys(zlrs_link_t *link)
{
	uint8_t ctx[2U * sizeof(uint32_t)];
	sys_put_le32(link->status.sess_nonce_leader, ctx);
	sys_put_le32(link->status.sess_nonce_follower, ctx + 4U);
	link_derive_by_endpoints(link, LABEL_SESSION, ctx, sizeof(ctx), link->session_key_out,
				 link->session_key_in);
}

static void session_activate(zlrs_link_t *link)
{
	link->status.session_valid = true;
	link->session_pending = false;
	link->last_rx_seq = link->sess_rx_seq;
	link->sess_ack_due = false;
	link->sess_sync_frame = link->status.frame_index;
	/* Keep the handshake's TX counter; reset only the new session's DATA/LQ state. */
	link_quality_reset(link);
	link->status.arq_used = 0U;
	link->arq_launched = false;
	link->arq_local_failed = false;
	link->arq_retries = 0U;
	link->status.arq_seq = 0U;
	link->arq_rx_started = false;
	link->status.arq_ack_pending = false;
}

bool link_handle_schedule(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	const zlrs_schedule_frame_t *schedule = &frame->body.schedule;
	const bool sync = frame->type == ZLRS_PKT_SYNC;
	const bool connected = link->status.state == ZLRS_STATE_CONNECTED;
	const bool confirmed = connected && link->status.session_valid;
	if (link->status.cfg.role != ZLRS_ROLE_FOLLOWER ||
	    schedule->hop_index >= link->status.fhss.sequence_count ||
	    !link_band_plan_valid(link, &schedule->band_plan, schedule->frame_index) ||
	    (sync && (!link->status.paired || schedule->version != ZLRS_PROTO_VERSION ||
		      schedule->profile != link->status.cfg.profile))) {
		return false;
	}
	if (sync) {
		if (connected) {
			if (link->status.cfg.resync_every_n == 0U) {
				return false;
			}
			if (!confirmed || schedule->nonce != link->status.sess_nonce_leader) {
				if ((uint32_t)(now_ms - link->status.last_rx_ms) <=
				    link->status.cfg.sync_period_ms) {
					link->status.stats.rx_dup++;
					return false;
				}
				link->status.session_valid = false;
				link->session_pending = false;
				link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
				if (link->status.cfg.automatic_band) {
					link->status.radio_acquiring = true;
					link->status.radio_sync_seen = false;
				}
			}
		}
		if (link->status.state == ZLRS_STATE_SEARCHING) {
			if (link->session_pending &&
			    schedule->nonce == link->status.sess_nonce_leader) {
				if (frame->seq <= link->sess_rx_seq) {
					link->status.stats.rx_dup++;
					return false;
				}
			} else {
				uint32_t nonce;
				if (!link_new_nonce(link, link->status.sess_nonce_follower,
						    &link->sess_nonce_valid, &nonce)) {
					return false;
				}
				link->status.sess_nonce_leader = schedule->nonce;
				link->status.sess_nonce_follower = nonce;
				link->tx_seq = 1U;
				derive_session_keys(link);
				link->session_pending = true;
			}
			link->sess_rx_seq = frame->seq;
			link->status.radio_sync_seen = true;
			link->status.stats.rx_syncs++;
			return true;
		}
		if (!connected) {
			return false;
		}
	} else if ((!confirmed &&
		    (!link->session_pending || link->status.state != ZLRS_STATE_SEARCHING)) ||
		   schedule->nonce != link->status.sess_nonce_follower) {
		return false;
	}
	if (frame->seq <= (confirmed ? link->last_rx_seq : link->sess_rx_seq)) {
		link->status.stats.rx_dup++;
		return false;
	}
	if (sync) {
		link->status.stats.rx_syncs++;
		link->status.stats.hop_resync +=
			link->status.cfg.hopping && link->status.fhss.index != schedule->hop_index;
	} else {
		link->status.stats.sess_cfm_rx++;
		if (!confirmed) {
			session_activate(link);
		}
		link->sess_sync_frame = schedule->frame_index;
	}
	if (!sync || link->status.cfg.automatic_band || link->status.radio_acquiring) {
		/* The leader waits on the capture channel for proof of return. */
		link->sess_ack_due = !link->status.radio_acquiring;
	}
	zlrs_fhss_set_index(&link->status.fhss, schedule->hop_index);
	link->status.frame_index = schedule->frame_index;
	link->last_rx_seq = frame->seq;
	link_lq_account(link, frame->seq);
	link->status.last_rx_ms = now_ms;
	link->status.last_rx_frame = schedule->frame_index;
	link_adopt_band_plan(link, &schedule->band_plan);
	link->status.radio_sync_seen = true;
	link_set_state(link, ZLRS_STATE_CONNECTED, now_ms);
	return true;
}

bool link_handle_sess_resp(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	const uint32_t nonce_leader = frame->body.sess_resp.nonce_leader;
	const uint32_t nonce_follower = frame->body.sess_resp.nonce_follower;
	if (link->status.cfg.role != ZLRS_ROLE_LEADER ||
	    nonce_leader != link->status.sess_nonce_leader) {
		return false;
	}
	if (link->status.session_valid) {
		if (nonce_follower != link->status.sess_nonce_follower) {
			/* A silent/rebooted peer is only a hint to issue a new local challenge.
			 * Never install the key from a response to an already completed exchange. */
			if (link->status.state == ZLRS_STATE_CONNECTED &&
			    (uint32_t)(now_ms - link->status.last_rx_ms) >
				    link->status.cfg.sync_period_ms) {
				link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
			}
			return false;
		}
		if (!link->status.sess_cfm_due || frame->seq <= link->last_rx_seq) {
			return false;
		}
		/* A newer repeated response may prompt confirmation, but never resets DATA. */
		link->last_rx_seq = frame->seq;
	} else {
		if (link->status.state != ZLRS_STATE_SEARCHING || !link->session_pending ||
		    frame->seq <= link->sess_rx_seq) {
			return false;
		}
		link->status.sess_nonce_follower = nonce_follower;
		link->sess_rx_seq = frame->seq;
		derive_session_keys(link);
		session_activate(link);
	}
	link->status.stats.sess_resp_rx++;
	link->status.sess_cfm_due = true;
	return true;
}

/* Identity, pairing key derivation and the repeated pairing handshake. */
const uint8_t pairing_sync_word[ZLRS_SYNC_WORD_LEN] = {'Z', 'L', 'R', 'S'};
#define LABEL_PAIR      "zlrs/pair"
#define LABEL_AUTH      "zlrs/auth"
#define LABEL_FHSS      "zlrs/fhss"
#define LABEL_SYNC_DATA "zlrs/sync/data"
#define LABEL_LINK_AUTH "zlrs/link-auth"

static void derive_pair(zlrs_link_t *link)
{
	uint8_t ctx[2U * ZLRS_DEV_ID_LEN + ZLRS_PAIR_SALT_LEN];
	uint8_t seed[4];

	/*
	 * Both ids plus the two pairing nonces, sorted and concatenated: both ends get the same
	 * pair_key, and re-pairing changes the nonces, which replaces the whole subtree below
	 * (see doc/08-security.md#cryptographic-parameters).
	 */
	const bool reverse =
		memcmp(link->status.dev_id, link->status.pairing.peer_id, ZLRS_DEV_ID_LEN) > 0;
	memcpy(ctx, reverse ? link->status.pairing.peer_id : link->status.dev_id, ZLRS_DEV_ID_LEN);
	memcpy(ctx + ZLRS_DEV_ID_LEN, reverse ? link->status.dev_id : link->status.pairing.peer_id,
	       ZLRS_DEV_ID_LEN);
	const uint32_t a = sys_get_le32(link->status.pairing.pair_salt);
	const uint32_t b = sys_get_le32(link->status.pairing.pair_salt + sizeof(uint32_t));
	sys_put_le32(MIN(a, b), ctx + 2U * ZLRS_DEV_ID_LEN);
	sys_put_le32(MAX(a, b), ctx + 2U * ZLRS_DEV_ID_LEN + sizeof(uint32_t));
	zlrs_kdf(link->root_key, ZLRS_KEY_LEN, LABEL_PAIR, ctx, sizeof(ctx), link->pair_key,
		 ZLRS_KEY_LEN);
	link_derive_by_endpoints(link, LABEL_LINK_AUTH, NULL, 0U, link->link_auth_out,
				 link->link_auth_in);

	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, LABEL_FHSS, NULL, 0U, seed, sizeof(seed));
	zlrs_fhss_init(&link->status.fhss, link->status.cfg.region, sys_get_le32(seed));

	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, LABEL_SYNC_DATA, NULL, 0U, link->sync_word,
		 sizeof(link->sync_word));

	link->status.session_valid = false;
	link->session_pending = false;
	link->status.sess_cfm_due = false;
	link->sess_ack_due = false;
	link->last_rx_seq = 0U;
	link->sess_rx_seq = 0U;
	link->status.fhss.index = 0U;
}

static void pairing_complete(zlrs_link_t *link, uint32_t now_ms)
{
	/* Publish the peer and salt together, only after confirmation. */
	memcpy(link->status.pairing.peer_id, link->pair_peer_id, ZLRS_DEV_ID_LEN);
	/* The two handshake nonces are this pair's material: keep them to recompute pair_key */
	sys_put_le32(link->status.pair_nonce,
		     &link->status.pairing.pair_salt[0]); /* nonce_a: initiator */
	sys_put_le32(link->status.pair_nonce_b,
		     &link->status.pairing.pair_salt[4]); /* nonce_b: responder */

	derive_pair(link);
	link->status.paired = true;
	link->status.pair_cfm_pending = false;
	link->status.pair_cfm_sent = 0U;
	link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
	link->status.last_rx_ms = now_ms;
}

static bool pair_reject(zlrs_link_t *link)
{
	link->status.stats.pair_rejects++;

	return false;
}

bool link_handle_pair_frame(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	const zlrs_pair_frame_t *const pair = &frame->body.pair;
	const bool initiator = (link->status.cfg.role == ZLRS_ROLE_LEADER);

	if (frame->type == ZLRS_PKT_PAIR_REQ) {
		link->status.stats.pair_req_rx++;
		/*
	 * Responder: remember the initiator's id and nonce, answer PAIR_RESP next slot. The
	 * responder nonce is only drawn once per handshake: the initiator repeats PAIR_REQ until
	 * it hears the response, and a fresh nonce per repeat would make the confirmation it
	 * finally sends fail the check below (the reply it answered had another nonce).
	 */
		if (initiator || link->status.state != ZLRS_STATE_PAIRING ||
		    zlrs_ct_equal(frame->body.pair.dev_a, link->status.dev_id, ZLRS_DEV_ID_LEN)) {
			return pair_reject(link);
		}
		const bool same_request =
			link->pair_request_valid &&
			link->status.pair_nonce == frame->body.pair.nonce_a &&
			zlrs_ct_equal(link->pair_peer_id, frame->body.pair.dev_a, ZLRS_DEV_ID_LEN);
		if (same_request && frame->seq <= link->pair_rx_seq) {
			link->status.stats.rx_dup++;
			return false;
		}
		if (!same_request) {
			uint32_t nonce_b;
			if (!link_new_nonce(link, link->status.pair_nonce_b,
					    &link->pair_nonce_valid, &nonce_b)) {
				return pair_reject(link);
			}
			memcpy(link->pair_peer_id, frame->body.pair.dev_a, ZLRS_DEV_ID_LEN);
			link->status.pair_nonce = frame->body.pair.nonce_a;
			link->status.pair_nonce_b = nonce_b;
			link->pair_tx_seq = 1U;
			link->pair_request_valid = true;
		}
		link->pair_rx_seq = frame->seq;

		return true;
	}
	if (frame->type == ZLRS_PKT_PAIR_RESP) {
		link->status.stats.pair_resp_rx++;
	} else if (frame->type == ZLRS_PKT_PAIR_CFM) {
		link->status.stats.pair_cfm_rx++;
	} else {
		return pair_reject(link);
	}
	if (!link->pair_request_valid ||
	    frame->type != (initiator ? ZLRS_PKT_PAIR_RESP : ZLRS_PKT_PAIR_CFM)) {
		return pair_reject(link);
	}
	const uint8_t *a = initiator ? link->status.dev_id : link->pair_peer_id;
	const uint8_t *b = initiator ? link->pair_peer_id : link->status.dev_id;
	if (!zlrs_ct_equal(pair->dev_a, a, ZLRS_DEV_ID_LEN) ||
	    pair->nonce_a != link->status.pair_nonce ||
	    (initiator && zlrs_ct_equal(pair->dev_b, link->status.dev_id, ZLRS_DEV_ID_LEN)) ||
	    ((!initiator || link->pair_rx_seq != 0U) &&
	     (!zlrs_ct_equal(pair->dev_b, b, ZLRS_DEV_ID_LEN) ||
	      pair->nonce_b != link->status.pair_nonce_b))) {
		return pair_reject(link);
	}
	if (frame->seq <= link->pair_rx_seq) {
		link->status.stats.rx_dup++;
		return false;
	}
	link->pair_rx_seq = frame->seq;
	if (initiator) {
		memcpy(link->pair_peer_id, pair->dev_b, ZLRS_DEV_ID_LEN);
		link->status.pair_nonce_b = pair->nonce_b;
		link->status.pair_cfm_pending = true;
	} else if (link->status.state == ZLRS_STATE_PAIRING) {
		pairing_complete(link, now_ms);
	} else {
		/* A repeated confirmation never resets the searching session. */
		return link->status.paired && link->status.state == ZLRS_STATE_SEARCHING;
	}
	return true;
}

bool link_pair_advance(zlrs_link_t *link)
{
	if (link->status.state == ZLRS_STATE_PAIRING &&
	    (uint32_t)(link->slot_ms - link->state_ms) > link->status.cfg.pair_window_ms) {
		(void)zlrs_link_cancel_pair(link, link->slot_ms);
		return true;
	}
	if (link->slot != ZLRS_SLOT_DATA_A || link->status.cfg.role != ZLRS_ROLE_LEADER ||
	    !link->status.pair_cfm_pending || link->status.pair_cfm_sent < ZLRS_PAIR_CFM_REPEATS) {
		return false;
	}
	if (link->status.state == ZLRS_STATE_PAIRING) {
		pairing_complete(link, link->slot_ms);
	} else {
		link->status.pair_cfm_pending = false;
	}
	return true;
}

void link_pair_plan(const zlrs_link_t *link, zlrs_link_plan_t *plan)
{
	const bool a = link->slot == ZLRS_SLOT_DATA_A;
	const zlrs_role_t role = link->status.cfg.role;
	const bool due = (uint32_t)(link->slot_ms - link->last_pair_tx_ms) >=
			 link->status.cfg.pair_period_ms;
	if ((role == ZLRS_ROLE_LEADER) && a && link->status.pair_cfm_pending) {
		if (due) {
			link_plan_tx(link, plan, ZLRS_PKT_PAIR_CFM, 0U);
		}
	} else if (link->status.state == ZLRS_STATE_PAIRING) {
		if ((role == ZLRS_ROLE_LEADER) && a && due) {
			link_plan_tx(link, plan, ZLRS_PKT_PAIR_REQ, 0U);
		} else if (!(role == ZLRS_ROLE_LEADER) && !a && link->pair_request_valid && due) {
			link_plan_tx(link, plan, ZLRS_PKT_PAIR_RESP, 0U);
		} else {
			link_plan_rx(link, plan, link_sync_freq_hz(link), ZLRS_SYNC_SLOT_PAIRING);
		}
	}
}

void zlrs_link_restore(zlrs_link_t *link, const zlrs_persist_state_t *saved, uint32_t now_ms)
{
	__ASSERT_NO_MSG(link->status.state == ZLRS_STATE_UNCONFIGURED);
	memcpy(link->status.dev_id, saved->dev_id, ZLRS_DEV_ID_LEN);
	if (!saved->configured) {
		return;
	}
	memcpy(link->root_key, saved->root_key, ZLRS_KEY_LEN);
	zlrs_kdf(link->root_key, ZLRS_KEY_LEN, LABEL_AUTH, NULL, 0U, link->auth_key, ZLRS_KEY_LEN);
	if (saved->paired &&
	    !zlrs_ct_equal(saved->pairing.peer_id, saved->dev_id, ZLRS_DEV_ID_LEN)) {
		link->status.pairing = saved->pairing;
		link->status.pair_nonce = sys_get_le32(saved->pairing.pair_salt);
		link->status.pair_nonce_b = sys_get_le32(saved->pairing.pair_salt + 4U);
		link->pair_nonce_valid = true;
		link->status.paired = true;
		derive_pair(link);
	}
	link_set_state(link, link->status.paired ? ZLRS_STATE_SEARCHING : ZLRS_STATE_UNPAIRED,
		       now_ms);
}

bool zlrs_link_press(zlrs_link_t *link, uint32_t now_ms)
{
	if (link->status.state == ZLRS_STATE_UNCONFIGURED) {
		return false;
	}

	if (link->status.cfg.role == ZLRS_ROLE_LEADER) {
		uint32_t nonce_a;
		if (!link_new_nonce(link, link->status.pair_nonce, &link->pair_nonce_valid,
				    &nonce_a)) {
			return false;
		}
		link->status.pair_nonce = nonce_a;
	}
	link->status.pair_cfm_pending = false;
	link->status.pair_cfm_sent = 0U;
	link->pair_request_valid = link->status.cfg.role == ZLRS_ROLE_LEADER;
	link->pair_rx_seq = 0U;
	link->pair_tx_seq = 1U;
	link->status.session_valid = false;
	link->session_pending = false;
	link_set_state(link, ZLRS_STATE_PAIRING, now_ms);
	return true;
}

bool zlrs_link_cancel_pair(zlrs_link_t *link, uint32_t now_ms)
{
	if (link->status.state != ZLRS_STATE_PAIRING) {
		return false;
	}
	link->status.pair_cfm_pending = false;
	link->pair_request_valid = false;
	memset(link->pair_peer_id, 0, sizeof(link->pair_peer_id));
	link->status.session_valid = false;
	link->session_pending = false;
	link->status.sess_cfm_due = false;
	link_set_state(link, link->status.paired ? ZLRS_STATE_SEARCHING : ZLRS_STATE_UNPAIRED,
		       now_ms);
	return true;
}

uint32_t zlrs_link_pair_fingerprint(const zlrs_link_t *link)
{
	uint32_t hash = 2166136261U;

	for (size_t i = 0U; i < sizeof(link->pair_key); i++) {
		hash = (hash ^ link->pair_key[i]) * 16777619U;
	}
	return hash;
}

uint16_t link_build_handshake(zlrs_link_t *link, uint8_t type)
{
	zlrs_frame_t frame = {.type = type};
	if (type == ZLRS_PKT_SYNC || type == ZLRS_PKT_SESS_CFM) {
		link->status.band_plan.region = link->status.cfg.region;
		link->status.band_plan.automatic = link->status.cfg.automatic_band;
		link->status.band_plan.phy =
			link->status.cfg.region == ZLRS_REGION_ISM2400 ? link->status.cfg.phy : 0U;
		frame.body.schedule = (zlrs_schedule_frame_t){
			.version = ZLRS_PROTO_VERSION,
			.profile = link->status.cfg.profile,
			.hop_index = link->status.fhss.index,
			.nonce = type == ZLRS_PKT_SYNC ? link->status.sess_nonce_leader
						       : link->status.sess_nonce_follower,
			.frame_index = link->status.frame_index,
			.band_plan = link->status.band_plan,
		};
	}
	switch (type) {
	case ZLRS_PKT_SYNC:
		link->band_announced = link->status.band_plan.switch_frame != 0U;
		link->last_sync_ms = link->slot_ms;
		link->status.stats.tx_syncs++;
		break;
	case ZLRS_PKT_SESS_RESP:
		frame.body.sess_resp.nonce_leader = link->status.sess_nonce_leader;
		frame.body.sess_resp.nonce_follower = link->status.sess_nonce_follower;
		link->status.stats.sess_resp_tx++;
		break;
	case ZLRS_PKT_SESS_CFM:
		link->sess_sync_frame = link->status.frame_index;
		link->status.stats.sess_cfm_tx++;
		break;
	case ZLRS_PKT_PAIR_REQ:
		link->status.stats.pair_req_tx++;
		memcpy(frame.body.pair.dev_a, link->status.dev_id, ZLRS_DEV_ID_LEN);
		frame.body.pair.nonce_a = link->status.pair_nonce;
		break;
	case ZLRS_PKT_PAIR_RESP:
	case ZLRS_PKT_PAIR_CFM: {
		const bool leader = link->status.cfg.role == ZLRS_ROLE_LEADER;
		link->status.stats.pair_resp_tx += type == ZLRS_PKT_PAIR_RESP;
		link->status.stats.pair_cfm_tx += type == ZLRS_PKT_PAIR_CFM;
		memcpy(frame.body.pair.dev_a, leader ? link->status.dev_id : link->pair_peer_id,
		       ZLRS_DEV_ID_LEN);
		memcpy(frame.body.pair.dev_b, leader ? link->pair_peer_id : link->status.dev_id,
		       ZLRS_DEV_ID_LEN);
		frame.body.pair.nonce_a = link->status.pair_nonce;
		frame.body.pair.nonce_b = link->status.pair_nonce_b;
		break;
	}
	default:
		return 0U;
	}
	const uint16_t len = link_build_frame(link, &frame, link->tx_buf, 0U);
	if (zlrs_packet_layouts[type].auth == ZLRS_AUTH_ROOT) {
		link->last_pair_tx_ms = link->slot_ms;
	}
	return len;
}

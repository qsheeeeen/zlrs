/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* Identity, pairing key derivation and the repeated pairing handshake. */
static const uint8_t pairing_sync_word[ZLRS_SYNC_WORD_LEN] = {'Z', 'L', 'R', 'S'};
#define LABEL_PAIR       "zlrs/pair"
#define LABEL_AUTH       "zlrs/auth"
#define LABEL_FHSS       "zlrs/fhss"
#define LABEL_SYNC_DATA  "zlrs/sync/data"
#define LABEL_LINK_AUTH  "zlrs/link-auth"

static void ordered_ids(const zlrs_link_t *link, uint8_t out[2U * ZLRS_DEV_ID_LEN])
{
	const uint8_t *a = link->dev_id;
	const uint8_t *b = link->peer_id;
	bool swap = false;

	/*
	 * Lexicographic, first differing byte decides. Stopping at the first byte that is *greater*
	 * instead (a later byte can then flip the order) makes the result depend on which end asks:
	 * the two ends would concatenate the ids in opposite orders, derive different pair keys from
	 * the same record and strand the handshake in SEARCHING.
	 */
	for (uint8_t i = 0; i < ZLRS_DEV_ID_LEN; i++) {
		if (a[i] != b[i]) {
			swap = a[i] > b[i];
			break;
		}
	}

	if (swap) {
		memcpy(out, b, ZLRS_DEV_ID_LEN);
		memcpy(&out[ZLRS_DEV_ID_LEN], a, ZLRS_DEV_ID_LEN);
	} else {
		memcpy(out, a, ZLRS_DEV_ID_LEN);
		memcpy(&out[ZLRS_DEV_ID_LEN], b, ZLRS_DEV_ID_LEN);
	}
}

static void ordered_pair_salt(const zlrs_link_t *link, uint8_t out[ZLRS_PAIR_SALT_LEN])
{
	uint32_t a = rd32(&link->pair_salt[0]);
	uint32_t b = rd32(&link->pair_salt[4]);

	if (a > b) {
		const uint32_t tmp = a;

		a = b;
		b = tmp;
	}
	wr32(&out[0], a);
	wr32(&out[4], b);
}

static void derive_auth(zlrs_link_t *link)
{
	zlrs_kdf(link->root_key, ZLRS_KEY_LEN, LABEL_AUTH, NULL, 0U, link->auth_key, ZLRS_KEY_LEN);
}

static void derive_pair(zlrs_link_t *link)
{
	uint8_t ctx[ZLRS_PAIR_CTX_LEN];
	uint8_t seed[4];
	uint8_t data_word[ZLRS_SYNC_WORD_LEN];
	uint32_t hop_seed;

	/*
	 * Both ids plus the two pairing nonces, sorted and concatenated: both ends get the same
	 * pair_key, and re-pairing changes the nonces, which replaces the whole subtree below
	 * (see doc/08-security.md#cryptographic-parameters).
	 */
	zlrs_link_pair_ctx(link, ctx);
	zlrs_kdf(link->root_key, ZLRS_KEY_LEN, LABEL_PAIR, ctx, sizeof(ctx), link->pair_key,
		 ZLRS_KEY_LEN);
	link_derive_by_endpoints(link, LABEL_LINK_AUTH, NULL, 0U, link->link_auth_out,
				 link->link_auth_in);

	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, LABEL_FHSS, NULL, 0U, seed, sizeof(seed));
	hop_seed = rd32(seed);
	zlrs_fhss_init(&link->fhss, link->cfg.domain, hop_seed);

	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, LABEL_SYNC_DATA, NULL, 0U, data_word,
		 sizeof(data_word));

	memcpy(link->sync_word, data_word, ZLRS_SYNC_WORD_LEN);

	link->session_valid = false;
	link->session_pending = false;
	link->sess_resp_due = false;
	link->sess_cfm_due = false;
	link->sess_ack_due = false;
	link->last_rx_seq = 0U;
	link->sess_rx_seq = 0U;
	link->fhss.index = 0U;
}

static void pair_payload_fill(const zlrs_link_t *link, zlrs_pair_frame_t *out,
			      uint32_t nonce_a, uint32_t nonce_b)
{
	const bool initiator = (link->cfg.role == ZLRS_ROLE_TX);
	const uint8_t *dev_a = initiator ? link->dev_id : link->peer_id;
	const uint8_t *dev_b = initiator ? link->peer_id : link->dev_id;

	memcpy(out->dev_a, dev_a, ZLRS_DEV_ID_LEN);
	memcpy(out->dev_b, dev_b, ZLRS_DEV_ID_LEN);
	out->nonce_a = nonce_a;
	out->nonce_b = nonce_b;
}

static const zlrs_radio_req_t *send_pair(zlrs_link_t *link, uint8_t type, uint32_t nonce_a,
					 uint32_t nonce_b, uint32_t now_ms)
{
	zlrs_frame_t frame = {.type = type};
	uint16_t payload_len;

	switch (type) {
	case ZLRS_PKT_PAIR_REQ:
		link->stats.pair_req_tx++;
		memcpy(frame.body.pair_req.dev_a, link->dev_id, ZLRS_DEV_ID_LEN);
		frame.body.pair_req.nonce_a = nonce_a;
		payload_len = ZLRS_PAIR_REQ_PAYLOAD_LEN;
		break;
	case ZLRS_PKT_PAIR_RESP:
		link->stats.pair_resp_tx++;
		pair_payload_fill(link, &frame.body.pair, nonce_a, nonce_b);
		payload_len = ZLRS_PAIR_PAYLOAD_LEN;
		break;
	case ZLRS_PKT_PAIR_CFM:
		link->stats.pair_cfm_tx++;
		pair_payload_fill(link, &frame.body.pair, nonce_a, nonce_b);
		payload_len = ZLRS_PAIR_PAYLOAD_LEN;
		break;
	default:
		return NULL;
	}

	const uint16_t len = link_build_frame(link, &frame, payload_len,
					     link_tag_key_for(link, type, true));
	link->last_pair_tx_ms = now_ms;

	return link_submit_tx(link, len, link_sync_freq_hz(link), ZLRS_SYNC_SLOT_PAIRING,
			      pairing_sync_word);
}

static const zlrs_radio_req_t *pair_rx(zlrs_link_t *link, uint16_t timeout_ms)
{
	return link_submit_rx(link, link_sync_freq_hz(link), ZLRS_SYNC_SLOT_PAIRING,
			      pairing_sync_word, timeout_ms);
}

static void pairing_complete(zlrs_link_t *link, uint32_t now_ms)
{
	/* The two handshake nonces are this pair's material: keep them to recompute pair_key */
	wr32(&link->pair_salt[0], link->pair_nonce);   /* nonce_a: initiator */
	wr32(&link->pair_salt[4], link->pair_nonce_b); /* nonce_b: responder */

	derive_pair(link);
	link->paired = true;
	link->pair_cfm_pending = false;
	link->pair_cfm_sent = 0U;
	link->pair_wait_cfm = false;
	link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
	link->last_rx_ms = now_ms;
}

static bool pair_reject(zlrs_link_t *link)
{
	link->stats.pair_rejects++;

	return false;
}

bool link_handle_pair_frame(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	const zlrs_pair_frame_t *const pair = &frame->body.pair;
	const bool initiator = (link->cfg.role == ZLRS_ROLE_TX);

	switch (frame->type) {
	case ZLRS_PKT_PAIR_REQ:
		link->stats.pair_req_rx++;
		/*
		 * Responder: remember the initiator's id and nonce, answer PAIR_RESP next slot. The
		 * responder nonce is only drawn once per handshake: the initiator repeats PAIR_REQ until
		 * it hears the response, and a fresh nonce per repeat would make the confirmation it
		 * finally sends fail the check below (the reply it answered had another nonce).
		 */
		if (initiator || link->state != ZLRS_STATE_PAIRING ||
		    zlrs_ct_equal(frame->body.pair_req.dev_a, link->dev_id, ZLRS_DEV_ID_LEN)) {
			return pair_reject(link);
		}
		const bool same_request = link->pair_request_valid &&
			link->pair_nonce == frame->body.pair_req.nonce_a &&
			zlrs_ct_equal(link->peer_id, frame->body.pair_req.dev_a, ZLRS_DEV_ID_LEN);
		if (same_request && frame->seq <= link->pair_rx_seq) {
			link->stats.rx_dup++;
			return false;
		}
		if (!same_request) {
			uint32_t nonce_b;
			if (!link_new_nonce(link, link->pair_nonce_b, link->pair_nonce_valid, &nonce_b)) {
				return pair_reject(link);
			}
			memcpy(link->peer_id, frame->body.pair_req.dev_a, ZLRS_DEV_ID_LEN);
			link->pair_nonce = frame->body.pair_req.nonce_a;
			link->pair_nonce_b = nonce_b;
			link->pair_nonce_valid = true;
			link->pair_tx_seq = 1U;
			link->pair_request_valid = true;
		}
		link->pair_rx_seq = frame->seq;
		link->pair_wait_cfm = true;

		return true;

	case ZLRS_PKT_PAIR_RESP:
		link->stats.pair_resp_rx++;
		/* Initiator: confirm both its id and its nonce came back */
		if (!initiator || !link->pair_request_valid) {
			return pair_reject(link);
		}
		if (!zlrs_ct_equal(pair->dev_a, link->dev_id, ZLRS_DEV_ID_LEN) ||
		    pair->nonce_a != link->pair_nonce ||
		    zlrs_ct_equal(pair->dev_b, link->dev_id, ZLRS_DEV_ID_LEN) ||
		    (link->pair_rx_seq != 0U &&
		     (!zlrs_ct_equal(pair->dev_b, link->peer_id, ZLRS_DEV_ID_LEN) ||
		      pair->nonce_b != link->pair_nonce_b))) {
			return pair_reject(link);
		}
		if (frame->seq <= link->pair_rx_seq) {
			link->stats.rx_dup++;
			return false;
		}
		link->pair_rx_seq = frame->seq;
		memcpy(link->peer_id, pair->dev_b, ZLRS_DEV_ID_LEN);
		link->pair_nonce_b = pair->nonce_b;
		link->last_pair_rx_ms = now_ms;
		link->pair_cfm_pending = true;
		return true;

	case ZLRS_PKT_PAIR_CFM:
		link->stats.pair_cfm_rx++;
		/* Responder: both nonces are back, pairing is done */
		if (initiator || !link->pair_request_valid) {
			return pair_reject(link);
		}
		if (!zlrs_ct_equal(pair->dev_a, link->peer_id, ZLRS_DEV_ID_LEN) ||
		    !zlrs_ct_equal(pair->dev_b, link->dev_id, ZLRS_DEV_ID_LEN) ||
		    pair->nonce_a != link->pair_nonce || pair->nonce_b != link->pair_nonce_b) {
			return pair_reject(link);
		}
		if (frame->seq <= link->pair_rx_seq) {
			link->stats.rx_dup++;
			return false;
		}
		link->pair_rx_seq = frame->seq;
		if (!link->pair_wait_cfm) {
			/* A repeated confirmation must not rederive keys or reset a searching session. */
			return link->paired && link->state == ZLRS_STATE_SEARCHING;
		}
		link->pair_wait_cfm = false;
		pairing_complete(link, now_ms);
		return true;

	default:
		return pair_reject(link);
	}
}

const zlrs_radio_req_t *link_pair_slot(zlrs_link_t *link, bool down, uint32_t now_ms)
{
	const bool tx_side = link->cfg.role == ZLRS_ROLE_TX;

	if (down) {
		link->frame_index++;
	}
	if (link->state == ZLRS_STATE_PAIRING) {
		if (down) {
			if (tx_side) {
				if (link->pair_cfm_pending) {
					/*
					 * Confirmation repeats: the two ends share no slot phase while pairing, so
					 * a single one is a coin flip. The responder repeats its answer until it
					 * hears a confirm, and the confirm is repeated here for the same reason.
					 */
					if (link->pair_cfm_sent >= ZLRS_PAIR_CFM_REPEATS) {
						pairing_complete(link, now_ms);
						return NULL;
					}
					if ((uint32_t)(now_ms - link->last_pair_tx_ms) <
					    link->cfg.pair_period_ms) {
						return NULL;
					}
					const zlrs_radio_req_t *req =
						send_pair(link, ZLRS_PKT_PAIR_CFM, link->pair_nonce,
							  link->pair_nonce_b, now_ms);

					if (req != NULL) {
						link->pair_cfm_sent++;
					}
					return req;
				}
				if ((uint32_t)(now_ms - link->last_pair_tx_ms) <
				    link->cfg.pair_period_ms) {
					/* Nothing to send this slot: listen instead. The peer's phase is unknown, so
					 * every extra window is another chance to hear it. */
					return pair_rx(link, link_acq_timeout_ms(link));
				}
				return send_pair(link, ZLRS_PKT_PAIR_REQ, link->pair_nonce, 0U,
						 now_ms);
			}
			return pair_rx(link, link_acq_timeout_ms(link));
		}

		if (tx_side) {
			return pair_rx(link, link_acq_timeout_ms(link));
		}
		if (link->pair_wait_cfm) {
			if ((uint32_t)(now_ms - link->last_pair_tx_ms) >=
			    link->cfg.pair_period_ms) {
				return send_pair(link, ZLRS_PKT_PAIR_RESP, link->pair_nonce,
						 link->pair_nonce_b, now_ms);
			}
			/* Between repeats: listen, for the same reason as the initiator above */
			return pair_rx(link, link_acq_timeout_ms(link));
		}
		return pair_rx(link, link_acq_timeout_ms(link));
	}

	if (link->state == ZLRS_STATE_SEARCHING && tx_side && down) {
		if (link->pair_cfm_pending) {
			if (link->pair_cfm_sent >= ZLRS_PAIR_CFM_REPEATS) {
				link->pair_cfm_pending = false;
				return NULL;
			}
			if ((uint32_t)(now_ms - link->last_pair_tx_ms) >=
			    link->cfg.pair_period_ms) {
				const zlrs_radio_req_t *req =
					send_pair(link, ZLRS_PKT_PAIR_CFM, link->pair_nonce,
						  link->pair_nonce_b, now_ms);

				if (req != NULL) {
					link->pair_cfm_sent++;
				}
				return req;
			}
			return NULL;
		}
	}
	return NULL;
}

void zlrs_link_set_root_key(zlrs_link_t *link, const uint8_t root_key[ZLRS_KEY_LEN],
			    uint32_t now_ms)
{
	memcpy(link->root_key, root_key, ZLRS_KEY_LEN);
	link->configured = true;
	derive_auth(link);

	if (link->paired) {
		derive_pair(link);
		link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
	} else {
		link_set_state(link, ZLRS_STATE_UNPAIRED, now_ms);
	}
}

void zlrs_link_set_dev_id(zlrs_link_t *link, const uint8_t dev_id[ZLRS_DEV_ID_LEN])
{
	memcpy(link->dev_id, dev_id, ZLRS_DEV_ID_LEN);
}

void zlrs_link_pair_ctx(const zlrs_link_t *link, uint8_t out[ZLRS_PAIR_CTX_LEN])
{
	ordered_ids(link, out);
	ordered_pair_salt(link, &out[2U * ZLRS_DEV_ID_LEN]);
}

void zlrs_link_set_peer(zlrs_link_t *link, const uint8_t peer_id[ZLRS_DEV_ID_LEN],
			const uint8_t pair_salt[ZLRS_PAIR_SALT_LEN], uint32_t now_ms)
{
	if (zlrs_ct_equal(peer_id, link->dev_id, ZLRS_DEV_ID_LEN)) {
		return;
	}
	memcpy(link->peer_id, peer_id, ZLRS_DEV_ID_LEN);
	memcpy(link->pair_salt, pair_salt, ZLRS_PAIR_SALT_LEN);
	link->pair_nonce = rd32(pair_salt);
	link->pair_nonce_b = rd32(pair_salt + 4U);
	link->pair_nonce_valid = true;
	link->paired = true;

	if (link->configured) {
		derive_pair(link);
		link_set_state(link, ZLRS_STATE_SEARCHING, now_ms);
	}
}

void zlrs_link_press(zlrs_link_t *link, uint32_t now_ms)
{
	if (!link->configured) {
		return;
	}

	if (link->cfg.role == ZLRS_ROLE_TX) {
		uint32_t nonce_a;
		if (!link_new_nonce(link, link->pair_nonce, link->pair_nonce_valid, &nonce_a)) {
			return;
		}
		link->pair_nonce = nonce_a;
		link->pair_nonce_valid = true;
	}
	link->pair_cfm_pending = false;
	link->pair_cfm_sent = 0U;
	link->pair_wait_cfm = false;
	link->pair_request_valid = link->cfg.role == ZLRS_ROLE_TX;
	link->pair_rx_seq = 0U;
	link->pair_tx_seq = 1U;
	link->prepared_data.valid = false;
	link->session_valid = false;
	link->session_pending = false;
	link_set_state(link, ZLRS_STATE_PAIRING, now_ms);
}

bool zlrs_link_cancel_pair(zlrs_link_t *link, uint32_t now_ms)
{
	if (link->state != ZLRS_STATE_PAIRING) {
		return false;
	}
	link->pair_cfm_pending = false;
	link->pair_wait_cfm = false;
	link->session_valid = false;
	link->session_pending = false;
	link->sess_resp_due = false;
	link->sess_cfm_due = false;
	link_set_state(link, link->paired ? ZLRS_STATE_SEARCHING : ZLRS_STATE_UNPAIRED, now_ms);
	return true;
}

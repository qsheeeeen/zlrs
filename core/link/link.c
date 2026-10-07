/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/link/link.h"

#include <string.h>

/* Public sync word used while pairing, slot 3 */
static const uint8_t pairing_sync_word[ZLRS_SYNC_WORD_LEN] = {'Z', 'L', 'R', 'S'};

/* Derivation labels */
#define LABEL_PAIR              "zlrs/pair"
#define LABEL_AUTH              "zlrs/auth"
#define LABEL_FHSS              "zlrs/fhss"
#define LABEL_SYNC_CONTROL      "zlrs/sync/control"
#define LABEL_SYNC_DATA         "zlrs/sync/data"
#define LABEL_SYNC_LORA_CONTROL "zlrs/sync/lora/control"
#define LABEL_SYNC_LORA_DATA    "zlrs/sync/lora/data"
#define LABEL_LINK_AUTH         "zlrs/link-auth"
#define LABEL_SESSION           "zlrs/session"

/* Pairing payload length: dev_a(4) + dev_b(4) + nonce_a(4) + nonce_b(4) */
/*
 * Pairing payload: both dev_ids (8) + two nonces (8) + flags (1) + failsafe values (22).
 * Only the initiator (TX) fills in the failsafe values: it sets FLAG_FAILSAFE when it has
 * any and the responder stores them; without the flag the responder keeps its own instead
 */
#define PAIR_PAYLOAD_LEN     ZLRS_PAIR_PAYLOAD_LEN
#define PAIR_FLAGS_OFFSET    16U
#define PAIR_FAILSAFE_OFFSET 17U
#define PAIR_FLAG_FAILSAFE   0x01U

static void derive_session_keys(zlrs_link_t *link, uint32_t nonce_t, uint32_t nonce_r);
static void session_activate(zlrs_link_t *link, uint16_t hop_index);
static void roll_session(zlrs_link_t *link, uint32_t now_ms);
static void lq_account(zlrs_link_t *link, uint16_t seq);

/*
 * Start a fresh reassembly. Only ARQ can hand the reassembler every frame of a run, so only ARQ
 * may keep a half run alive indefinitely (see zlrs_reasm_set_expiry).
 */
static void reasm_restart(zlrs_link_t *link)
{
	zlrs_reasm_init(&link->reasm, link->reasm_buf, sizeof(link->reasm_buf));
	zlrs_reasm_set_expiry(&link->reasm, link->cfg.arq == 0U);
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t link_random(const zlrs_link_t *link, uint32_t salt)
{
	if (link->random != NULL) {
		return link->random(link->random_ctx);
	}

	/* Without a random source, mix time and a counter into a non-zero value: enough to exercise the flow */
	return (salt * 2654435761U) ^ 0x9e3779b9U;
}

/*
 * Sync channel frequency. While pairing there is no derived key and no hop table yet, so
 * look the band table up directly: this is the fixed meeting point for pairing and SYNC.
 */
static uint32_t sync_freq_hz(const zlrs_link_t *link)
{
	const zlrs_fhss_domain_t *domain = &zlrs_fhss_domains[link->cfg.domain];
	const uint8_t channel = (uint8_t)(domain->channel_count / 2U);

	return domain->start_hz + (uint32_t)channel * domain->step_hz;
}

/*
 * Is this frame a resync frame (doc/08-protocol.md 5.1)? While connected one arrives
 * every cfg.resync_every_n frames and the whole frame stays on the sync channel: SYNC
 * goes out downlink, the uplink leaves room for a SESS_RESP. Both ends count frame_index.
 */
static bool sync_frame(const zlrs_link_t *link)
{
	return link->cfg.resync_every_n > 0U &&
	       (link->frame_index % link->cfg.resync_every_n) == 0U;
}

/*
 * Which frequency this frame uses: resync frames and frames that still owe a session
 * confirm fall back to the sync channel, everything else follows the hop table.
 */
/* Slot kinds, used to order sub-band moves (telemetry, then data, then RC; 08-protocol 5.1) */
enum slot_kind {
	SLOT_RC = 0,
	SLOT_TLM,
	SLOT_DATA,
};

/*
 * Should this slot land on sub-G? Four mix levels move 0/1/2/4 of every 4 slots, always in
 * the same order: telemetry first, then data, the RC downlink last.
 */
static bool slot_in_subg(const zlrs_link_t *link, uint8_t kind)
{
	const uint8_t k = zlrs_fhss_mix_slots(link->sub_mix);

	if (k == 0U || !zlrs_fhss_sub_ready(&link->fhss)) {
		return false;
	}
	if (k >= 4U) {
		return true; /* 100%: the whole link falls back to sub-G */
	}
	if (kind == SLOT_TLM) {
		return true; /* 25%: telemetry moves first */
	}
	if (k >= 2U && kind == SLOT_DATA) {
		return true; /* 50%: data moves next */
	}

	return false; /* the RC downlink only moves at 100% */
}

static uint32_t slot_freq_hz(const zlrs_link_t *link, uint8_t kind)
{
	if (sync_frame(link) || link->sess_cfm_due || link->sess_ack_due) {
		return sync_freq_hz(link);
	}
	/* Both ends count from the confirmed frame, including repeated confirms after lost ACKs.
	 * Keep initial DATA on the same channel where the peer is waiting for confirmation. */
	if (link->cfg.kind == ZLRS_LINK_DATA && link->cfg.handshake && link->session_valid &&
	    (uint32_t)(link->frame_index - link->sess_sync_frame) < ZLRS_DATA_CONFIRM_FRAMES) {
		return sync_freq_hz(link);
	}
	/* Both ends keep the fresh-session uplink on the confirmation channel. */
	if ((kind == SLOT_TLM || (kind == SLOT_DATA && link->slot == ZLRS_SLOT_DATA_B)) &&
	    (uint32_t)(link->slot_ms - link->state_ms) < ZLRS_SESS_CONFIRM_MS) {
		return sync_freq_hz(link);
	}
	if (slot_in_subg(link, kind)) {
		return zlrs_fhss_sub_current_hz(&link->fhss);
	}

	return zlrs_fhss_current_hz(&link->fhss);
}

static void set_state(zlrs_link_t *link, uint8_t state, uint32_t now_ms)
{
	if (link->state == state) {
		return;
	}

	link->state = state;
	link->state_ms = now_ms;
	/* The simplified handshake's "I am here" only means something while CONNECTED */
	if (state != ZLRS_STATE_CONNECTED) {
		link->announce = false;
	}
	if (link->ev.on_state != NULL) {
		link->ev.on_state(link->ev.ctx, state);
	}

	/* The TX is the authority: every return to SEARCHING rolls a new session nonce */
	if (state == ZLRS_STATE_SEARCHING && link->cfg.role == ZLRS_ROLE_TX) {
		roll_session(link, now_ms);
		/* Send a SYNC right away instead of waiting a full period, to shorten the reconnect */
		link->last_sync_ms = (uint32_t)(now_ms - link->cfg.sync_period_ms);
	}
}

/*
 * Every time the TX enters SEARCHING it draws a new nonce_t and drops the old session.
 * The session key only exists once the RX answers with nonce_r (doc/08-protocol.md 3.5).
 */
static void roll_session(zlrs_link_t *link, uint32_t now_ms)
{
	link->sess_nonce_t = link_random(link, now_ms);
	link->session_valid = false;
	link->session_pending = false;
	link->sess_cfm_due = false;
	link->sess_resp_due = false;
	link->sess_ack_due = false;
	link->announce = false;
}

/* Concatenate the two dev_ids in numeric order so both ends derive the same pair_key */
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

/* The pairing nonces are sorted numerically too, so who initiated does not matter */
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

/*
 * Keys are directional: KDF(pair_key, label, ctx || local || peer) tags outgoing frames and
 * the swapped order verifies incoming ones. The order encodes direction; each end derives both.
 * Without that, our own frame echoed back would be taken for the peer's (reflection attack).
 */
static void derive_by_endpoints(const zlrs_link_t *link, const char *label, const uint8_t *ctx,
				size_t ctx_len, uint8_t out[ZLRS_KEY_LEN], uint8_t in[ZLRS_KEY_LEN])
{
	uint8_t ids_ctx[2U * sizeof(uint32_t) + 2U * ZLRS_DEV_ID_LEN];
	const size_t len = ctx_len + 2U * ZLRS_DEV_ID_LEN;

	if (len > sizeof(ids_ctx)) {
		return;
	}
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

static void derive_pair(zlrs_link_t *link)
{
	uint8_t ctx[ZLRS_PAIR_CTX_LEN];
	uint8_t seed[4];
	uint8_t ctrl_word[ZLRS_SYNC_WORD_LEN];
	uint8_t data_word[ZLRS_SYNC_WORD_LEN];
	uint32_t hop_seed;

	/*
	 * Both ids plus the two pairing nonces, sorted and concatenated: both ends get the same
	 * pair_key, and re-pairing changes the nonces, which replaces the whole subtree below
	 * (see doc/09-security.md 3).
	 */
	zlrs_link_pair_ctx(link, ctx);
	zlrs_kdf(link->root_key, ZLRS_KEY_LEN, LABEL_PAIR, ctx, sizeof(ctx), link->pair_key,
		 ZLRS_KEY_LEN);
	derive_by_endpoints(link, LABEL_LINK_AUTH, NULL, 0U, link->link_auth_out,
			    link->link_auth_in);

	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, LABEL_FHSS, NULL, 0U, seed, sizeof(seed));
	hop_seed = rd32(seed);
	if (link->cfg.second_domain != 0xFFU) {
		zlrs_fhss_init_dual(&link->fhss, link->cfg.domain, link->cfg.second_domain,
				    hop_seed);
	} else {
		zlrs_fhss_init(&link->fhss, link->cfg.domain, hop_seed);
	}

	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, LABEL_SYNC_CONTROL, NULL, 0U, ctrl_word,
		 sizeof(ctrl_word));
	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, LABEL_SYNC_DATA, NULL, 0U, data_word,
		 sizeof(data_word));

	memcpy(link->sync_word, (link->cfg.kind == ZLRS_LINK_CONTROL) ? ctrl_word : data_word,
	       ZLRS_SYNC_WORD_LEN);

	/*
	 * LoRa sync words are one byte plus a private bit, so the 4-byte one above cannot be
	 * reused: a separate label derives them (still named by role, not by band). The top bit
	 * is the private bit separating our group from public sync words (08-protocol 5.3).
	 */
	{
		const char *label = (link->cfg.kind == ZLRS_LINK_CONTROL) ? LABEL_SYNC_LORA_CONTROL
									  : LABEL_SYNC_LORA_DATA;
		uint8_t byte = 0U;

		zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, label, NULL, 0U, &byte, sizeof(byte));
		link->lora_sync = (uint8_t)(byte | 0x80U);
		if (link->cfg.mod.modulation == (uint8_t)ZLRS_MOD_LORA) {
			link->cfg.mod.sync_byte = link->lora_sync;
		}
	}

	link->session_valid = false;
	link->session_pending = false;
	link->sess_resp_due = false;
	link->sess_cfm_due = false;
	link->sess_ack_due = false;
	/*
	 * Simplified handshake (only disabled for bench work): no session key is derived, the
	 * pairing-level link_auth is the session key and one SESS_RESP answers a SYNC (09-security 3).
	 */
	if (link->cfg.handshake == 0U) {
		memcpy(link->session_key_out, link->link_auth_out, ZLRS_KEY_LEN);
		memcpy(link->session_key_in, link->link_auth_in, ZLRS_KEY_LEN);
		link->session_valid = true;
	}
	link->last_rx_seq = 0xFFFFU;
	link->fhss.index = 0U;
}

/* Session key = KDF(pair_key, "zlrs/session", nonce_t || nonce_r || both ids) */
static void derive_session_keys(zlrs_link_t *link, uint32_t nonce_t, uint32_t nonce_r)
{
	uint8_t ctx[2U * sizeof(uint32_t)];

	wr32(&ctx[0], nonce_t);
	wr32(&ctx[4], nonce_r);
	derive_by_endpoints(link, LABEL_SESSION, ctx, sizeof(ctx), link->session_key_out,
			    link->session_key_in);
}

/*
 * Adopt this session: the key takes effect now and sequence numbers, ARQ and the LQ window
 * restart (in-flight data from the old session is void). TX calls it before SESS_CFM, RX after.
 */
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
	/*
	 * A half message from the old session must not be stitched onto the new one: the peer's
	 * sequence numbers restart, so what arrives next belongs to a different stream.
	 */
	reasm_restart(link);
	/* Sequence numbers restart per session; the LQ window must not span sessions */
	link->lq_last_seq = 0U;
	link->stats.lq_rx = 0U;
	link->stats.lq_expected = 0U;
	/* Session reset releases the outstanding packet; its bytes stay queued for retransmission. */
	link->arq_used = 0U;
	link->arq_run_bytes = 0U;
	link->arq_retries = 0U;
	link->arq_seq = 0U;
	link->arq_rx_started = false;
	link->arq_ack_pending = false;
	zlrs_fhss_set_index(&link->fhss, hop_index);
}

static uint16_t encode_frame(const zlrs_link_t *link, uint8_t *buf, uint16_t seq, uint8_t type,
			     uint8_t flags, const uint8_t *payload, uint16_t len,
			     const uint8_t *tag_key)
{
	const uint8_t tag_len = zlrs_profile_tag_len(link->cfg.profile);
	zlrs_frame_t frame;
	uint16_t total;

	if (link->cfg.profile == ZLRS_PROFILE_P2) {
		return 0U; /* P2 payload encryption is not done here, so a P2 link sends nothing */
	}
	if (len > ZLRS_PAYLOAD_MAX_LEN) {
		return 0U;
	}

	total = zlrs_frame_len(len, tag_len);
	if (total > sizeof(link->tx_buf) ||
	    (link->cfg.mod.modulation == ZLRS_MOD_LORA && total > ZLRS_LORA_FRAME_MAX_LEN)) {
		return 0U;
	}

	frame.type = type;
	frame.flags = flags;
	frame.seq = seq;
	frame.payload = payload;
	frame.payload_len = len;
	frame.tag = NULL;

	/* encode writes header + payload only; the tag is appended here */
	if (zlrs_frame_encode(&frame, buf, ZLRS_FRAME_MAX_LEN) !=
	    (uint16_t)(ZLRS_FRAME_HDR_LEN + len)) {
		return 0U;
	}

	if (tag_len > 0U) {
		const uint16_t mac_len = (uint16_t)(ZLRS_FRAME_HDR_LEN + len);

		zlrs_tag(tag_key, ZLRS_KEY_LEN, buf, mac_len, &buf[mac_len]);
	}
	return total;
}

static uint16_t build_frame(zlrs_link_t *link, uint8_t type, uint8_t flags, const uint8_t *payload,
			    uint16_t len, const uint8_t *tag_key)
{
	link->prepared_data.valid = false;
	const uint16_t total =
		encode_frame(link, link->tx_buf, link->tx_seq, type, flags, payload, len, tag_key);
	if (total > 0U) {
		link->tx_seq++;
	}
	return total;
}

static bool receive_ack_bit(const zlrs_link_t *link)
{
	return link->arq_rx_started && ((link->arq_rx_seq + 1U) & 1U) != 0U;
}

/* Tag key for a frame: outbound uses out, otherwise in. The pairing handshake is the
 * exception: role gating and type labels carry direction, both ends share one auth_key. */
static const uint8_t *tag_key_for(const zlrs_link_t *link, uint8_t type, bool outbound)
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
	const uint16_t rx_cap = link->cfg.mod.modulation == ZLRS_MOD_LORA ? ZLRS_LORA_FRAME_MAX_LEN
									  : ZLRS_FRAME_MAX_LEN;

	link->req.max_rx_size = is_tx ? 0U : rx_cap;
	link->req.timeout_ms = timeout_ms;
	link->req.buf = is_tx ? link->tx_buf : link->rx_buf;
	link->req_is_tx = is_tx;
	link->req_pending = true;

	return &link->req;
}

static const zlrs_radio_req_t *submit_tx(zlrs_link_t *link, uint16_t len, uint32_t freq_hz,
					 uint8_t sync_slot,
					 const uint8_t sync_word[ZLRS_SYNC_WORD_LEN])
{
	if (len == 0U) {
		return NULL;
	}

	return make_req(link, true, len, freq_hz, sync_slot, sync_word, 0U);
}

static const zlrs_radio_req_t *submit_rx(zlrs_link_t *link, uint32_t freq_hz, uint8_t sync_slot,
					 const uint8_t sync_word[ZLRS_SYNC_WORD_LEN],
					 uint16_t timeout_ms)
{
	return make_req(link, false, 0U, freq_hz, sync_slot, sync_word, timeout_ms);
}

/* ---- transmit: one builder per frame type -------------------------------- */

static const zlrs_radio_req_t *send_sync(zlrs_link_t *link, uint32_t now_ms)
{
	uint8_t payload[12];
	uint16_t len;

	payload[0] = ZLRS_PROTO_VERSION;
	payload[1] = (uint8_t)((uint8_t)link->cfg.profile | (uint8_t)(link->sub_mix << 4) |
			       (uint8_t)(link->sub_mix_version << 6));
	payload[2] = (uint8_t)(link->fhss.index & 0xFFU);
	payload[3] = (uint8_t)(link->fhss.index >> 8);
	wr32(&payload[4], link->sess_nonce_t);
	wr32(&payload[8], link->frame_index);

	len = build_frame(link, ZLRS_PKT_SYNC, 0U, payload, sizeof(payload),
			  tag_key_for(link, ZLRS_PKT_SYNC, true));
	link->last_sync_ms = now_ms;
	link->stats.tx_syncs++;

	/* SYNC always goes out on the sync channel */
	return submit_tx(link, len, sync_freq_hz(link), link->cfg.sync_slot, link->sync_word);
}

/*
 * RX answers a SYNC with its nonce_r and echoes nonce_t, authenticated with the pairing key
 * (the session key does not exist yet). With handshake=0 it is also the "I am here" report.
 */
static const zlrs_radio_req_t *send_sess_resp(zlrs_link_t *link)
{
	uint8_t payload[3U * sizeof(uint32_t) + sizeof(uint16_t)];
	uint16_t len;

	wr32(&payload[0], link->sess_nonce_t);
	wr32(&payload[4], link->sess_nonce_r);
	payload[8] = (uint8_t)link->fhss.index;
	payload[9] = (uint8_t)(link->fhss.index >> 8);
	wr32(&payload[10], link->frame_index);
	len = build_frame(link, ZLRS_PKT_SESS_RESP, 0U, payload, sizeof(payload),
			  tag_key_for(link, ZLRS_PKT_SESS_RESP, true));
	link->sess_resp_due = false;
	link->stats.sess_resp_tx++;

	/* Still on the sync channel: the TX is in SEARCHING and listens there */
	return submit_tx(link, len, sync_freq_hz(link), link->cfg.sync_slot, link->sync_word);
}

/*
 * The TX's session confirm: tagged with the freshly derived session key and echoing nonce_r,
 * which tells the RX the key matches. Losing it is fine: the RX repeats its response on the
 * next SYNC and the confirm goes out again.
 */
static const zlrs_radio_req_t *send_sess_cfm(zlrs_link_t *link)
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
	len = build_frame(link, ZLRS_PKT_SESS_CFM, 0U, payload, sizeof(payload),
			  tag_key_for(link, ZLRS_PKT_SESS_CFM, true));
	/* DATA confirmation remains due until the peer proves that it has this session key.
	 * Building or even transmitting the confirm does not establish that it arrived. */
	if (link->cfg.kind != ZLRS_LINK_DATA) {
		link->sess_cfm_due = false;
	}
	link->stats.sess_cfm_tx++;

	return submit_tx(link, len, sync_freq_hz(link), link->cfg.sync_slot, link->sync_word);
}

static const zlrs_radio_req_t *send_rc(zlrs_link_t *link)
{
	uint16_t len = build_frame(link, ZLRS_PKT_RC, 0U, link->rc_payload, link->rc_len,
				   tag_key_for(link, ZLRS_PKT_RC, true));

	return submit_tx(link, len, slot_freq_hz(link, SLOT_RC), link->cfg.sync_slot,
			 link->sync_word);
}

static const zlrs_radio_req_t *send_tlm(zlrs_link_t *link)
{
	uint16_t len = build_frame(link, ZLRS_PKT_TLM, 0U, link->tlm_payload, link->tlm_len,
				   tag_key_for(link, ZLRS_PKT_TLM, true));

	return submit_tx(link, len, slot_freq_hz(link, SLOT_TLM), link->cfg.sync_slot,
			 link->sync_word);
}

/* ---- ARQ (optional data-link mode, see doc/08-protocol.md 4.3) ------------- */

static uint16_t arq_inflight_bytes(const zlrs_link_t *link)
{
	return link->arq_used != 0U ? link->arq_len : 0U;
}

/* Commit the one outstanding packet only after the peer's ACK toggle changes. */
static void arq_commit_ack(zlrs_link_t *link)
{
	link->queue_head = (uint16_t)((link->queue_head + link->arq_len) % ZLRS_LINK_TX_QUEUE_LEN);
	link->queue_count -= link->arq_len;
	link->arq_used = 0U;
	link->arq_seq++;
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

/* Send the outstanding packet: data sequence is retained across retransmissions. */
static const zlrs_radio_req_t *arq_send_slot(zlrs_link_t *link, uint16_t seq, uint16_t len,
					     bool more)
{
	uint8_t payload[ZLRS_PAYLOAD_MAX_LEN];
	const uint8_t type =
		(link->cfg.role == ZLRS_ROLE_TX) ? ZLRS_PKT_DATA_UP : ZLRS_PKT_DATA_DOWN;
	uint16_t frame_len;

	payload[0] = (uint8_t)(seq & 0xFFU);
	payload[1] = (uint8_t)(seq >> 8);
	queue_copy(link, 0U, payload + 2U, len);

	frame_len = build_frame(link, type,
				(more ? ZLRS_FLAG_MORE : 0U) |
					(receive_ack_bit(link) ? ZLRS_FLAG_ACK : 0U),
				payload, (uint16_t)(len + 2U), tag_key_for(link, type, true));
	if (frame_len == 0U) {
		return NULL;
	}

	/* ARQ bytes are committed on ACK, so keep the completion callback out of the queue */
	link->pending_data_len = 0U;
	link->arq_ack_pending = false;

	return submit_tx(link, frame_len, slot_freq_hz(link, SLOT_DATA), link->cfg.sync_slot,
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

static bool arq_next_data(const zlrs_link_t *link, uint16_t *take, bool *more)
{
	const uint16_t inflight = arq_inflight_bytes(link);
	const uint16_t mtu = link->cfg.data_mtu;

	if (link->queue_count <= inflight || mtu == 0U) {
		return false;
	}
	*take = link->queue_count - inflight;
	*more = *take > mtu;
	if (*more) {
		*take = mtu;
	}
	/* End a run before its next fragment could overflow the receiver's reassembly buffer. */
	if (*more && (uint32_t)link->arq_run_bytes + *take + mtu > ZLRS_REASM_BUF_LEN) {
		*more = false;
	}
	return true;
}

static void arq_commit_data(zlrs_link_t *link, uint16_t len, bool more, uint32_t now_ms)
{
	link->arq_run_bytes = more ? link->arq_run_bytes + len : 0U;
	link->arq_len = len;
	link->arq_first_sent_ms = now_ms;
	link->arq_more = more ? 1U : 0U;
	link->arq_used = 1U;
	link->arq_retries = 0U;
	link->arq_sent_ms = now_ms;
}

const uint8_t *zlrs_link_prepare_data(zlrs_link_t *link, uint8_t slot, uint32_t now_ms,
				      uint16_t *size)
{
	zlrs_prepared_data_t *packet = &link->prepared_data;
	const uint8_t type = link->cfg.role == ZLRS_ROLE_TX ? ZLRS_PKT_DATA_UP : ZLRS_PKT_DATA_DOWN;
	const uint32_t frame = link->frame_index + (slot == ZLRS_SLOT_DATA_A ? 1U : 0U);
	uint8_t payload[ZLRS_PAYLOAD_MAX_LEN];

	*size = 0U;
	/* The TX request may still refer to packet->buf; never overwrite it while on air. */
	if (link->req_pending && link->req_is_tx) {
		return NULL;
	}
	packet->valid = false;
	if (link->cfg.kind != ZLRS_LINK_DATA || !link->cfg.arq ||
	    link->cfg.mod.modulation != ZLRS_MOD_FLRC || link->cfg.data_mtu == 0U ||
	    link->state != ZLRS_STATE_CONNECTED || !link->session_valid ||
	    slot != (link->cfg.role == ZLRS_ROLE_TX ? ZLRS_SLOT_DATA_A : ZLRS_SLOT_DATA_B) ||
	    link->announce || link->sess_cfm_due ||
	    (link->cfg.resync_every_n > 0U && frame % link->cfg.resync_every_n == 0U) ||
	    frame % link->cfg.data_every_n != 0U) {
		return NULL;
	}
	packet->retry = arq_timed_out(link, now_ms);
	if (packet->retry) {
		packet->data_len = link->arq_len;
		packet->more = link->arq_more != 0U;
	} else {
		if (link->arq_used != 0U ||
		    !arq_next_data(link, &packet->data_len, &packet->more)) {
			return NULL;
		}
	}
	packet->ack_bit = receive_ack_bit(link);
	packet->frame_seq = link->tx_seq;
	packet->data_seq = (uint16_t)(link->arq_seq + (packet->retry ? 0U : link->arq_used));
	payload[0] = (uint8_t)packet->data_seq;
	payload[1] = (uint8_t)(packet->data_seq >> 8);
	const uint16_t offset = packet->retry ? 0U : arq_inflight_bytes(link);

	queue_copy(link, offset, payload + 2U, packet->data_len);
	memcpy(packet->key, tag_key_for(link, type, true), ZLRS_KEY_LEN);
	packet->size = encode_frame(link, packet->buf, packet->frame_seq, type,
				    (packet->more ? ZLRS_FLAG_MORE : 0U) |
					    (packet->ack_bit ? ZLRS_FLAG_ACK : 0U),
				    payload, packet->data_len + 2U, packet->key);
	if (packet->size == 0U) {
		return NULL;
	}
	packet->frame_index = frame;
	packet->data_mtu = link->cfg.data_mtu;
	packet->run_bytes = link->arq_run_bytes;
	packet->profile = link->cfg.profile;
	packet->valid = true;
	*size = packet->size;
	return packet->buf;
}

static const zlrs_radio_req_t *send_prepared_data(zlrs_link_t *link, uint32_t now_ms, bool retry)
{
	zlrs_prepared_data_t *packet = &link->prepared_data;
	const uint8_t type = link->cfg.role == ZLRS_ROLE_TX ? ZLRS_PKT_DATA_UP : ZLRS_PKT_DATA_DOWN;
	const uint16_t inflight = arq_inflight_bytes(link);
	const uint16_t data_seq = (uint16_t)(link->arq_seq + (retry ? 0U : link->arq_used));
	const bool data_valid =
		retry ? link->arq_used > 0U && packet->data_len == link->arq_len &&
				packet->more == (link->arq_more != 0U)
		      : link->arq_used == 0U && link->queue_count >= inflight + packet->data_len;

	if (!packet->valid || packet->retry != retry || !data_valid ||
	    packet->frame_index != link->frame_index || packet->frame_seq != link->tx_seq ||
	    packet->data_seq != data_seq || packet->data_mtu != link->cfg.data_mtu ||
	    packet->run_bytes != link->arq_run_bytes || packet->profile != link->cfg.profile ||
	    (packet->buf[0] >> 2) != type ||
	    !zlrs_ct_equal(packet->key, tag_key_for(link, type, true), ZLRS_KEY_LEN)) {
		packet->valid = false;
		return NULL;
	}
	const zlrs_radio_req_t *req = submit_tx(link, packet->size, slot_freq_hz(link, SLOT_DATA),
						link->cfg.sync_slot, link->sync_word);

	if (req == NULL) {
		return NULL;
	}
	link->req.buf = packet->buf;
	link->tx_seq++;
	if (!retry) {
		arq_commit_data(link, packet->data_len, packet->more, now_ms);
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

		/* Retransmit the head frame: same sequence number, length and MORE as the original */
		return arq_send_slot(link, link->arq_seq, link->arq_len, link->arq_more != 0U);
	}

	const zlrs_radio_req_t *req = send_prepared_data(link, now_ms, false);
	uint16_t take;
	bool more;

	if (req == NULL && link->arq_used != 0U) {
		link->stats.tx_arq_wait_stalls++;
	}
	if (req != NULL || link->arq_used != 0U || !arq_next_data(link, &take, &more)) {
		return req;
	}
	req = arq_send_slot(link, link->arq_seq, take, more);
	if (req != NULL) {
		arq_commit_data(link, take, more, now_ms);
	}
	return req;
}

/* Empty DATA reports the same receive toggle as every data-bearing DATA packet. */
static const zlrs_radio_req_t *send_empty_data(zlrs_link_t *link)
{
	const uint8_t type = link->cfg.role == ZLRS_ROLE_TX ? ZLRS_PKT_DATA_UP : ZLRS_PKT_DATA_DOWN;
	const uint16_t len = build_frame(link, type, receive_ack_bit(link) ? ZLRS_FLAG_ACK : 0U,
					 NULL, 0U, tag_key_for(link, type, true));
	if (len == 0U) {
		return NULL;
	}
	link->arq_ack_pending = false;
	link->pending_data_len = 0U;
	return submit_tx(link, len, slot_freq_hz(link, SLOT_DATA), link->cfg.sync_slot,
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

static const zlrs_radio_req_t *send_data(zlrs_link_t *link, uint32_t now_ms)
{
	uint8_t payload[ZLRS_PAYLOAD_MAX_LEN];
	uint16_t take = link->queue_count;
	uint16_t len;
	uint8_t flags = 0U;
	uint8_t type;

	if (link->cfg.arq) {
		return send_data_arq(link, now_ms);
	}

	if (take == 0U) {
		return NULL;
	}
	if (take > link->cfg.data_mtu) {
		take = link->cfg.data_mtu;
		flags = ZLRS_FLAG_MORE;
	}
	/*
	 * Same run limit as the ARQ path: the receiver assembles until it sees a frame without MORE and
	 * can only hold ZLRS_REASM_BUF_LEN bytes, so a longer run has to be cut into separate messages
	 * here instead of arriving as one run the peer has to throw away.
	 */
	if ((flags & ZLRS_FLAG_MORE) != 0U &&
	    ((uint32_t)link->arq_run_bytes + take + link->cfg.data_mtu > ZLRS_REASM_BUF_LEN)) {
		flags = 0U;
	}
	link->arq_run_bytes =
		((flags & ZLRS_FLAG_MORE) != 0U) ? (uint16_t)(link->arq_run_bytes + take) : 0U;

	queue_copy(link, 0U, payload, take);

	type = (link->cfg.role == ZLRS_ROLE_TX) ? ZLRS_PKT_DATA_UP : ZLRS_PKT_DATA_DOWN;
	len = build_frame(link, type, flags, payload, take, tag_key_for(link, type, true));
	link->pending_data_len = take;

	return submit_tx(link, len, slot_freq_hz(link, SLOT_DATA), link->cfg.sync_slot,
			 link->sync_word);
}

/*
 * The pairing payload is fixed as dev_a (initiator) | dev_b (responder) | nonce_a | nonce_b:
 * both ends fill their own role, the field order never changes.
 */
static void pair_payload_fill(const zlrs_link_t *link, uint8_t out[PAIR_PAYLOAD_LEN],
			      uint32_t nonce_a, uint32_t nonce_b)
{
	const bool initiator = (link->cfg.role == ZLRS_ROLE_TX);
	const uint8_t *dev_a = initiator ? link->dev_id : link->peer_id;
	const uint8_t *dev_b = initiator ? link->peer_id : link->dev_id;

	memset(out, 0, PAIR_PAYLOAD_LEN);
	memcpy(out, dev_a, ZLRS_DEV_ID_LEN);
	memcpy(&out[ZLRS_DEV_ID_LEN], dev_b, ZLRS_DEV_ID_LEN);
	wr32(&out[2U * ZLRS_DEV_ID_LEN], nonce_a);
	wr32(&out[2U * ZLRS_DEV_ID_LEN + 4U], nonce_b);

	/* The initiator ships its failsafe values along, the responder stores them */
	if (initiator && link->failsafe_len == ZLRS_RC_PAYLOAD_LEN) {
		out[PAIR_FLAGS_OFFSET] = PAIR_FLAG_FAILSAFE;
		memcpy(&out[PAIR_FAILSAFE_OFFSET], link->failsafe, ZLRS_RC_PAYLOAD_LEN);
	}
}

static const zlrs_radio_req_t *send_pair(zlrs_link_t *link, uint8_t type, uint32_t nonce_a,
					 uint32_t nonce_b, uint32_t now_ms)
{
	uint8_t payload[PAIR_PAYLOAD_LEN];
	uint16_t len;

	switch (type) {
	case ZLRS_PKT_PAIR_REQ:
		link->stats.pair_req_tx++;
		break;
	case ZLRS_PKT_PAIR_RESP:
		link->stats.pair_resp_tx++;
		break;
	case ZLRS_PKT_PAIR_CFM:
		link->stats.pair_cfm_tx++;
		break;
	default:
		break;
	}

	pair_payload_fill(link, payload, nonce_a, nonce_b);
	len = build_frame(link, type, 0U, payload, sizeof(payload), tag_key_for(link, type, true));
	link->last_pair_tx_ms = now_ms;

	return submit_tx(link, len, sync_freq_hz(link), ZLRS_SYNC_SLOT_PAIRING, pairing_sync_word);
}

static const zlrs_radio_req_t *pair_rx(zlrs_link_t *link, uint16_t timeout_ms)
{
	return submit_rx(link, sync_freq_hz(link), ZLRS_SYNC_SLOT_PAIRING, pairing_sync_word,
			 timeout_ms);
}

/* Acquisition uses its own window length, see zlrs_link_cfg_t */
static uint16_t acq_timeout_ms(const zlrs_link_t *link)
{
	return (link->cfg.acq_timeout_ms != 0U) ? link->cfg.acq_timeout_ms
						: link->cfg.rx_timeout_ms;
}

/* ---- receive: dispatch --------------------------------------------------- */

static void deliver_data(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	switch (zlrs_reasm_push(&link->reasm, frame, now_ms)) {
	case ZLRS_REASM_DONE:
		if (link->ev.on_data != NULL && link->reasm.len > 0U) {
			link->ev.on_data(link->ev.ctx, link->reasm.buf, link->reasm.len);
		}
		break;
	case ZLRS_REASM_DROPPED:
		link->stats.rx_frag_drop++;
		break;
	default:
		break;
	}
}

static void handle_data(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	/* Validate the entire DATA shape before allowing its ACK to release queued bytes. */
	if (link->cfg.arq && frame->payload_len > 0U && frame->payload_len < 3U) {
		link->stats.rx_bad_frame++;
		return;
	}
	if (link->cfg.arq) {
		handle_ack_bit(link, (frame->flags & ZLRS_FLAG_ACK) != 0U, now_ms);
	}
	if (frame->payload_len == 0U) {
		return; /* Also proves the session when ARQ is disabled. */
	}
	if (link->cfg.arq) {
		const uint16_t seq =
			(uint16_t)(frame->payload[0] | ((uint16_t)frame->payload[1] << 8));
		const uint16_t next = link->arq_rx_started ? (uint16_t)(link->arq_rx_seq + 1U) : 0U;
		const int16_t distance = (int16_t)(seq - next);
		link->arq_ack_pending = true;
		if (distance != 0) {
			if (distance < 0) {
				link->stats.rx_dup++;
			} else {
				link->stats.rx_out_of_order++;
			}
			return; /* Repeat the existing toggle; duplicates never toggle it again. */
		}
		zlrs_frame_t inner = *frame;
		inner.seq = seq;
		inner.payload = &frame->payload[2];
		inner.payload_len = frame->payload_len - 2U;
		inner.tag = NULL;
		link->arq_rx_started = true;
		link->arq_rx_seq = seq;
		deliver_data(link, &inner, now_ms);
		return;
	}
	deliver_data(link, frame, now_ms);
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
	set_state(link, ZLRS_STATE_SEARCHING, now_ms);
	link->last_rx_ms = now_ms;
}

static void handle_sync(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	uint32_t nonce;
	uint16_t hop_index;

	/* SYNC only means something to a paired pair */
	if (!link->paired) {
		return;
	}
	if (frame->payload_len != 12U) {
		return;
	}
	if (frame->payload[0] != ZLRS_PROTO_VERSION ||
	    (frame->payload[1] & 0x0FU) != (uint8_t)link->cfg.profile) {
		return;
	}

	/*
	 * Sub-band mix and table version (doc/08-protocol.md 5.1): the TX moves them unilaterally
	 * and the RX adopts them while handling this SYNC — that moment is the frame boundary for
	 * both ends, so nobody ends up half on the old table.
	 */
	{
		const uint8_t mix = (uint8_t)((frame->payload[1] >> 4) & 0x03U);
		const uint8_t version = (uint8_t)((frame->payload[1] >> 6) & 0x03U);

		if (version != link->sub_mix_version || mix != link->sub_mix) {
			link->sub_mix = mix;
			link->sub_mix_version = version;
			link->stats.mix_changes++;
		}
		link->sub_mix_seen_ms = now_ms;
	}

	hop_index = (uint16_t)(frame->payload[2] | ((uint16_t)frame->payload[3] << 8));
	nonce = rd32(&frame->payload[4]);

	if (link->state == ZLRS_STATE_CONNECTED) {
		/*
		 * Periodic SYNC while connected (doc/08-protocol.md 5.1).
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
			lq_account(link, frame->seq);
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
		set_state(link, ZLRS_STATE_SEARCHING, now_ms);
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
		set_state(link, ZLRS_STATE_CONNECTED, now_ms);
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

/* A pairing frame that arrived but does not belong to this handshake counts, it is a symptom */
static bool pair_reject(zlrs_link_t *link)
{
	link->stats.pair_rejects++;

	return false;
}

static bool handle_pair_frame(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	const uint8_t *dev_a = frame->payload;
	const uint8_t *dev_b = &frame->payload[ZLRS_DEV_ID_LEN];
	uint32_t nonce_a;
	uint32_t nonce_b;
	const bool initiator = (link->cfg.role == ZLRS_ROLE_TX);

	if (frame->payload_len != PAIR_PAYLOAD_LEN) {
		return pair_reject(link);
	}

	nonce_a = rd32(&frame->payload[2U * ZLRS_DEV_ID_LEN]);
	nonce_b = rd32(&frame->payload[2U * ZLRS_DEV_ID_LEN + 4U]);

	switch (frame->type) {
	case ZLRS_PKT_PAIR_REQ:
		link->stats.pair_req_rx++;
		break;
	case ZLRS_PKT_PAIR_RESP:
		link->stats.pair_resp_rx++;
		break;
	case ZLRS_PKT_PAIR_CFM:
		link->stats.pair_cfm_rx++;
		break;
	default:
		break;
	}

	switch (frame->type) {
	case ZLRS_PKT_PAIR_REQ:
		/*
		 * Responder: remember the initiator's id and nonce, answer PAIR_RESP next slot. The
		 * responder nonce is only drawn once per handshake: the initiator repeats PAIR_REQ until
		 * it hears the response, and a fresh nonce per repeat would make the confirmation it
		 * finally sends fail the check below (the reply it answered had another nonce).
		 */
		if (initiator) {
			return pair_reject(link);
		}
		memcpy(link->peer_id, dev_a, ZLRS_DEV_ID_LEN);
		/* Only a different initiator nonce starts a new handshake (repeats keep this answer) */
		if (link->pair_nonce != nonce_a) {
			link->pair_nonce = nonce_a;
			link->pair_nonce_b = link_random(link, now_ms);
		}
		link->pair_wait_cfm = true;

		/* The TX ships failsafe values (the flag is set only if it has them): store and hand over */
		if ((frame->payload[PAIR_FLAGS_OFFSET] & PAIR_FLAG_FAILSAFE) != 0U) {
			memcpy(link->failsafe, &frame->payload[PAIR_FAILSAFE_OFFSET],
			       ZLRS_RC_PAYLOAD_LEN);
			link->failsafe_len = ZLRS_RC_PAYLOAD_LEN;
			if (link->ev.on_failsafe != NULL) {
				link->ev.on_failsafe(link->ev.ctx, link->failsafe,
						     link->failsafe_len);
			}
		}
		return true;

	case ZLRS_PKT_PAIR_RESP:
		/* Initiator: confirm both its id and its nonce came back */
		if (!initiator) {
			return pair_reject(link);
		}
		if (!zlrs_ct_equal(dev_a, link->dev_id, ZLRS_DEV_ID_LEN) ||
		    nonce_a != link->pair_nonce) {
			return pair_reject(link);
		}
		memcpy(link->peer_id, dev_b, ZLRS_DEV_ID_LEN);
		link->pair_nonce_b = nonce_b;
		link->last_pair_rx_ms = now_ms;
		link->pair_cfm_pending = true;
		return true;

	case ZLRS_PKT_PAIR_CFM:
		/* Responder: both nonces are back, pairing is done */
		if (initiator) {
			return pair_reject(link);
		}
		if (!zlrs_ct_equal(dev_a, link->peer_id, ZLRS_DEV_ID_LEN) ||
		    !zlrs_ct_equal(dev_b, link->dev_id, ZLRS_DEV_ID_LEN) ||
		    nonce_a != link->pair_nonce || nonce_b != link->pair_nonce_b) {
			return pair_reject(link);
		}
		link->pair_wait_cfm = false;
		pairing_complete(link, now_ms);
		return true;

	default:
		return pair_reject(link);
	}
}

/*
 * Uplink quality: the sequence-number gap says how many frames should have arrived; once a
 * full window has passed, update the percentage. Only in-session, increasing frames count.
 */
#define ZLRS_LQ_WINDOW_FRAMES 128U

static void lq_account(zlrs_link_t *link, uint16_t seq)
{
	uint16_t delta = (uint16_t)(seq - link->lq_last_seq);

	if (link->stats.lq_rx == 0U && link->stats.lq_expected == 0U) {
		/* First sample: do not charge this run for the gap the last disconnection left */
		delta = 1U;
	}

	link->lq_last_seq = seq;
	link->stats.lq_rx++;
	link->stats.lq_expected = (uint16_t)(link->stats.lq_expected + delta);

	if (link->stats.lq_expected >= ZLRS_LQ_WINDOW_FRAMES) {
		link->stats.lq_percent =
			(uint8_t)(((uint32_t)link->stats.lq_rx * 100U) / link->stats.lq_expected);
		link->stats.lq_rx = 0U;
		link->stats.lq_expected = 0U;
	}
}

/*
 * RX receives the session confirm: verify the tag with the candidate session key (not the
 * state-based path). Only then is the session real; a replayed confirm cannot match.
 */
static int handle_sess_cfm(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	uint8_t expect[ZLRS_TAG_LEN];
	const uint8_t tag_len = zlrs_profile_tag_len(link->cfg.profile);
	const bool repeated = link->cfg.kind == ZLRS_LINK_DATA && link->cfg.role == ZLRS_ROLE_RX &&
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
	link->sess_ack_due = link->cfg.kind == ZLRS_LINK_DATA;
	link->sess_sync_frame = link->frame_index;
	set_state(link, ZLRS_STATE_CONNECTED, now_ms);

	return 0;
}

/*
 * TX receives the session response: the echoed nonce_t must belong to this request (stale or
 * replayed ones are dropped), then derive the key and confirm. The RX repeats its response
 * when the confirm is lost, so the same nonce pair may trigger this again and again.
 */
static bool handle_sess_resp(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
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
			set_state(link, ZLRS_STATE_CONNECTED, now_ms);
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
	} else if (link->cfg.kind == ZLRS_LINK_DATA && !link->sess_cfm_due) {
		/* A delayed pairing-level response cannot reopen a confirmed DATA session. */
		return true;
	}
	link->sess_cfm_due = true;
	/* Session just came up: keep the peer-loss check from kicking us out on a stale timestamp */
	link->last_rx_ms = now_ms;

	return true;
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
		return handle_sess_cfm(link, &frame, now_ms);
	}

	key = tag_key_for(link, frame.type, false);
	if (tag_len > 0U) {
		uint8_t expect[ZLRS_TAG_LEN];

		zlrs_tag(key, ZLRS_KEY_LEN, link->rx_buf, zlrs_frame_mac_len(&frame), expect);
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
		return handle_pair_frame(link, &frame, now_ms) ? 0 : -1;
	}

	if (frame.type == ZLRS_PKT_SYNC) {
		handle_sync(link, &frame, now_ms);
		return 0;
	}

	if (frame.type == ZLRS_PKT_SESS_RESP) {
		return handle_sess_resp(link, &frame, now_ms) ? 0 : -1;
	}

	/* For the initiator any session frame from the peer means the link is up */
	if (link->state == ZLRS_STATE_SEARCHING && link->cfg.role == ZLRS_ROLE_TX &&
	    (frame.type == ZLRS_PKT_TLM || frame.type == ZLRS_PKT_DATA_UP ||
	     frame.type == ZLRS_PKT_DATA_DOWN)) {
		set_state(link, ZLRS_STATE_CONNECTED, now_ms);
	}

	if (link->state != ZLRS_STATE_CONNECTED) {
		return -1;
	}

	/* Replay protection: sequence numbers must strictly increase within a session */
	if ((int16_t)(frame.seq - link->last_rx_seq) <= 0) {
		link->stats.rx_dup++;
		return -1;
	}
	link->last_rx_seq = frame.seq;
	link->last_rx_ms = now_ms;
	link->last_rx_frame = link->frame_index;
	if (link->cfg.kind == ZLRS_LINK_DATA && link->cfg.role == ZLRS_ROLE_TX) {
		link->sess_cfm_due = false;
	}
	/* The peer is alive: stop sending the simplified handshake's "I am here" */
	link->announce = false;
	lq_account(link, frame.seq);

	switch (frame.type) {
	case ZLRS_PKT_RC:
		if (link->ev.on_rc != NULL && frame.payload_len > 0U) {
			link->ev.on_rc(link->ev.ctx, frame.payload, frame.payload_len);
		}
		break;
	case ZLRS_PKT_TLM:
		if (link->ev.on_tlm != NULL && frame.payload_len > 0U) {
			link->ev.on_tlm(link->ev.ctx, frame.payload, frame.payload_len);
		}
		break;
	case ZLRS_PKT_DATA_UP:
	case ZLRS_PKT_DATA_DOWN:
		handle_data(link, &frame, now_ms);
		break;
	default:
		break;
	}

	return 0;
}

/* ---- slots --------------------------------------------------------------- */

static void advance_hop(zlrs_link_t *link)
{
	if (link->cfg.hopping == 0U) {
		/* Pinned to the sync channel (entry 0 of the hop table) */
		return;
	}

	if (link->state == ZLRS_STATE_CONNECTED) {
		link->stats.hop_advances++;
		(void)zlrs_fhss_next_hz(&link->fhss);
		/* The sub-G table has its own index: advance it every frame so both ends stay aligned */
		if (zlrs_fhss_sub_ready(&link->fhss)) {
			(void)zlrs_fhss_sub_next_hz(&link->fhss);
		}
	}
}

static const zlrs_radio_req_t *control_slot(zlrs_link_t *link, uint8_t slot, uint32_t now_ms)
{
	const bool down = (slot == ZLRS_SLOT_CTL_DOWN);
	const bool tx_side = (link->cfg.role == ZLRS_ROLE_TX);

	/* The main (downlink) slot counts once per frame, keeping both ends in step */
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
					return pair_rx(link, acq_timeout_ms(link));
				}
				return send_pair(link, ZLRS_PKT_PAIR_REQ, link->pair_nonce, 0U,
						 now_ms);
			}
			return pair_rx(link, acq_timeout_ms(link));
		}

		if (tx_side) {
			return pair_rx(link, acq_timeout_ms(link));
		}
		if (link->pair_wait_cfm) {
			if ((uint32_t)(now_ms - link->last_pair_tx_ms) >=
			    link->cfg.pair_period_ms) {
				return send_pair(link, ZLRS_PKT_PAIR_RESP, link->pair_nonce,
						 link->pair_nonce_b, now_ms);
			}
			/* Between repeats: listen, for the same reason as the initiator above */
			return pair_rx(link, acq_timeout_ms(link));
		}
		return pair_rx(link, acq_timeout_ms(link));
	}

	if (link->state == ZLRS_STATE_SEARCHING) {
		if (tx_side) {
			if (down) {
				/*
				 * A peer whose confirms were all lost keeps repeating its answer; answering again
				 * is what unsticks it, and that costs this frame's SYNC at most.
				 */
				if (link->pair_cfm_pending) {
					if (link->pair_cfm_sent >= ZLRS_PAIR_CFM_REPEATS) {
						link->pair_cfm_pending = false;
						return NULL;
					}
					if ((uint32_t)(now_ms - link->last_pair_tx_ms) >=
					    link->cfg.pair_period_ms) {
						const zlrs_radio_req_t *req = send_pair(
							link, ZLRS_PKT_PAIR_CFM, link->pair_nonce,
							link->pair_nonce_b, now_ms);

						if (req != NULL) {
							link->pair_cfm_sent++;
						}
						return req;
					}
					return NULL;
				}
				if (link->sess_cfm_due) {
					/*
					 * Enter CONNECTED as soon as the confirm goes out, so both ends advance the hop table from
					 * the same frame and the indices cannot drift. The confirm itself is repeated by the
					 * CONNECTED slot below until the peer is known to have heard one.
					 */
					const zlrs_radio_req_t *req = send_sess_cfm(link);

					if (req != NULL) {
						set_state(link, ZLRS_STATE_CONNECTED, now_ms);
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
				return send_sync(link, now_ms);
			}

			/*
			 * The initiator listens for the peer's telemetry in the uplink slot, but takes the
			 * pairing channel now and then: a peer that never heard a confirm is still answering
			 * there, and this window is the only way to hear it and finish its pairing.
			 */
			if ((link->frame_index % ZLRS_PAIR_SCAN_FRAMES) ==
			    (ZLRS_PAIR_SCAN_FRAMES - 1U)) {
				return pair_rx(link, acq_timeout_ms(link));
			}
			return submit_rx(link, sync_freq_hz(link), link->cfg.sync_slot,
					 link->sync_word, acq_timeout_ms(link));
		}
		if (down) {
			return submit_rx(link, sync_freq_hz(link), link->cfg.sync_slot,
					 link->sync_word, acq_timeout_ms(link));
		}
		/*
		 * Repeat the answer until the confirm arrives: one shot per SYNC would leave the handshake
		 * waiting for the next resync frame whenever that single frame is lost.
		 */
		if (link->sess_resp_due || link->session_pending) {
			return send_sess_resp(link);
		}
		return NULL;
	}

	if (link->state != ZLRS_STATE_CONNECTED) {
		return NULL;
	}

	if (down) {
		advance_hop(link);
		if (tx_side) {
			/* Owing a confirm: send it, and this whole frame stays on the sync channel */
			if (link->sess_cfm_due) {
				return send_sess_cfm(link);
			}
			/*
			 * Resync frame: send a SYNC that states our hop index. The peer uses it to correct drift,
			 * and if it changed sessions (a reboot) that is how it re-handshakes.
			 */
			if (sync_frame(link)) {
				return send_sync(link, now_ms);
			}
			return send_rc(link);
		}
		return submit_rx(link, slot_freq_hz(link, SLOT_RC), link->cfg.sync_slot,
				 link->sync_word, link->cfg.rx_timeout_ms);
	}

	/* Owing a confirm: the peer (in SEARCHING) repeats SESS_RESP on the sync channel */
	if (link->sess_cfm_due) {
		if (tx_side) {
			return submit_rx(link, sync_freq_hz(link), link->cfg.sync_slot,
					 link->sync_word, acq_timeout_ms(link));
		}
		return NULL;
	}

	/*
	 * Fresh session: a peer that has not heard the confirm yet is still answering on the sync
	 * channel, so keep a window there for a moment instead of switching straight to telemetry.
	 * Without it a single lost confirm strands that peer until the next resync frame.
	 */
	if (tx_side && (uint32_t)(now_ms - link->state_ms) < ZLRS_SESS_CONFIRM_MS) {
		return submit_rx(link, sync_freq_hz(link), link->cfg.sync_slot, link->sync_word,
				 link->cfg.rx_timeout_ms);
	}

	/*
	 * The uplink slot of a resync frame stays on the sync channel too: the TX waits there for a
	 * possible SESS_RESP (the peer-reboot path), and the RX puts telemetry on it as well.
	 */
	if (sync_frame(link)) {
		if (tx_side) {
			return submit_rx(link, sync_freq_hz(link), link->cfg.sync_slot,
					 link->sync_word, acq_timeout_ms(link));
		}
		if (link->cfg.telemetry == 0U) {
			return NULL;
		}
		return send_tlm(link);
	}

	/*
	 * The simplified handshake's report frame wins: the peer may still be in SEARCHING,
	 * listening on the sync channel. With telemetry off that channel carries nothing else.
	 */
	if (link->announce) {
		if (tx_side) {
			return submit_rx(link, sync_freq_hz(link), link->cfg.sync_slot,
					 link->sync_word, acq_timeout_ms(link));
		}
		return send_sess_resp(link);
	}

	if (link->cfg.telemetry == 0U) {
		return NULL;
	}

	/* Uplink slot: telemetry is throttled by tlm_every_n (both ends agree, hopping is unaffected) */
	if ((link->frame_index % link->cfg.tlm_every_n) != 0U) {
		return NULL;
	}

	if (tx_side) {
		return submit_rx(link, slot_freq_hz(link, SLOT_TLM), link->cfg.sync_slot,
				 link->sync_word, link->cfg.rx_timeout_ms);
	}

	return send_tlm(link);
}

static const zlrs_radio_req_t *data_slot(zlrs_link_t *link, uint8_t slot, uint32_t now_ms)
{
	const bool a = (slot == ZLRS_SLOT_DATA_A);
	const bool tx_side = (link->cfg.role == ZLRS_ROLE_TX);

	/* Pairing uses the same exchange on either link, in that link's own two slots.
	 * SEARCHING also answers repeated responses when a pairing confirm was lost. */
	if (link->state == ZLRS_STATE_PAIRING ||
	    (link->state == ZLRS_STATE_SEARCHING && link->pair_cfm_pending)) {
		return control_slot(link, a ? ZLRS_SLOT_CTL_DOWN : ZLRS_SLOT_CTL_UP, now_ms);
	}

	/* The main slot (A) counts once per frame */
	if (a) {
		link->frame_index++;
	}

	if (link->state == ZLRS_STATE_SEARCHING) {
		if (tx_side) {
			if (a) {
				if (link->sess_cfm_due) {
					/* The responder may have rebooted while this confirm was pending. */
					if ((uint32_t)(now_ms - link->last_sync_ms) >=
					    link->cfg.sync_period_ms) {
						return send_sync(link, now_ms);
					}
					/*
					 * Repeat the confirm (the peer leaves SEARCHING on one of them) and only then count the
					 * session as established.
					 */
					const zlrs_radio_req_t *req = send_sess_cfm(link);

					if (req != NULL) {
						set_state(link, ZLRS_STATE_CONNECTED, now_ms);
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
				return send_sync(link, now_ms);
			}
			/* The initiator waits for the peer's first frame in the uplink slot */
			return submit_rx(link, sync_freq_hz(link), link->cfg.sync_slot,
					 link->sync_word, acq_timeout_ms(link));
		}
		if (a) {
			return submit_rx(link, sync_freq_hz(link), link->cfg.sync_slot,
					 link->sync_word, acq_timeout_ms(link));
		}
		/* Same as the control link: keep answering until the confirm arrives */
		if (link->sess_resp_due || link->session_pending) {
			return send_sess_resp(link);
		}
		return NULL;
	}

	if (link->state != ZLRS_STATE_CONNECTED) {
		return NULL;
	}

	/*
	 * At high control rates the data windows thin out by data_every_n (both ends agree).
	 * Resync frames and session confirms stay out of that: their cadence has to divide the
	 * remaining frames, or (333 frames against 5, say) they would almost never line up.
	 */
	if (!sync_frame(link) && !link->sess_cfm_due && !link->sess_ack_due &&
	    (link->frame_index % link->cfg.data_every_n) != 0U) {
		return NULL;
	}

	advance_hop(link);

	if (link->sess_ack_due && tx_side == a) {
		const zlrs_radio_req_t *req = send_empty_data(link);

		if (req != NULL) {
			link->sess_ack_due = false;
		}
		return req;
	}

	/* Legal idle DATA can stay silent until the next periodic SYNC. Only reacquire
	 * after two missed SYNC periods; the SEARCHING repeat interval is unrelated. */
	if (!tx_side && a && link->cfg.resync_every_n > 0U &&
	    (uint32_t)(link->frame_index - link->last_rx_frame) > 2U * link->cfg.resync_every_n) {
		return submit_rx(link, sync_freq_hz(link), link->cfg.sync_slot, link->sync_word,
				 link->cfg.rx_timeout_ms);
	}

	/* Simplified handshake report: the data link sends it in its own half window, sync channel */
	if (link->announce && (tx_side == a)) {
		return send_sess_resp(link);
	}

	if (tx_side == a) {
		/* Session confirm first, resync SYNC second (both use the sync channel) */
		if (link->sess_cfm_due) {
			if ((uint32_t)(now_ms - link->last_sync_ms) >= link->cfg.sync_period_ms) {
				return send_sync(link, now_ms);
			}
			return send_sess_cfm(link);
		}
		if (sync_frame(link)) {
			return send_sync(link, now_ms);
		}
	}

	if (tx_side == a) {
		const zlrs_radio_req_t *req;

		req = send_data(link, now_ms);

		if (req != NULL) {
			return req;
		}
		/* Report the receive toggle in an empty DATA when no user DATA can be sent. */
		if (link->cfg.arq && link->arq_ack_pending) {
			return send_empty_data(link);
		}

		/* An idle transmit slot stays idle; receiving the opposite slot loses hop/phase alignment. */
		return NULL;
	}

	/*
	 * Fresh session on the data link as well: keep a window on the sync channel where a peer that
	 * missed the confirm is still repeating its answer.
	 */
	if (tx_side && (uint32_t)(now_ms - link->state_ms) < ZLRS_SESS_CONFIRM_MS) {
		return submit_rx(link, sync_freq_hz(link), link->cfg.sync_slot, link->sync_word,
				 link->cfg.rx_timeout_ms);
	}

	return submit_rx(link, slot_freq_hz(link, SLOT_DATA), link->cfg.sync_slot, link->sync_word,
			 link->cfg.rx_timeout_ms);
}

/* ---- public API ---------------------------------------------------------- */

static void limit_data_mtu(zlrs_link_t *link)
{
	const uint16_t cap = link->cfg.mod.modulation == ZLRS_MOD_LORA ? ZLRS_LORA_FRAME_MAX_LEN
								       : ZLRS_FRAME_MAX_LEN;
	const uint16_t mtu =
		cap - ZLRS_FRAME_HDR_LEN - zlrs_profile_tag_len(link->cfg.profile) - 2U;

	if (link->cfg.data_mtu > mtu) {
		link->cfg.data_mtu = mtu;
	}
}

void zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg, uint32_t (*random)(void *ctx),
		    void *random_ctx, const zlrs_link_events_t *events)
{
	memset(link, 0, sizeof(*link));
	link->cfg = *cfg;
	limit_data_mtu(link);
	/* Cadence fields default to 1 (every frame); 0 is not a legal value */
	if (link->cfg.tlm_every_n == 0U) {
		link->cfg.tlm_every_n = 1U;
	}
	if (link->cfg.data_every_n == 0U) {
		link->cfg.data_every_n = 1U;
	}
	/* Sub-band mix: the initial level comes from the configuration, the TX advances it via SYNC versions */
	link->sub_mix = (uint8_t)(link->cfg.mix & 0x03U);
	link->sub_mix_default = link->sub_mix;
	link->sub_mix_version = 0U;
	link->sub_mix_seen_ms = 0U;
	link->random = random;
	link->random_ctx = random_ctx;
	if (events != NULL) {
		link->ev = *events;
	}

	reasm_restart(link);
	link->last_rx_seq = 0xFFFFU;
	link->stats.lq_percent = 100U;
	link->state = ZLRS_STATE_UNCONFIGURED;
}

void zlrs_link_set_root_key(zlrs_link_t *link, const uint8_t root_key[ZLRS_KEY_LEN],
			    uint32_t now_ms)
{
	memcpy(link->root_key, root_key, ZLRS_KEY_LEN);
	link->configured = true;
	derive_auth(link);

	if (link->paired) {
		derive_pair(link);
		set_state(link, ZLRS_STATE_SEARCHING, now_ms);
	} else {
		set_state(link, ZLRS_STATE_UNPAIRED, now_ms);
	}
}

void zlrs_link_set_dev_id(zlrs_link_t *link, const uint8_t dev_id[ZLRS_DEV_ID_LEN])
{
	memcpy(link->dev_id, dev_id, ZLRS_DEV_ID_LEN);
}

uint16_t zlrs_link_tx_space(const zlrs_link_t *link)
{
	return (uint16_t)(ZLRS_LINK_TX_QUEUE_LEN - link->queue_count);
}

void zlrs_link_pair_ctx(const zlrs_link_t *link, uint8_t out[ZLRS_PAIR_CTX_LEN])
{
	ordered_ids(link, out);
	ordered_pair_salt(link, &out[2U * ZLRS_DEV_ID_LEN]);
}

const zlrs_radio_req_t *zlrs_link_listen(zlrs_link_t *link)
{
	if (link->state != ZLRS_STATE_CONNECTED && link->state != ZLRS_STATE_SEARCHING) {
		return NULL;
	}

	if (link->cfg.kind == ZLRS_LINK_CONTROL) {
		return submit_rx(link, slot_freq_hz(link, SLOT_RC), link->cfg.sync_slot,
				 link->sync_word, link->cfg.rx_timeout_ms);
	}

	return submit_rx(link, slot_freq_hz(link, SLOT_DATA), link->cfg.sync_slot, link->sync_word,
			 link->cfg.rx_timeout_ms);
}

void zlrs_link_set_mix(zlrs_link_t *link, uint8_t mix)
{
	link->sub_mix_default = (uint8_t)(mix & 0x03U);

	/* The TX advances at once (the next SYNC carries the version); the RX only moves its fallback */
	if (link->cfg.role == ZLRS_ROLE_TX) {
		link->sub_mix = link->sub_mix_default;
		link->sub_mix_version = (uint8_t)((link->sub_mix_version + 1U) & 0x03U);
	}
}

void zlrs_link_set_mod(zlrs_link_t *link, const zlrs_mod_cfg_t *mod)
{
	if (mod == NULL) {
		return;
	}

	link->cfg.mod = *mod;
	limit_data_mtu(link);
	/* LoRa sync words are derived; a caller passing 0 must not clobber the current one */
	if (link->cfg.mod.modulation == (uint8_t)ZLRS_MOD_LORA && link->cfg.mod.sync_byte == 0U) {
		link->cfg.mod.sync_byte = link->lora_sync;
	}
}

void zlrs_link_set_power(zlrs_link_t *link, int8_t tx_power_dbm)
{
	link->cfg.tx_power_dbm = tx_power_dbm;
}

void zlrs_link_set_peer(zlrs_link_t *link, const uint8_t peer_id[ZLRS_DEV_ID_LEN],
			const uint8_t pair_salt[ZLRS_PAIR_SALT_LEN], uint32_t now_ms)
{
	memcpy(link->peer_id, peer_id, ZLRS_DEV_ID_LEN);
	memcpy(link->pair_salt, pair_salt, ZLRS_PAIR_SALT_LEN);
	link->paired = true;

	if (link->configured) {
		derive_pair(link);
		set_state(link, ZLRS_STATE_SEARCHING, now_ms);
	}
}

void zlrs_link_press(zlrs_link_t *link, uint32_t now_ms)
{
	if (!link->configured) {
		return;
	}

	link->pair_nonce = link_random(link, now_ms);
	link->pair_nonce_b = link_random(link, link->pair_nonce);
	link->pair_cfm_pending = false;
	link->pair_cfm_sent = 0U;
	link->pair_wait_cfm = false;
	set_state(link, ZLRS_STATE_PAIRING, now_ms);
}

const zlrs_radio_req_t *zlrs_link_slot(zlrs_link_t *link, uint8_t slot, uint32_t now_ms)
{
	link->slot = slot;
	link->slot_ms = now_ms;

	if (link->cfg.kind == ZLRS_LINK_CONTROL) {
		return control_slot(link, slot, now_ms);
	}

	return data_slot(link, slot, now_ms);
}

void zlrs_link_skip_slot(zlrs_link_t *link, uint8_t slot)
{
	link->prepared_data.valid = false;
	link->slot = slot;
	if (link->cfg.kind == ZLRS_LINK_CONTROL) {
		if (slot == ZLRS_SLOT_CTL_DOWN) {
			link->frame_index++;
			advance_hop(link);
		}
		return;
	}
	if (slot == ZLRS_SLOT_DATA_A) {
		link->frame_index++;
	}
	if (!sync_frame(link) && !link->sess_cfm_due &&
	    (link->frame_index % link->cfg.data_every_n) != 0U) {
		return;
	}
	advance_hop(link);
}

void zlrs_link_complete(zlrs_link_t *link, int status, uint16_t rx_len, int16_t rssi, int8_t snr,
			uint32_t now_ms)
{
	const bool was_tx = link->req_is_tx;
	const uint8_t slot = (link->slot < 4U) ? link->slot : 0U;

	link->req_pending = false;
	link->stats.last_rssi = rssi;
	link->stats.last_snr = snr;

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
			if (frame.payload_len == 0U) {
				link->stats.tx_ack_only_frames++;
			} else if (frame.payload_len > (link->cfg.arq ? 2U : 0U)) {
				link->stats.tx_data_frames++;
				link->stats.tx_data_ack_frames += link->cfg.arq != 0U;
				link->stats.tx_data_bytes +=
					frame.payload_len - (link->cfg.arq ? 2U : 0U);
				const uint16_t cap = link->cfg.mod.modulation == ZLRS_MOD_LORA
							     ? ZLRS_LORA_FRAME_MAX_LEN
							     : ZLRS_FRAME_MAX_LEN;

				link->stats.tx_full_data_frames += link->req.size == cap ? 1U : 0U;
			}
		}
		link->queue_head = (uint16_t)((link->queue_head + link->pending_data_len) %
					      ZLRS_LINK_TX_QUEUE_LEN);
		link->queue_count = (uint16_t)(link->queue_count - link->pending_data_len);
		link->pending_data_len = 0U;
		return;
	}

	link->stats.slot_rx[slot]++;
	if (rssi != 0) {
		if (link->stats.rssi_min == 0 || rssi < link->stats.rssi_min) {
			link->stats.rssi_min = rssi;
		}
		if (rssi > link->stats.rssi_max) {
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
	/*
	 * Sub-band fallback: when the TX changed the mix and the RX did not follow, the RX keeps
	 * receiving and returns to the default level after 3 sync periods without a new version.
	 */
	if (link->state == ZLRS_STATE_CONNECTED && link->sub_mix != link->sub_mix_default &&
	    link->cfg.sync_period_ms > 0U &&
	    (uint32_t)(now_ms - link->sub_mix_seen_ms) >
		    (uint32_t)(3U * link->cfg.sync_period_ms)) {
		link->sub_mix = link->sub_mix_default;
		link->stats.mix_fallbacks++;
	}

	switch (link->state) {
	case ZLRS_STATE_UNCONFIGURED:
		if (link->configured) {
			set_state(link, ZLRS_STATE_UNPAIRED, now_ms);
		}
		break;

	case ZLRS_STATE_PAIRING:
		if ((uint32_t)(now_ms - link->state_ms) > link->cfg.pair_window_ms) {
			set_state(link, link->paired ? ZLRS_STATE_SEARCHING : ZLRS_STATE_UNPAIRED,
				  now_ms);
		}
		break;

	case ZLRS_STATE_CONNECTED:
		/*
		 * Grace after entering CONNECTED: the peer may still be leaving SEARCHING (it needs to
		 * hear one of our confirms), and its first telemetry cannot arrive until it does. Running
		 * the peer-loss check inside that window would roll a new session and restart the
		 * handshake before it ever had a chance to finish.
		 */
		if ((uint32_t)(now_ms - link->state_ms) <= ZLRS_SESS_CONFIRM_MS) {
			break;
		}
		if (link->cfg.peer_timeout_ms > 0U &&
		    (uint32_t)(now_ms - link->last_rx_ms) > link->cfg.peer_timeout_ms) {
			link->session_valid = false;
			set_state(link, ZLRS_STATE_SEARCHING, now_ms);
		}
		break;

	default:
		break;
	}
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

void zlrs_link_set_rc(zlrs_link_t *link, const uint8_t *channels, uint16_t len)
{
	if (len > sizeof(link->rc_payload)) {
		len = sizeof(link->rc_payload);
	}
	memcpy(link->rc_payload, channels, len);
	link->rc_len = len;
}

void zlrs_link_set_tlm(zlrs_link_t *link, const uint8_t *payload, uint16_t len)
{
	if (len > sizeof(link->tlm_payload)) {
		len = sizeof(link->tlm_payload);
	}
	memcpy(link->tlm_payload, payload, len);
	link->tlm_len = len;
}

void zlrs_link_set_failsafe(zlrs_link_t *link, const uint8_t *payload, uint16_t len)
{
	if (payload == NULL || len != ZLRS_RC_PAYLOAD_LEN) {
		return;
	}

	memcpy(link->failsafe, payload, ZLRS_RC_PAYLOAD_LEN);
	link->failsafe_len = ZLRS_RC_PAYLOAD_LEN;
}

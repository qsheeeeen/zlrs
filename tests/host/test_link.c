/* SPDX-License-Identifier: Apache-2.0 */
/* Two-node DATA-only simulation: pairing, sessions, duplex ARQ and backpressure. */
#include "test.h"
#include <string.h>
#include "zlrs/link/link.h"
#include "band.h"

typedef struct {
	zlrs_link_t link;
	uint32_t rng;
	uint8_t received[4096];
	uint16_t received_len;
	bool blocked;
	bool random_failed;
	bool force_random;
	uint32_t forced_random;
} node_t;

typedef struct {
	node_t tx;
	node_t rx;
	uint32_t now_ms;
	uint32_t frames;
	uint16_t drop_tx;
	uint16_t drop_rx;
	bool drop_confirms_only;
	bool block_24_tx, block_24_rx, lose_band_plans;
	int16_t rssi_24;
} sim_t;

static bool random_word(void *ctx, uint32_t *out)
{
	node_t *const node = ctx;
	if (node->random_failed) {
		return false;
	}
	if (node->force_random) {
		*out = node->forced_random;
		return true;
	}
	uint32_t x = node->rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*out = node->rng = x != 0U ? x : 1U;
	return true;
}

static bool receive_data(void *ctx, const uint8_t *data, uint16_t len)
{
	node_t *const node = ctx;
	if (node->blocked || len > sizeof(node->received) - node->received_len) {
		return false;
	}
	memcpy(node->received + node->received_len, data, len);
	node->received_len += len;
	return true;
}

static void init_node(node_t *node, zlrs_role_t role, const uint8_t dev_id[4],
		      const uint8_t root[32])
{
	zlrs_link_cfg_t cfg = {
		.role = role,
		.domain = ZLRS_DOMAIN_ISM2400,

		.profile = ZLRS_PROFILE_P1,
		.mod = {.rate_bps = 2600000U},
		.tx_power_dbm = 10,
		.rx_timeout_ms = 2U,
		.acq_timeout_ms = 8U,
		.sync_period_ms = 100U,
		.pair_period_ms = 100U,
		.pair_window_ms = 30000U,
		.data_mtu = 128U,
		.arq = 1U,
		.arq_timeout_ms = 30U,

		.hopping = 1U,
		.resync_every_n = 20U,
		.feedback_period_ms = 250U,
	};
	zlrs_link_events_t events = {.on_data = receive_data, .ctx = node};
	zlrs_link_init(&node->link, &cfg, random_word, node, &events);
	zlrs_link_set_dev_id(&node->link, dev_id);
	zlrs_link_set_root_key(&node->link, root, 0U);
}

static void sim_init(sim_t *sim)
{
	static const uint8_t tx_id[4] = {1, 2, 3, 4};
	static const uint8_t rx_id[4] = {5, 6, 7, 8};
	uint8_t root[32];
	memset(sim, 0, sizeof(*sim));
	for (uint8_t i = 0U; i < sizeof(root); i++) {
		root[i] = i + 1U;
	}
	sim->tx.rng = 0x12345678U;
	sim->rx.rng = 0x87654321U;
	init_node(&sim->tx, ZLRS_ROLE_TX, tx_id, root);
	init_node(&sim->rx, ZLRS_ROLE_RX, rx_id, root);
}

/* Fixed v11 pairing/session/duplex/loss wire transcript. */
static uint64_t wire_hash;
static uint32_t wire_packets;
static uint8_t captured[ZLRS_FRAME_MAX_LEN];
static uint16_t captured_len;

static void record_wire(const zlrs_radio_req_t *req, bool from_tx)
{
	if (req == NULL || !req->is_tx) {
		return;
	}
	wire_packets++;
	wire_hash = (wire_hash ^ from_tx) * UINT64_C(1099511628211);
	for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
		wire_hash =
			(wire_hash ^ ((req->freq_hz >> shift) & 0xffU)) * UINT64_C(1099511628211);
	}
	for (uint16_t i = 0U; i < req->size; ++i) {
		wire_hash = (wire_hash ^ req->buf[i]) * UINT64_C(1099511628211);
	}
	zlrs_frame_t frame;
	if (from_tx && zlrs_frame_decode(&frame, req->buf, req->size, ZLRS_TAG_LEN) &&
	    frame.type == ZLRS_PKT_DATA_UP &&
	    req->size > ZLRS_FRAME_HDR_LEN + ZLRS_DATA_PREFIX_LEN + ZLRS_TAG_LEN) {
		memcpy(captured, req->buf, req->size);
		captured_len = req->size;
	}
}

static void sim_slot(sim_t *sim, uint8_t slot)
{
	zlrs_link_t *const tx = &sim->tx.link;
	zlrs_link_t *const rx = &sim->rx.link;
	uint16_t prepared_len;
	(void)zlrs_link_prepare_data(tx, slot, sim->now_ms, &prepared_len);
	(void)zlrs_link_prepare_data(rx, slot, sim->now_ms, &prepared_len);
	const zlrs_radio_req_t *const a = zlrs_link_slot(tx, slot, sim->now_ms);
	const zlrs_radio_req_t *const b = zlrs_link_slot(rx, slot, sim->now_ms);
	record_wire(a, true);
	record_wire(b, false);
	if (a != NULL && b != NULL && a->is_tx != b->is_tx && a->freq_hz == b->freq_hz &&
	    memcmp(a->sync_word, b->sync_word, ZLRS_SYNC_WORD_LEN) == 0) {
		const zlrs_radio_req_t *const source = a->is_tx ? a : b;
		const zlrs_radio_req_t *const dest = a->is_tx ? b : a;
		zlrs_link_t *const sender = a->is_tx ? tx : rx;
		zlrs_link_t *const receiver = a->is_tx ? rx : tx;
		uint16_t *const drops = a->is_tx ? &sim->drop_tx : &sim->drop_rx;
		zlrs_frame_t air;
		CHECK(zlrs_frame_decode(&air, source->buf, source->size,
				      zlrs_profile_tag_len(sender->cfg.profile)));
		const bool on_24 = source->freq_hz > 1000000000U;
		const bool blocked = on_24 && (a->is_tx ? sim->block_24_tx : sim->block_24_rx);
		const bool lost_plan = sim->lose_band_plans && air.type == ZLRS_PKT_SYNC &&
			air.body.sync.band_plan.switch_frame != 0U;
		if (blocked || lost_plan || (*drops != 0U &&
		    (!sim->drop_confirms_only || air.type == ZLRS_PKT_SESS_CFM))) {
			if (*drops != 0U) { (*drops)--; }
			zlrs_link_complete(receiver, ZLRS_RADIO_OK, 0U, 0, 0, sim->now_ms);
		} else {
			memcpy(dest->buf, source->buf, source->size);
			zlrs_link_complete(receiver, ZLRS_RADIO_OK, source->size,
					   on_24 && sim->rssi_24 ? sim->rssi_24 : -60, 0,
					   sim->now_ms);
		}
		zlrs_link_complete(sender, ZLRS_RADIO_OK, 0U, 0, 0, sim->now_ms);
	} else {
		if (a != NULL) {
			zlrs_link_complete(tx, ZLRS_RADIO_OK, 0U, 0, 0, sim->now_ms);
		}
		if (b != NULL) {
			zlrs_link_complete(rx, ZLRS_RADIO_OK, 0U, 0, 0, sim->now_ms);
		}
	}
}

static void sim_frame(sim_t *sim)
{
	for (uint8_t slot = 0U; slot < ZLRS_SLOT_COUNT; slot++) {
		sim_slot(sim, slot);
		sim->now_ms += 3U;
	}
	zlrs_link_tick(&sim->tx.link, sim->now_ms);
	zlrs_link_tick(&sim->rx.link, sim->now_ms);
	sim->frames++;
}

static bool both_connected(const sim_t *sim)
{
	return zlrs_link_get_state(&sim->tx.link) == ZLRS_STATE_CONNECTED &&
	       zlrs_link_get_state(&sim->rx.link) == ZLRS_STATE_CONNECTED;
}

static void finish_quality_window(sim_t *sim)
{
	const zlrs_link_stats_t *stats = zlrs_link_get_stats(&sim->rx.link);
	uint16_t previous = stats->lq_expected;
	for (unsigned int i = 0; i < 6000U; i++) {
		sim_frame(sim);
		if (previous != 0U && stats->lq_expected == 0U) {
			return;
		}
		previous = stats->lq_expected;
	}
	CHECK(false);
}

static void test_quality_window_boundary(void)
{
	static sim_t sim;
	sim_init(&sim);
	zlrs_link_press(&sim.tx.link, sim.now_ms);
	zlrs_link_press(&sim.rx.link, sim.now_ms);
	for (unsigned int i = 0; i < 3000U && !both_connected(&sim); i++) {
		sim_frame(&sim);
	}
	CHECK(both_connected(&sim));
	const zlrs_link_stats_t *stats = zlrs_link_get_stats(&sim.rx.link);
	finish_quality_window(&sim);
	CHECK(stats->lq_percent == 100U);

	/* The first transmission in the next window is lost; its gap belongs to that window. */
	sim.drop_tx = 1U;
	for (unsigned int i = 0; i < 1000U && sim.drop_tx != 0U; i++) {
		sim_frame(&sim);
	}
	CHECK(sim.drop_tx == 0U && stats->lq_expected == 0U);
	for (unsigned int i = 0; i < 1000U && stats->lq_expected == 0U; i++) {
		sim_frame(&sim);
	}
	CHECK(stats->lq_rx == 1U && stats->lq_expected == 2U);
	finish_quality_window(&sim);
	CHECK(stats->lq_percent == 99U);
	finish_quality_window(&sim);
	CHECK(stats->lq_percent == 100U);
}

static void test_data_sequence_wrap(void)
{
	static sim_t sim;
	sim_init(&sim);
	sim.tx.link.cfg.data_mtu = 1U;
	sim.rx.link.cfg.data_mtu = 1U;
	zlrs_link_press(&sim.tx.link, sim.now_ms);
	zlrs_link_press(&sim.rx.link, sim.now_ms);
	for (unsigned int i = 0; i < 3000U && !both_connected(&sim); i++) {
		sim_frame(&sim);
	}
	CHECK(both_connected(&sim));
	for (unsigned int i = 0U; i < 512U; i++) {
		const uint8_t up = (uint8_t)i;
		const uint8_t down = (uint8_t)(i ^ 0x5aU);
		CHECK(zlrs_link_write(&sim.tx.link, &up, 1U) == 1U);
		CHECK(zlrs_link_write(&sim.rx.link, &down, 1U) == 1U);
		/* Drop reverse ACKs, and hold the receiver at sequence wrap boundaries. */
		if (i % 31U == 0U) {
			sim.drop_rx = 2U;
		}
		const bool blocked = (i & 127U) == 127U;
		sim.rx.blocked = blocked;
		for (unsigned int wait = 0U; wait < 1000U; wait++) {
			if (wait == 12U) {
				sim.rx.blocked = false;
			}
			sim_frame(&sim);
			if (sim.tx.link.queue_count == 0U && sim.rx.link.queue_count == 0U &&
			    sim.tx.received_len == i + 1U && sim.rx.received_len == i + 1U) {
				break;
			}
		}
		CHECK(sim.tx.received_len == i + 1U && sim.rx.received_len == i + 1U);
		CHECK(sim.rx.received[i] == up && sim.tx.received[i] == down);
		CHECK(sim.tx.link.arq_seq == ((i + 1U) & ZLRS_DATA_SEQ_MASK));
		CHECK(sim.rx.link.arq_seq == ((i + 1U) & ZLRS_DATA_SEQ_MASK));
	}
	CHECK(sim.rx.link.stats.rx_dup > 0U);
	CHECK(sim.tx.link.stats.tx_retries > 0U);
}


/* Exercise the production RX entry with correctly authenticated attack packets. */
static void inject(node_t *receiver, const zlrs_frame_t *packet, uint16_t body_len,
		   const uint8_t *key, uint32_t now_ms, bool bad_tag)
{
	uint8_t bytes[ZLRS_FRAME_MAX_LEN];
	const uint8_t tag_len = zlrs_profile_tag_len(receiver->link.cfg.profile);
	const uint16_t size = zlrs_frame_len(body_len, tag_len);
	CHECK(zlrs_frame_encode(packet, bytes, sizeof(bytes), size, tag_len) == size);
	if (tag_len != 0U) {
		zlrs_tag(key, ZLRS_KEY_LEN, bytes, size - tag_len, bytes + size - tag_len);
		if (bad_tag) {
			bytes[size - 1U] ^= 0x80U;
		}
	}
	zlrs_link_rx(&receiver->link, bytes, size, -60, 0, now_ms);
}

static void connect_saved(sim_t *sim, zlrs_profile_t profile)
{
	const uint8_t salt[ZLRS_PAIR_SALT_LEN] = {1,2,3,4,5,6,7,8};
	sim->tx.link.cfg.profile = profile;
	sim->rx.link.cfg.profile = profile;
	zlrs_link_set_peer(&sim->tx.link, sim->rx.link.dev_id, salt, sim->now_ms);
	zlrs_link_set_peer(&sim->rx.link, sim->tx.link.dev_id, salt, sim->now_ms);
	for (unsigned int i = 0U; i < 3000U && !both_connected(sim); i++) {
		sim_frame(sim);
	}
	CHECK(both_connected(sim));
}

static void test_replay_and_counters(void)
{
	static sim_t sim;
	sim_init(&sim);
	connect_saved(&sim, ZLRS_PROFILE_P1);
	zlrs_link_t *tx = &sim.tx.link;
	zlrs_link_t *rx = &sim.rx.link;
	const uint8_t byte = 0x5a;
	CHECK(zlrs_link_write(tx, &byte, 1U) == 1U);
	tx->arq_used = 1U;
	tx->arq_len = 1U;
	tx->arq_seq = 0U;
	tx->last_rx_seq = 32770U;
	zlrs_frame_t frame = {.type = ZLRS_PKT_DATA_DOWN, .seq = 1U,
		.body.data = {.ack = 1U, .lq_percent = 100U}};
	inject(&sim.tx, &frame, ZLRS_DATA_PREFIX_LEN, tx->session_key_in, sim.now_ms, false);
	CHECK(tx->last_rx_seq == 32770U && tx->queue_count == 1U && tx->arq_used == 1U);
	frame.seq = UINT32_MAX;
	inject(&sim.tx, &frame, ZLRS_DATA_PREFIX_LEN, tx->session_key_in, sim.now_ms, true);
	CHECK(tx->last_rx_seq == 32770U && tx->queue_count == 1U);
	frame.type = ZLRS_PKT_DATA_UP;
	inject(&sim.tx, &frame, ZLRS_DATA_PREFIX_LEN, tx->session_key_in, sim.now_ms, false);
	CHECK(tx->last_rx_seq == 32770U && tx->queue_count == 1U);
	frame.type = ZLRS_PKT_DATA_DOWN;
	frame.seq = 32771U;
	inject(&sim.tx, &frame, ZLRS_DATA_PREFIX_LEN, tx->session_key_in, sim.now_ms, false);
	CHECK(tx->last_rx_seq == 32771U && tx->queue_count == 0U && tx->arq_used == 0U);
	const uint32_t increasing[] = {65535U, 65536U, 0x7fffffffU, 0x80000000U, UINT32_MAX};
	for (unsigned int i = 0U; i < sizeof(increasing) / sizeof(increasing[0]); i++) {
		frame.seq = increasing[i];
		inject(&sim.tx, &frame, ZLRS_DATA_PREFIX_LEN, tx->session_key_in, sim.now_ms, false);
		CHECK(tx->last_rx_seq == increasing[i]);
	}
	frame.seq = 1U;
	inject(&sim.tx, &frame, ZLRS_DATA_PREFIX_LEN, tx->session_key_in, sim.now_ms, false);
	CHECK(tx->last_rx_seq == UINT32_MAX);

	/* Correct tags do not authorize wrong nonces, versions or out-of-domain hops. */
	const uint32_t baseline = rx->last_rx_seq;
	const uint32_t last_ms = rx->last_rx_ms;
	const uint32_t index = rx->frame_index;
	frame = (zlrs_frame_t){.type = ZLRS_PKT_SYNC, .seq = UINT32_MAX,
		.body.sync = {.version = 9U, .profile = 1U, .nonce_t = rx->sess_nonce_t,
			      .hop_index = 0U, .frame_index = index + 100U,
			      .band_plan = {.domain = ZLRS_DOMAIN_ISM2400}}};
	inject(&sim.rx, &frame, ZLRS_SYNC_PAYLOAD_LEN, rx->link_auth_in, sim.now_ms, false);
	CHECK(rx->last_rx_seq == baseline && rx->frame_index == index);
	frame.body.sync.version = ZLRS_PROTO_VERSION;
	frame.body.sync.profile = 0U;
	inject(&sim.rx, &frame, ZLRS_SYNC_PAYLOAD_LEN, rx->link_auth_in, sim.now_ms, false);
	CHECK(rx->last_rx_seq == baseline && rx->last_rx_ms == last_ms);
	frame.body.sync.profile = 1U;
	frame.body.sync.hop_index = rx->fhss.sequence_count;
	inject(&sim.rx, &frame, ZLRS_SYNC_PAYLOAD_LEN, rx->link_auth_in, sim.now_ms, false);
	CHECK(rx->last_rx_seq == baseline && rx->frame_index == index);
	frame = (zlrs_frame_t){.type = ZLRS_PKT_SESS_CFM, .seq = UINT32_MAX,
		.body.sess_cfm = {.nonce_r = rx->sess_nonce_r ^ 1U, .hop_index = 0U,
				  .frame_index = index + 100U,
				  .band_plan = {.domain = ZLRS_DOMAIN_ISM2400}}};
	inject(&sim.rx, &frame, ZLRS_SESS_CFM_PAYLOAD_LEN, rx->session_key_in, sim.now_ms, false);
	CHECK(rx->last_rx_seq == baseline && rx->last_rx_ms == last_ms);
	CHECK(zlrs_link_write(rx, &byte, 1U) == 1U);
	rx->arq_used = 1U;
	rx->arq_len = 1U;
	rx->arq_seq = 9U;
	frame.seq = baseline + 1U;
	frame.body.sess_cfm.nonce_r = rx->sess_nonce_r;
	inject(&sim.rx, &frame, ZLRS_SESS_CFM_PAYLOAD_LEN, rx->session_key_in, sim.now_ms + 1U, false);
	CHECK(rx->last_rx_seq == baseline + 1U && rx->frame_index == index + 100U);
	CHECK(rx->queue_count == 1U && rx->arq_used == 1U && rx->arq_seq == 9U);
	inject(&sim.rx, &frame, ZLRS_SESS_CFM_PAYLOAD_LEN, rx->session_key_in, sim.now_ms + 2U, false);
	CHECK(rx->last_rx_ms == sim.now_ms + 1U);
}

static void test_response_binding(void)
{
	static sim_t sim;
	sim_init(&sim);
	connect_saved(&sim, ZLRS_PROFILE_P1);
	zlrs_link_t *tx = &sim.tx.link;
	const uint32_t completed_nonce = tx->sess_nonce_t;
	uint8_t old_key[ZLRS_KEY_LEN];
	memcpy(old_key, tx->session_key_in, sizeof(old_key));
	const uint32_t old_baseline = tx->last_rx_seq;
	zlrs_frame_t frame = {.type = ZLRS_PKT_SESS_RESP, .seq = UINT32_MAX,
		.body.sess_resp = {.nonce_t = completed_nonce, .nonce_r = tx->sess_nonce_r ^ 1U}};
	tx->last_rx_ms = sim.now_ms;
	inject(&sim.tx, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, tx->link_auth_in, sim.now_ms, false);
	CHECK(tx->state == ZLRS_STATE_CONNECTED && tx->last_rx_seq == old_baseline);
	CHECK(memcmp(tx->session_key_in, old_key, sizeof(old_key)) == 0);
	/* Once silent, that hint can request a fresh challenge, never adopt the old answer. */
	sim.now_ms += tx->cfg.sync_period_ms + 1U;
	inject(&sim.tx, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, tx->link_auth_in, sim.now_ms, false);
	CHECK(tx->state == ZLRS_STATE_SEARCHING && tx->session_pending && !tx->session_valid);
	CHECK(tx->sess_nonce_t != completed_nonce && tx->sess_rx_seq == 0U);
	const uint32_t fresh = tx->sess_nonce_t;
	inject(&sim.tx, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, tx->link_auth_in, sim.now_ms, false);
	CHECK(tx->sess_nonce_t == fresh && tx->sess_rx_seq == 0U && !tx->session_valid);
	frame.body.sess_resp.nonce_t = fresh;
	frame.seq = 1U;
	inject(&sim.tx, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, tx->link_auth_in, sim.now_ms, true);
	CHECK(tx->sess_rx_seq == 0U && !tx->session_valid);
	inject(&sim.tx, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, tx->link_auth_in, sim.now_ms, false);
	CHECK(tx->session_valid && tx->sess_rx_seq == 1U && tx->last_rx_seq == 1U);
	memcpy(old_key, tx->session_key_in, sizeof(old_key));
	tx->arq_seq = 7U;
	tx->arq_used = 1U;
	frame.seq = 2U;
	inject(&sim.tx, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, tx->link_auth_in, sim.now_ms, false);
	CHECK(tx->arq_seq == 7U && tx->arq_used == 1U && tx->last_rx_seq == 2U);
	CHECK(memcmp(tx->session_key_in, old_key, sizeof(old_key)) == 0);
	frame.body.sess_resp.nonce_r ^= 1U;
	frame.seq = UINT32_MAX;
	inject(&sim.tx, &frame, ZLRS_SESS_RESP_PAYLOAD_LEN, tx->link_auth_in, sim.now_ms, false);
	CHECK(tx->last_rx_seq == 2U && tx->arq_seq == 7U && tx->arq_used == 1U);
	CHECK(memcmp(tx->session_key_in, old_key, sizeof(old_key)) == 0);
}

static void test_pairing_context(void)
{
	static sim_t sim;
	sim_init(&sim);
	zlrs_link_t *rx = &sim.rx.link;
	zlrs_link_press(rx, 0U);
	zlrs_frame_t frame = {.type = ZLRS_PKT_PAIR_REQ, .seq = 42U,
		.body.pair_req = {.dev_a = {1,2,3,4}, .nonce_a = 0U}};
	inject(&sim.rx, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, rx->auth_key, 1U, false);
	CHECK(rx->pair_request_valid && rx->pair_wait_cfm && rx->pair_nonce == 0U);
	const uint32_t nonce_b = rx->pair_nonce_b;
	frame.seq++;
	inject(&sim.rx, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, rx->auth_key, 2U, false);
	CHECK(rx->pair_nonce_b == nonce_b && rx->pair_rx_seq == 43U);
	frame.seq--;
	inject(&sim.rx, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, rx->auth_key, 3U, false);
	CHECK(rx->pair_rx_seq == 43U && rx->pair_nonce_b == nonce_b);
	/* The same nonce from another ID is a different request, not a retransmission. */
	frame.body.pair_req.dev_a[0] = 9U;
	frame.seq = 1U;
	inject(&sim.rx, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, rx->auth_key, 4U, false);
	CHECK(rx->peer_id[0] == 9U && rx->pair_nonce_b != nonce_b && rx->pair_rx_seq == 1U);
	frame = (zlrs_frame_t){.type = ZLRS_PKT_PAIR_CFM, .seq = UINT32_MAX,
		.body.pair = {.dev_a = {9,2,3,4}, .dev_b = {5,6,7,8},
			      .nonce_a = 0U, .nonce_b = rx->pair_nonce_b ^ 1U}};
	inject(&sim.rx, &frame, ZLRS_PAIR_PAYLOAD_LEN, rx->auth_key, 5U, false);
	CHECK(!rx->paired && rx->pair_rx_seq == 1U);
	frame.body.pair.nonce_b = rx->pair_nonce_b;
	frame.seq = 2U;
	inject(&sim.rx, &frame, ZLRS_PAIR_PAYLOAD_LEN, rx->auth_key, 6U, false);
	CHECK(rx->paired && rx->state == ZLRS_STATE_SEARCHING && rx->pair_rx_seq == 2U);
	rx->session_pending = true;
	rx->sess_rx_seq = 77U;
	rx->fhss.index = 100U;
	frame.seq = 3U;
	inject(&sim.rx, &frame, ZLRS_PAIR_PAYLOAD_LEN, rx->auth_key, 7U, false);
	CHECK(rx->session_pending && rx->sess_rx_seq == 77U && rx->fhss.index == 100U);
	frame.type = ZLRS_PKT_PAIR_REQ;
	frame.body.pair_req = (zlrs_pair_request_frame_t){.dev_a = {1,2,3,4}};
	frame.seq = 4U;
	inject(&sim.rx, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, rx->auth_key, 8U, false);
	CHECK(rx->peer_id[0] == 9U && rx->pair_rx_seq == 3U);
	/* Pairing a reflected/local identity is never permitted. */
	zlrs_link_press(rx, 9U);
	memcpy(frame.body.pair_req.dev_a, rx->dev_id, ZLRS_DEV_ID_LEN);
	inject(&sim.rx, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, rx->auth_key, 10U, false);
	CHECK(!rx->pair_request_valid && rx->pair_rx_seq == 0U);
}

static void test_confirmation_loss_and_exhaustion(void)
{
	static sim_t sim;
	for (zlrs_profile_t profile = ZLRS_PROFILE_P0; profile <= ZLRS_PROFILE_P1; profile++) {
		sim_init(&sim);
		sim.drop_confirms_only = true;
		sim.drop_tx = 3U;
		connect_saved(&sim, profile);
		CHECK(sim.drop_tx == 0U && sim.tx.link.stats.sess_cfm_tx >= 4U);
		CHECK(sim.rx.link.lq_started);
		const uint8_t byte = 0xa5;
		CHECK(zlrs_link_write(&sim.tx.link, &byte, 1U) == 1U);
		const uint32_t nonce = sim.tx.link.sess_nonce_t;
		sim.tx.link.tx_seq = UINT32_MAX;
		sim_frame(&sim);
		CHECK(sim.tx.link.sess_nonce_t != nonce && sim.tx.link.tx_seq != 0U);
		CHECK(sim.tx.link.queue_count == 1U);
		for (unsigned int i = 0U; i < 3000U &&
		     (!both_connected(&sim) || sim.rx.received_len == 0U); i++) {
			sim_frame(&sim);
		}
		CHECK(both_connected(&sim) && sim.rx.received_len == 1U && sim.rx.received[0] == byte);
		/* Responder exhaustion also stops the old key and requires a fresh challenge. */
		const uint32_t before = sim.tx.link.sess_nonce_t;
		sim.rx.link.tx_seq = UINT32_MAX;
		sim_frame(&sim);
		for (unsigned int i = 0U; i < 3000U && !both_connected(&sim); i++) {
			sim_frame(&sim);
		}
		CHECK(both_connected(&sim) && sim.tx.link.sess_nonce_t != before);
	}
}

static void test_entropy_failure(void)
{
	static sim_t sim;
	sim_init(&sim);
	const uint8_t salt[ZLRS_PAIR_SALT_LEN] = {1,2,3,4,5,6,7,8};
	sim.tx.random_failed = true;
	zlrs_link_press(&sim.tx.link, 0U);
	CHECK(sim.tx.link.state == ZLRS_STATE_UNPAIRED);
	zlrs_link_set_peer(&sim.tx.link, sim.rx.link.dev_id, salt, 0U);
	CHECK(!sim.tx.link.session_pending && !sim.tx.link.session_valid);
	CHECK(zlrs_link_slot(&sim.tx.link, ZLRS_SLOT_DATA_A, 1U) == NULL);
	sim.rx.random_failed = true;
	zlrs_link_set_peer(&sim.rx.link, sim.tx.link.dev_id, salt, 0U);
	zlrs_frame_t frame = {.type = ZLRS_PKT_SYNC, .seq = 1U,
		.body.sync = {.version = ZLRS_PROTO_VERSION, .profile = 1U, .nonce_t = 42U,
			.band_plan = {.domain = ZLRS_DOMAIN_ISM2400}}};
	inject(&sim.rx, &frame, ZLRS_SYNC_PAYLOAD_LEN, sim.rx.link.link_auth_in, 1U, false);
	CHECK(!sim.rx.link.session_pending && !sim.rx.link.session_valid);
	CHECK(zlrs_link_slot(&sim.rx.link, ZLRS_SLOT_DATA_B, 2U) == NULL);
	zlrs_link_press(&sim.rx.link, 3U);
	frame = (zlrs_frame_t){.type = ZLRS_PKT_PAIR_REQ, .seq = 1U,
		.body.pair_req = {.dev_a = {1,2,3,4}}};
	inject(&sim.rx, &frame, ZLRS_PAIR_REQ_PAYLOAD_LEN, sim.rx.link.auth_key, 4U, false);
	CHECK(!sim.rx.link.pair_request_valid && !sim.rx.link.pair_wait_cfm);
	sim.tx.random_failed = false;
	sim.rx.random_failed = false;
	zlrs_link_cancel_pair(&sim.rx.link, 5U);
	for (unsigned int i = 0U; i < 3000U && !both_connected(&sim); i++) {
		sim_frame(&sim);
	}
	CHECK(both_connected(&sim));
}

static void test_nonce_reuse(void)
{
	static sim_t sim;
	sim_init(&sim);
	/* Zero is valid on the first pairing round, but cannot seed a second one. */
	sim.tx.force_random = true;
	sim.tx.forced_random = 0U;
	zlrs_link_press(&sim.tx.link, 0U);
	CHECK(sim.tx.link.pair_request_valid && sim.tx.link.pair_nonce == 0U);
	zlrs_link_cancel_pair(&sim.tx.link, 1U);
	zlrs_link_press(&sim.tx.link, 2U);
	CHECK(sim.tx.link.state == ZLRS_STATE_UNPAIRED);
	sim.tx.force_random = false;
	connect_saved(&sim, ZLRS_PROFILE_P1);
	sim.tx.force_random = true;
	sim.tx.forced_random = sim.tx.link.sess_nonce_t;
	sim.tx.link.tx_seq = UINT32_MAX;
	sim_frame(&sim);
	CHECK(!sim.tx.link.session_pending && !sim.tx.link.session_valid);
	CHECK(zlrs_link_slot(&sim.tx.link, ZLRS_SLOT_DATA_A, sim.now_ms) == NULL);
	sim.tx.force_random = false;
	for (unsigned int i = 0U; i < 3000U && !both_connected(&sim); i++) {
		sim_frame(&sim);
	}
	CHECK(both_connected(&sim));
}

static void retune(node_t *node, bool subg)
{
	zlrs_link_cfg_t cfg = node->link.cfg;
	cfg.domain = subg ? ZLRS_DOMAIN_CN433 : ZLRS_DOMAIN_ISM2400;
	cfg.mod = zlrs_mod_flrc(subg ? 325000U : 2600000U);
	cfg.sync_period_ms = subg ? 600U : 100U;
	CHECK(zlrs_link_reconfigure_radio(&node->link, &cfg));
}

static void automatic_frame(sim_t *sim, zlrs_band_policy_t policies[2])
{
	sim_frame(sim);
	node_t *nodes[2] = {&sim->tx, &sim->rx};
	for (unsigned int i = 0; i < 2; i++) {
		zlrs_link_t *link = &nodes[i]->link;
		const zlrs_link_info_t info = zlrs_link_get_info(link);
		bool changed = false, target = false, probe = false;
		if (info.band_plan.switch_frame && info.frame_index >= info.band_plan.switch_frame - 1) {
			target = info.band_plan.target_subg; probe = !target; changed = true;
		} else {
			zlrs_band_observation_t o = {.now_ms = sim->now_ms,
				.last_rx_ms = info.last_rx_ms, .feedback_ms = info.last_peer_feedback_ms,
				.paired = info.paired, .pairing = info.state == ZLRS_STATE_PAIRING,
				.connected = info.session_valid && info.state == ZLRS_STATE_CONNECTED,
				.acquiring = info.radio_acquiring, .planned = info.band_plan.switch_frame != 0,
				.rssi_dbm = link->stats.last_rssi, .peer_rssi_dbm = info.peer_rssi_dbm,
				.lq_percent = link->stats.lq_percent, .peer_lq_percent = info.peer_lq_percent};
			const zlrs_band_action_t action = zlrs_band_policy_update(&policies[i], &o);
			if (action == ZLRS_BAND_SEARCH) {
				target = link->cfg.domain == ZLRS_DOMAIN_ISM2400; changed = true;
			} else if (action == ZLRS_BAND_PLAN) {
				CHECK(zlrs_link_plan_band(link, link->cfg.domain == ZLRS_DOMAIN_ISM2400,
							 info.frame_index + link->cfg.resync_every_n + 334U));
			}
		}
		if (changed) {
			retune(nodes[i], target);
			zlrs_link_acquire_radio(link, sim->now_ms);
			zlrs_band_policy_changed(&policies[i], target, probe, sim->now_ms, info.last_rx_ms);
		}
	}
}

static bool captured_band(const sim_t *sim, bool subg)
{
	const zlrs_fhss_domain_id_t domain = subg ? ZLRS_DOMAIN_CN433 : ZLRS_DOMAIN_ISM2400;
	return both_connected(sim) && sim->tx.link.cfg.domain == domain &&
		sim->rx.link.cfg.domain == domain && !sim->tx.link.radio_acquiring &&
		!sim->rx.link.radio_acquiring;
}

static void test_automatic_bands(void)
{
	static sim_t sim;
	for (unsigned int scenario = 0; scenario < 4; scenario++) {
		sim_init(&sim);
		sim.tx.link.cfg.automatic_band = sim.rx.link.cfg.automatic_band = true;
		connect_saved(&sim, ZLRS_PROFILE_P1);
		for (unsigned int i = 0; i < 100; i++) { sim_frame(&sim); }
		zlrs_band_policy_t policies[2];
		zlrs_band_policy_init(&policies[0], true, true, false, 0, sim.now_ms);
		zlrs_band_policy_init(&policies[1], true, false, false, 0, sim.now_ms);
		const uint32_t nonce = sim.tx.link.sess_nonce_t;
		const uint32_t seq = sim.tx.link.tx_seq;
		const uint8_t data[] = {3, 1, 4, 1, 5, 9};
		/* First payload reaches RX, but its ACK is lost before changing radio. */
		sim.drop_rx = 100;
		CHECK(zlrs_link_write(&sim.tx.link, data, sizeof(data)) == sizeof(data));
		for (unsigned int i = 0; i < 20; i++) { sim_frame(&sim); }
		CHECK(sim.rx.received_len == sizeof(data) && sim.tx.link.arq_used);
		sim.drop_rx = 0;
		CHECK(zlrs_link_write(&sim.rx.link, data, sizeof(data)) == sizeof(data));
		if (scenario < 2) {
			sim.block_24_tx = scenario == 0; sim.block_24_rx = true;
		} else {
			sim.rssi_24 = -100; sim.lose_band_plans = scenario == 3;
		}
		bool reached = false;
		for (unsigned int i = 0; i < 7000; i++) {
			automatic_frame(&sim, policies);
			if (captured_band(&sim, true) && sim.tx.link.queue_count == 0 &&
			    sim.rx.link.queue_count == 0) { reached = true; break; }
		}
		CHECK(reached && sim.tx.link.sess_nonce_t == nonce && sim.tx.link.tx_seq > seq);
		CHECK(sim.rx.received_len == sizeof(data) && sim.tx.received_len == sizeof(data));
		CHECK(memcmp(sim.rx.received, data, sizeof(data)) == 0);
		CHECK(memcmp(sim.tx.received, data, sizeof(data)) == 0);
		CHECK(sim.tx.link.stats.data_drop_bytes == 0 && sim.rx.link.stats.data_drop_bytes == 0);
		if (scenario == 0) {
			/* Blocked 2.4 GHz probe must return to the same SubG session. */
			bool tried = false, returned = false;
			for (unsigned int i = 0; i < 16000; i++) {
				automatic_frame(&sim, policies);
				tried |= sim.tx.link.cfg.domain == ZLRS_DOMAIN_ISM2400;
				if (tried && captured_band(&sim, true)) { returned = true; break; }
			}
			CHECK(tried && returned && sim.tx.link.sess_nonce_t == nonce);
			sim.block_24_tx = sim.block_24_rx = false;
			bool preferred = false;
			for (unsigned int i = 0; i < 18000; i++) {
				automatic_frame(&sim, policies);
				if (captured_band(&sim, false)) { preferred = true; break; }
			}
			CHECK(preferred && sim.tx.link.sess_nonce_t == nonce);
		}
	}
}

static void test_band_plan_security(void)
{
	static sim_t sim;
	sim_init(&sim);
	sim.tx.link.cfg.automatic_band = sim.rx.link.cfg.automatic_band = true;
	connect_saved(&sim, ZLRS_PROFILE_P1);
	zlrs_link_t *rx = &sim.rx.link;
	const uint32_t baseline = rx->last_rx_seq, index = rx->frame_index;
	const uint32_t events = rx->accepted_rx_events;
	const int16_t rssi = rx->stats.last_rssi;
	zlrs_frame_t f = {.type = ZLRS_PKT_SYNC, .seq = baseline + 1,
		.body.sync = {.version = ZLRS_PROTO_VERSION, .profile = 1,
			.nonce_t = rx->sess_nonce_t, .hop_index = rx->fhss.index, .frame_index = index,
			.band_plan = {.domain = ZLRS_DOMAIN_ISM2400, .automatic = 1,
				.target_subg = 1, .switch_frame = index + 20}}};
	inject(&sim.rx, &f, ZLRS_SYNC_PAYLOAD_LEN, rx->link_auth_in, sim.now_ms, true);
	CHECK(rx->last_rx_seq == baseline && rx->band_plan.switch_frame == 0);
	CHECK(rx->stats.last_rssi == rssi);
	f.body.sync.band_plan.domain = ZLRS_DOMAIN_CN433;
	inject(&sim.rx, &f, ZLRS_SYNC_PAYLOAD_LEN, rx->link_auth_in, sim.now_ms, false);
	CHECK(rx->last_rx_seq == baseline && rx->accepted_rx_events == events);
	f.body.sync.band_plan.domain = ZLRS_DOMAIN_ISM2400;
	f.body.sync.band_plan.switch_frame = index;
	inject(&sim.rx, &f, ZLRS_SYNC_PAYLOAD_LEN, rx->link_auth_in, sim.now_ms, false);
	CHECK(rx->last_rx_seq == baseline && rx->frame_index == index);
	f.body.sync.band_plan.switch_frame = index + 65536U;
	inject(&sim.rx, &f, ZLRS_SYNC_PAYLOAD_LEN, rx->link_auth_in, sim.now_ms, false);
	CHECK(rx->last_rx_seq == baseline && rx->accepted_rx_events == events);
	f.body.sync.band_plan.switch_frame = index + 20;
	f.body.sync.band_plan.target_subg = 0;
	inject(&sim.rx, &f, ZLRS_SYNC_PAYLOAD_LEN, rx->link_auth_in, sim.now_ms, false);
	CHECK(rx->last_rx_seq == baseline);
	f.body.sync.band_plan.target_subg = 1;
	inject(&sim.rx, &f, ZLRS_SYNC_PAYLOAD_LEN, rx->link_auth_in, sim.now_ms, false);
	CHECK(rx->last_rx_seq == baseline + 1 && rx->band_plan.switch_frame == index + 20);
	f.body.sync.band_plan.switch_frame++;
	inject(&sim.rx, &f, ZLRS_SYNC_PAYLOAD_LEN, rx->link_auth_in, sim.now_ms, false);
	CHECK(rx->band_plan.switch_frame == index + 20 && rx->accepted_rx_events == events + 1);
	CHECK(!zlrs_link_plan_band(rx, true, index + 20));
	CHECK(!zlrs_link_plan_band(&sim.tx.link, true, 0));
	const zlrs_radio_req_t *req = zlrs_link_slot(rx, ZLRS_SLOT_DATA_A, sim.now_ms);
	CHECK(req != NULL);
	zlrs_link_cfg_t cfg = rx->cfg; cfg.domain = ZLRS_DOMAIN_CN433;
	CHECK(!zlrs_link_reconfigure_radio(rx, &cfg) && rx->cfg.domain == ZLRS_DOMAIN_ISM2400);
	zlrs_link_complete(rx, ZLRS_RADIO_OK, 0, 0, 0, sim.now_ms);
	cfg.profile = ZLRS_PROFILE_P0;
	CHECK(!zlrs_link_reconfigure_radio(rx, &cfg));
}

void test_link(void)
{
	static sim_t sim;
	uint8_t tx_data[1024];
	uint8_t rx_data[768];
	printf("data link: pair, session, duplex ARQ, loss and backpressure\n");
	wire_hash = UINT64_C(14695981039346656037);
	wire_packets = 0U;
	sim_init(&sim);
	zlrs_link_press(&sim.tx.link, sim.now_ms);
	zlrs_link_press(&sim.rx.link, sim.now_ms);
	for (uint32_t i = 0U; i < 3000U && !both_connected(&sim); i++) {
		sim_frame(&sim);
	}
	CHECK(both_connected(&sim));
	CHECK(zlrs_link_get_info(&sim.tx.link).paired);
	CHECK(zlrs_link_get_info(&sim.rx.link).paired);
	for (uint16_t i = 0U; i < sizeof(tx_data); i++) {
		tx_data[i] = (uint8_t)(i * 37U + 11U);
	}
	for (uint16_t i = 0U; i < sizeof(rx_data); i++) {
		rx_data[i] = (uint8_t)(i * 19U + 7U);
	}
	CHECK(zlrs_link_write(&sim.tx.link, tx_data, sizeof(tx_data)) == sizeof(tx_data));
	CHECK(zlrs_link_write(&sim.rx.link, rx_data, sizeof(rx_data)) == sizeof(rx_data));
	sim.rx.blocked = true;
	sim.drop_tx = 4U;
	sim.drop_rx = 3U;
	for (uint32_t i = 0U; i < 300U; i++) {
		sim_frame(&sim);
	}
	CHECK(sim.rx.received_len == 0U);
	sim.rx.blocked = false;
	for (uint32_t i = 0U; i < 5000U && (sim.rx.received_len < sizeof(tx_data) ||
					    sim.tx.received_len < sizeof(rx_data));
	     i++) {
		sim_frame(&sim);
	}
	CHECK(sim.rx.received_len == sizeof(tx_data));
	CHECK(sim.tx.received_len == sizeof(rx_data));
	CHECK(memcmp(sim.rx.received, tx_data, sizeof(tx_data)) == 0);
	CHECK(memcmp(sim.tx.received, rx_data, sizeof(rx_data)) == 0);
	CHECK(zlrs_link_get_stats(&sim.tx.link)->tx_retries > 0U);
	CHECK(zlrs_link_get_stats(&sim.rx.link)->tx_retries > 0U);
	const uint32_t tx_feedback_before = zlrs_link_get_info(&sim.tx.link).last_peer_feedback_ms;
	const uint32_t rx_feedback_before = zlrs_link_get_info(&sim.rx.link).last_peer_feedback_ms;
	for (uint32_t i = 0U; i < 100U; i++) {
		sim_frame(&sim);
	}
	CHECK(zlrs_link_get_info(&sim.tx.link).peer_rssi_dbm == -60);
	CHECK(zlrs_link_get_info(&sim.rx.link).peer_rssi_dbm == -60);
	CHECK(zlrs_link_get_info(&sim.tx.link).last_peer_feedback_ms > tx_feedback_before);
	CHECK(zlrs_link_get_info(&sim.rx.link).last_peer_feedback_ms > rx_feedback_before);
	/* Golden bytes for this fixed protocol scenario. */
	CHECK(wire_packets == 200U);
	printf("wire v11: packets=%u hash=%016llx\n", wire_packets, (unsigned long long)wire_hash);
	CHECK(wire_hash == UINT64_C(0xd472ddab87624f6c));
	/* An authenticated DATA capture cannot be delivered twice, tampered with or reflected. */
	CHECK(captured_len > 0U);
	const uint16_t received = sim.rx.received_len;
	const uint32_t duplicates = sim.rx.link.stats.rx_dup;
	zlrs_link_rx(&sim.rx.link, captured, captured_len, -60, 0, sim.now_ms);
	CHECK(sim.rx.link.stats.rx_dup == duplicates + 1U);
	CHECK(sim.rx.received_len == received);
	const uint32_t bad_tags = sim.rx.link.stats.rx_bad_tag;
	captured[captured_len - 1U] ^= 0x80U;
	zlrs_link_rx(&sim.rx.link, captured, captured_len, -60, 0, sim.now_ms);
	CHECK(sim.rx.link.stats.rx_bad_tag == bad_tags + 1U);
	CHECK(sim.rx.received_len == received);
	captured[captured_len - 1U] ^= 0x80U;
	const uint32_t reflected = sim.tx.link.stats.rx_bad_tag;
	zlrs_link_rx(&sim.tx.link, captured, captured_len, -60, 0, sim.now_ms);
	CHECK(sim.tx.link.stats.rx_bad_tag == reflected + 1U);

	/* FIFO-prepared DATA keeps its ACK snapshot; a later peer packet must leave ACK pending. */
	const uint8_t queued[] = {0x33, 0x55, 0xaa};
	CHECK(zlrs_link_write(&sim.tx.link, queued, sizeof(queued)) == sizeof(queued));
	uint16_t prepared_size;
	const uint8_t *prepared = NULL;
	for (unsigned int i = 0U; i < 25U && prepared == NULL; ++i) {
		prepared = zlrs_link_prepare_data(&sim.tx.link, ZLRS_SLOT_DATA_A, sim.now_ms,
						  &prepared_size);
		if (prepared == NULL) {
			sim_frame(&sim);
		}
	}
	CHECK(prepared != NULL);
	if (prepared == NULL) {
		return;
	}
	zlrs_frame_t before;
	CHECK(zlrs_frame_decode(&before, prepared, prepared_size, ZLRS_TAG_LEN));
	uint8_t peer_frame[32];
	zlrs_frame_t packet = {.type = ZLRS_PKT_DATA_DOWN,
		.seq = sim.rx.link.tx_seq++, .body.data = {
			      .data_seq = sim.rx.link.arq_seq,
			      .ack = sim.tx.link.arq_seq & 1U,
			      .rssi_magnitude = 60U, .lq_percent = 100U,
			      .bytes = {0xabU}}};
	const uint16_t peer_size = zlrs_frame_len(ZLRS_DATA_PREFIX_LEN + 1U, ZLRS_TAG_LEN);
	CHECK(zlrs_frame_encode(&packet, peer_frame, sizeof(peer_frame), peer_size, ZLRS_TAG_LEN)
	      == peer_size);
	zlrs_tag(sim.rx.link.session_key_out, ZLRS_KEY_LEN, peer_frame, peer_size - ZLRS_TAG_LEN,
		 &peer_frame[peer_size - ZLRS_TAG_LEN]);
	const uint16_t delivered_before = sim.tx.received_len;
	zlrs_link_rx(&sim.tx.link, peer_frame, peer_size, -60, 0, sim.now_ms);
	CHECK(sim.tx.received_len == delivered_before + 1U);
	CHECK(sim.tx.received[delivered_before] == 0xabU);
	const zlrs_radio_req_t *request =
		zlrs_link_slot(&sim.tx.link, ZLRS_SLOT_DATA_A, sim.now_ms);
	CHECK(request != NULL && request->is_tx);
	if (request != NULL) {
		zlrs_frame_t after;
		CHECK(zlrs_frame_decode(&after, request->buf, request->size, ZLRS_TAG_LEN));
		CHECK(after.type == ZLRS_PKT_DATA_UP);
		CHECK(before.body.data.ack == after.body.data.ack);
		CHECK(sim.tx.link.arq_ack_pending);
		CHECK(request->size == zlrs_frame_len(ZLRS_DATA_PREFIX_LEN + sizeof(queued), ZLRS_TAG_LEN));
		CHECK(memcmp(after.body.data.bytes, queued, sizeof(queued)) == 0);
		zlrs_link_complete(&sim.tx.link, ZLRS_RADIO_OK, 0U, 0, 0, sim.now_ms);
	}

	bool ack_sent = false;
	for (unsigned int i = 0U; i < 25U && !ack_sent; ++i) {
		zlrs_link_skip_slot(&sim.tx.link, ZLRS_SLOT_DATA_B);
		request = zlrs_link_slot(&sim.tx.link, ZLRS_SLOT_DATA_A, sim.now_ms);
		if (request == NULL) {
			continue;
		}
		zlrs_frame_t ack;
		CHECK(zlrs_frame_decode(&ack, request->buf, request->size, ZLRS_TAG_LEN));
		if (ack.type == ZLRS_PKT_DATA_UP) {
			CHECK(request->size == zlrs_frame_len(ZLRS_DATA_PREFIX_LEN, ZLRS_TAG_LEN));
			CHECK(before.body.data.ack != ack.body.data.ack);
			CHECK(!sim.tx.link.arq_ack_pending);
			ack_sent = true;
		}
		zlrs_link_complete(&sim.tx.link, ZLRS_RADIO_OK, 0U, 0, 0, sim.now_ms);
	}
	CHECK(ack_sent);

	/* A receiver reboot derives fresh session keys and re-establishes both directions. */
	uint8_t root[ZLRS_KEY_LEN];
	uint8_t old_key[ZLRS_KEY_LEN];
	uint8_t old_capture[ZLRS_FRAME_MAX_LEN];
	const uint16_t old_size = captured_len;
	memcpy(old_capture, captured, old_size);
	memcpy(root, sim.rx.link.root_key, sizeof(root));
	memcpy(old_key, sim.rx.link.session_key_in, sizeof(old_key));
	zlrs_link_set_root_key(&sim.rx.link, root, sim.now_ms);
	for (unsigned int i = 0U; i < 3000U && !both_connected(&sim); ++i) {
		sim_frame(&sim);
	}
	CHECK(both_connected(&sim));
	CHECK(memcmp(old_key, sim.rx.link.session_key_in, sizeof(old_key)) != 0);
	const uint32_t old_session_tags = sim.rx.link.stats.rx_bad_tag;
	zlrs_link_rx(&sim.rx.link, old_capture, old_size, -60, 0, sim.now_ms);
	CHECK(sim.rx.link.stats.rx_bad_tag == old_session_tags + 1U);
	test_replay_and_counters();
	test_response_binding();
	test_pairing_context();
	test_confirmation_loss_and_exhaustion();
	test_entropy_failure();
	test_nonce_reuse();
	test_band_plan_security();
	test_automatic_bands();
	test_quality_window_boundary();
	test_data_sequence_wrap();
}

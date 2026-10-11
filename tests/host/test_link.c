/* SPDX-License-Identifier: Apache-2.0 */
/* Two-node DATA-only simulation: pairing, sessions, duplex ARQ and backpressure. */
#include "test.h"
#include <string.h>
#include "zlrs/link/link.h"

typedef struct {
	zlrs_link_t link;
	uint32_t rng;
	uint8_t received[4096];
	uint16_t received_len;
	bool blocked;
} node_t;

typedef struct {
	node_t tx;
	node_t rx;
	uint32_t now_ms;
	uint32_t frames;
	uint16_t drop_tx;
	uint16_t drop_rx;
} sim_t;

static uint32_t random_word(void *ctx)
{
	node_t *const node = ctx;
	uint32_t x = node->rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return node->rng = x != 0U ? x : 1U;
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
		.handshake = 1U,
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

/* Wire transcript of the existing v8 pairing/session/duplex/loss scenario. */
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
	    frame.type == ZLRS_PKT_DATA_UP && frame.payload_len > 4U) {
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
		if (*drops != 0U) {
			(*drops)--;
			zlrs_link_complete(receiver, ZLRS_RADIO_OK, 0U, 0, 0, sim->now_ms);
		} else {
			memcpy(dest->buf, source->buf, source->size);
			zlrs_link_complete(receiver, ZLRS_RADIO_OK, source->size, -60, 0,
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
	/* Captured from the pre-refactor 66a624f implementation with this fixed scenario. */
	CHECK(wire_packets == 200U);
	CHECK(wire_hash == UINT64_C(0x9111d88d2488e835));
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
	uint8_t peer_payload[] = {(uint8_t)sim.rx.link.arq_seq, (uint8_t)(sim.rx.link.arq_seq >> 8),
				  60U, 100U, 0xabU};
	uint8_t peer_frame[32];
	const uint8_t placeholder_tag[ZLRS_TAG_LEN] = {0};
	zlrs_frame_t packet = {.type = ZLRS_PKT_DATA_DOWN,
			       .seq = sim.rx.link.tx_seq++,
			       .flags = sim.tx.link.arq_seq & 1U ? ZLRS_FLAG_ACK : 0U,
			       .payload = peer_payload,
			       .payload_len = sizeof(peer_payload),
			       .tag = placeholder_tag};
	const uint16_t peer_size = zlrs_frame_len(sizeof(peer_payload), ZLRS_TAG_LEN);
	CHECK(zlrs_frame_encode(&packet, peer_frame, sizeof(peer_frame)) == peer_size);
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
		CHECK((before.flags & ZLRS_FLAG_ACK) == (after.flags & ZLRS_FLAG_ACK));
		CHECK(sim.tx.link.arq_ack_pending);
		CHECK(after.payload_len == sizeof(queued) + 4U);
		CHECK(memcmp(after.payload + 4U, queued, sizeof(queued)) == 0);
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
			CHECK(ack.payload_len == 2U);
			CHECK((before.flags & ZLRS_FLAG_ACK) != (ack.flags & ZLRS_FLAG_ACK));
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
}

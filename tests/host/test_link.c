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

static void init_node(node_t *node, uint8_t role, const uint8_t dev_id[4], const uint8_t root[32])
{
	zlrs_link_cfg_t cfg = {
		.role = role,
		.domain = ZLRS_DOMAIN_ISM2400,
		.second_domain = 0xFFU,
		.sync_slot = ZLRS_SYNC_SLOT_DATA,
		.profile = ZLRS_PROFILE_P1,
		.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 2600000U},
		.tx_power_dbm = 10,
		.rx_timeout_ms = 2U,
		.acq_timeout_ms = 8U,
		.sync_period_ms = 100U,
		.pair_period_ms = 100U,
		.pair_window_ms = 30000U,
		.data_mtu = 128U,
		.arq = 1U,
		.arq_timeout_ms = 30U,
		.data_every_n = 1U,
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

static void sim_slot(sim_t *sim, uint8_t slot)
{
	zlrs_link_t *const tx = &sim->tx.link;
	zlrs_link_t *const rx = &sim->rx.link;
	const zlrs_radio_req_t *const a = zlrs_link_slot(tx, slot, sim->now_ms);
	const zlrs_radio_req_t *const b = zlrs_link_slot(rx, slot, sim->now_ms);
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
	for (uint8_t slot = 0U; slot < 4U; slot++) {
		sim_slot(sim, (slot & 1U) == 0U ? ZLRS_SLOT_DATA_A : ZLRS_SLOT_DATA_B);
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
	const uint32_t tx_feedback_before =
		zlrs_link_get_info(&sim.tx.link).last_peer_feedback_ms;
	const uint32_t rx_feedback_before =
		zlrs_link_get_info(&sim.rx.link).last_peer_feedback_ms;
	for (uint32_t i = 0U; i < 100U; i++) {
		sim_frame(&sim);
	}
	CHECK(zlrs_link_get_info(&sim.tx.link).peer_rssi_dbm == -60);
	CHECK(zlrs_link_get_info(&sim.rx.link).peer_rssi_dbm == -60);
	CHECK(zlrs_link_get_info(&sim.tx.link).last_peer_feedback_ms > tx_feedback_before);
	CHECK(zlrs_link_get_info(&sim.rx.link).last_peer_feedback_ms > rx_feedback_before);
}

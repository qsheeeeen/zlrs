/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two-node link simulation:
 *   - two nodes, one TX and one RX, each running a control and a data link
 *   - "the air" is a function call, so drops, corruption and replays are all injectable
 *   - the superframe order matches the application: CTL_DOWN, CTL_UP, DATA_A, DATA_B, 6 ms each
 */

#include "test.h"

#include <string.h>

#include "zlrs/link/link.h"
#include "zlrs/link/rate.h"

#define SIM_FRAME_MS       6U
#define SIM_RATE_CTL       1300000U
#define SIM_RATE_DATA      2600000U
#define SIM_DATA_MTU       128U
#define SIM_FRAMES_PER_SEC (1000U / SIM_FRAME_MS)
#define SIM_MAX_NODES      3U /* the main pair plus an optional third node (peer or attacker) */

typedef struct node node_t;

typedef struct {
	node_t *node;
	uint8_t kind;
} link_ctx_t;

struct node {
	link_ctx_t ctl_ctx;
	link_ctx_t data_ctx;
	zlrs_link_t ctl;
	zlrs_link_t data;
	uint32_t rng;
	uint8_t dev_id[ZLRS_DEV_ID_LEN];

	/* callback records */
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint16_t rc_len;
	uint32_t rc_count;
	uint8_t tlm[ZLRS_LINK_TLM_MAX_LEN];
	uint16_t tlm_len;
	uint32_t tlm_count;
	uint8_t failsafe[ZLRS_RC_PAYLOAD_LEN];
	uint16_t failsafe_len;
	uint32_t failsafe_count;
	uint8_t stream[2048];
	uint32_t stream_len;
	uint8_t ctl_state;
	uint8_t data_state;
};

typedef struct {
	node_t tx;
	node_t rx;
	node_t nth; /* third node: bystander or attacker, only the three-node cases use it */
	bool nth_enabled;
	uint32_t now_ms;
	/* Superframe length in ms: 6 by default (~167 Hz), the 1000 Hz case sets 1 */
	uint32_t frame_ms;
	uint8_t root_key[ZLRS_KEY_LEN];
	int32_t drop_down;      /* how many more TX->RX deliveries to drop */
	int32_t drop_up;        /* how many more RX->TX deliveries to drop */
	int32_t drop_data_down; /* drop only TX->RX data frames (ARQ cases) */
	bool replay_data_next;  /* deliver the next data frame twice (dedup cases) */
	bool corrupt_next;
	bool replay_next;
	uint8_t down[ZLRS_FRAME_MAX_LEN];
	uint16_t down_len;
	uint8_t up[ZLRS_FRAME_MAX_LEN];
	uint16_t up_len;
	uint8_t sync[ZLRS_FRAME_MAX_LEN];
	uint16_t sync_len;
	uint8_t sess_resp[ZLRS_FRAME_MAX_LEN]; /* session response, kept for replay cases */
	uint16_t sess_resp_len;
} sim_t;

/*
 * Bench switches: everything defaults to production; a case can turn one off to show that the
 * link still works without it (doc/12-debug.md 3). Set before the nodes are created.
 */
typedef struct {
	uint8_t hopping;
	uint8_t handshake;
	uint8_t telemetry;
	uint8_t resync_every_n;
} sim_features_t;

static const sim_features_t sim_features_default = {
	.hopping = 1U,
	.handshake = 1U,
	.telemetry = 1U,
	.resync_every_n = 0U,
};

static sim_features_t sim_features;

/* ---- callbacks and the fake radio ---------------------------------------- */

static uint32_t next_random(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = (x != 0U) ? x : 0x12345678U;

	return *state;
}

static uint32_t fake_random(void *ctx)
{
	node_t *node = ctx;

	return next_random(&node->rng);
}

static int fake_submit(void *ctx, const zlrs_radio_req_t *req)
{
	(void)ctx;
	(void)req;

	return 0;
}

static void on_rc(void *ctx, const uint8_t *payload, uint16_t len)
{
	link_ctx_t *lc = ctx;
	node_t *node = lc->node;

	if (len > sizeof(node->rc)) {
		len = sizeof(node->rc);
	}
	memcpy(node->rc, payload, len);
	node->rc_len = len;
	node->rc_count++;
}

static void on_tlm(void *ctx, const uint8_t *payload, uint16_t len)
{
	link_ctx_t *lc = ctx;
	node_t *node = lc->node;

	if (len > sizeof(node->tlm)) {
		len = sizeof(node->tlm);
	}
	memcpy(node->tlm, payload, len);
	node->tlm_len = len;
	node->tlm_count++;
}

static void on_data(void *ctx, const uint8_t *data, uint16_t len)
{
	link_ctx_t *lc = ctx;
	node_t *node = lc->node;
	const uint32_t room = (uint32_t)sizeof(node->stream) - node->stream_len;

	if (len > room) {
		len = (uint16_t)room;
	}
	memcpy(&node->stream[node->stream_len], data, len);
	node->stream_len += len;
}

static void on_failsafe(void *ctx, const uint8_t *payload, uint16_t len)
{
	link_ctx_t *lc = ctx;
	node_t *node = lc->node;

	if (len > sizeof(node->failsafe)) {
		len = sizeof(node->failsafe);
	}
	memcpy(node->failsafe, payload, len);
	node->failsafe_len = len;
	node->failsafe_count++;
}

static void on_state(void *ctx, uint8_t state)
{
	link_ctx_t *lc = ctx;

	if (lc->kind == ZLRS_LINK_CONTROL) {
		lc->node->ctl_state = state;
	} else {
		lc->node->data_state = state;
	}
}

/* ---- nodes and the simulation --------------------------------------------- */

static void node_init(node_t *node, uint8_t role, const uint8_t *root_key,
		      const uint8_t dev_id[ZLRS_DEV_ID_LEN], uint32_t rng_seed)
{
	zlrs_radio_ops_t ops = {
		.submit = fake_submit,
		.random = fake_random,
		.ctx = node,
	};
	zlrs_link_cfg_t cfg = {
		.kind = ZLRS_LINK_CONTROL,
		.role = role,
		.domain = ZLRS_DOMAIN_FCC915,
		.sync_slot = ZLRS_SYNC_SLOT_CONTROL,
		.profile = ZLRS_PROFILE_P1,
		.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = SIM_RATE_CTL},
		.tx_power_dbm = 10,
		.rx_timeout_ms = 2U,
		.sync_period_ms = 1000U,
		.peer_timeout_ms = (role == ZLRS_ROLE_TX) ? 250U : 100U,
		.pair_period_ms = 100U,
		.pair_window_ms = 30000U,
		.data_mtu = 0U,
		.hopping = sim_features.hopping,
		.handshake = sim_features.handshake,
		.telemetry = sim_features.telemetry,
		.resync_every_n = sim_features.resync_every_n,
	};
	zlrs_link_events_t ctl_ev = {
		.on_rc = on_rc,
		.on_tlm = on_tlm,
		.on_state = on_state,
		.on_failsafe = on_failsafe,
		.ctx = &node->ctl_ctx,
	};
	zlrs_link_events_t data_ev = {
		.on_data = on_data,
		.on_state = on_state,
		.ctx = &node->data_ctx,
	};
	zlrs_link_cfg_t data_cfg;

	memset(node, 0, sizeof(*node));
	node->rng = (rng_seed != 0U) ? rng_seed : 0x2545F491U;
	memcpy(node->dev_id, dev_id, ZLRS_DEV_ID_LEN);
	node->ctl_ctx.node = node;
	node->ctl_ctx.kind = ZLRS_LINK_CONTROL;
	node->data_ctx.node = node;
	node->data_ctx.kind = ZLRS_LINK_DATA;

	data_cfg = cfg;
	data_cfg.kind = ZLRS_LINK_DATA;
	data_cfg.domain = ZLRS_DOMAIN_ISM2400;
	data_cfg.sync_slot = ZLRS_SYNC_SLOT_DATA;
	data_cfg.mod = zlrs_mod_flrc(SIM_RATE_DATA);
	data_cfg.peer_timeout_ms = 0U; /* no peer-loss check on the data link (09-protocol 3.7) */
	data_cfg.data_mtu = SIM_DATA_MTU;

	zlrs_link_init(&node->ctl, &cfg, &ops, &ctl_ev);
	zlrs_link_init(&node->data, &data_cfg, &ops, &data_ev);
	zlrs_link_set_dev_id(&node->ctl, dev_id);
	zlrs_link_set_dev_id(&node->data, dev_id);

	if (root_key != NULL) {
		zlrs_link_set_root_key(&node->ctl, root_key, 0U);
		zlrs_link_set_root_key(&node->data, root_key, 0U);
	}
}

/* features = NULL means production; bench cases pass their switches in */
static void sim_init_with(sim_t *sim, const sim_features_t *features)
{
	static uint8_t root_key[ZLRS_KEY_LEN];
	static const uint8_t tx_id[ZLRS_DEV_ID_LEN] = {0x11, 0x22, 0x33, 0x44};
	static const uint8_t rx_id[ZLRS_DEV_ID_LEN] = {0xaa, 0xbb, 0xcc, 0xdd};

	sim_features = (features != NULL) ? *features : sim_features_default;
	memset(sim, 0, sizeof(*sim));
	sim->frame_ms = SIM_FRAME_MS;
	for (uint8_t i = 0; i < sizeof(root_key); i++) {
		root_key[i] = (uint8_t)(i + 1U);
	}
	memcpy(sim->root_key, root_key, sizeof(sim->root_key));

	node_init(&sim->tx, ZLRS_ROLE_TX, root_key, tx_id, 0x11111111U);
	node_init(&sim->rx, ZLRS_ROLE_RX, root_key, rx_id, 0x22222222U);
}

static void sim_init(sim_t *sim)
{
	sim_init_with(sim, NULL);
}

static zlrs_link_t *link_of(node_t *node, uint8_t slot)
{
	return (slot == ZLRS_SLOT_CTL_DOWN || slot == ZLRS_SLOT_CTL_UP) ? &node->ctl : &node->data;
}

/*
 * What happens on air: only a receiver with matching band and sync word hears it (radio filter).
 * The comparison uses the sync word the transaction really carries: pairing uses the public constant.
 */
static bool sim_phy_match(const zlrs_radio_req_t *src, const zlrs_radio_req_t *dst,
			  const zlrs_link_t *src_link, const zlrs_link_t *dst_link)
{
	return src_link->cfg.domain == dst_link->cfg.domain && src->freq_hz == dst->freq_hz &&
	       memcmp(src->sync_word, dst->sync_word, ZLRS_SYNC_WORD_LEN) == 0;
}

/* Record frames the main pair sends: capture and buffers only care about the main pair */
static void sim_record_air(sim_t *sim, const zlrs_link_t *src_link, const uint8_t *buf,
			   uint16_t len)
{
	if (len > ZLRS_FRAME_MAX_LEN) {
		len = ZLRS_FRAME_MAX_LEN;
	}
	if (src_link == &sim->tx.ctl || src_link == &sim->tx.data) {
		memcpy(sim->down, buf, len);
		sim->down_len = len;
	} else if (src_link == &sim->rx.ctl || src_link == &sim->rx.data) {
		memcpy(sim->up, buf, len);
		sim->up_len = len;
	}
	if ((buf[0] >> 2) == (uint8_t)ZLRS_PKT_SYNC && sim->sync_len == 0U) {
		memcpy(sim->sync, buf, len);
		sim->sync_len = len;
	}
	if ((buf[0] >> 2) == (uint8_t)ZLRS_PKT_SESS_RESP && sim->sess_resp_len == 0U) {
		memcpy(sim->sess_resp, buf, len);
		sim->sess_resp_len = len;
	}
}

/* Hand a frame to a receive request (the radio writing those bytes into its buffer) */
static void sim_feed(sim_t *sim, zlrs_link_t *dst_link, const zlrs_radio_req_t *dst,
		     const uint8_t *buf, uint16_t len, bool primary)
{
	memcpy(dst->buf, buf, len);
	if (primary && sim->corrupt_next) {
		dst->buf[len - 1U] ^= 0xFFU;
		sim->corrupt_next = false;
	}
	zlrs_link_complete(dst_link, ZLRS_RADIO_OK, len, -60, 0, sim->now_ms);

	if (primary && sim->replay_next) {
		sim->replay_next = false;
		zlrs_link_complete(dst_link, ZLRS_RADIO_OK, len, -60, 0, sim->now_ms);
	}
}

static void sim_slot(sim_t *sim, uint8_t slot)
{
	zlrs_link_t *links[SIM_MAX_NODES];
	const zlrs_radio_req_t *reqs[SIM_MAX_NODES];
	bool served[SIM_MAX_NODES] = {false};
	uint8_t nodes = 0U;

	links[nodes] = link_of(&sim->tx, slot);
	reqs[nodes] = zlrs_link_slot(links[nodes], slot, sim->now_ms);
	nodes++;
	links[nodes] = link_of(&sim->rx, slot);
	reqs[nodes] = zlrs_link_slot(links[nodes], slot, sim->now_ms);
	nodes++;
	if (sim->nth_enabled) {
		links[nodes] = link_of(&sim->nth, slot);
		reqs[nodes] = zlrs_link_slot(links[nodes], slot, sim->now_ms);
		nodes++;
	}

	for (uint8_t i = 0U; i < nodes; i++) {
		uint16_t len;

		if (reqs[i] == NULL || !reqs[i]->is_tx) {
			continue;
		}

		len = reqs[i]->size;
		if (len > ZLRS_FRAME_MAX_LEN) {
			len = ZLRS_FRAME_MAX_LEN;
		}
		sim_record_air(sim, links[i], reqs[i]->buf, len);
		zlrs_link_complete(links[i], ZLRS_RADIO_OK, 0U, 0, 0, sim->now_ms);

		for (uint8_t j = 0U; j < nodes; j++) {
			/* The main pair's drop switches only touch the main pair (0 = TX node, 1 = RX node) */
			const bool primary = (i < 2U && j < 2U);
			int32_t *drop = NULL;

			if (j == i || reqs[j] == NULL || reqs[j]->is_tx || served[j] ||
			    !sim_phy_match(reqs[i], reqs[j], links[i], links[j])) {
				continue;
			}

			if (primary) {
				if (i == 0U && j == 1U) {
					drop = (slot == ZLRS_SLOT_DATA_A && sim->drop_data_down > 0)
						       ? &sim->drop_data_down
						       : &sim->drop_down;
				} else if (i == 1U && j == 0U) {
					drop = &sim->drop_up;
				}
			}

			if (drop != NULL && *drop > 0) {
				(*drop)--;
				zlrs_link_complete(links[j], ZLRS_RADIO_OK, 0U, 0, 0, sim->now_ms);
				served[j] = true;
				continue;
			}

			sim_feed(sim, links[j], reqs[j], reqs[i]->buf, len, primary);
			served[j] = true;
		}
	}

	/* A receive request that heard nothing still has to be finished off */
	for (uint8_t j = 0U; j < nodes; j++) {
		if (reqs[j] != NULL && !reqs[j]->is_tx && !served[j]) {
			zlrs_link_complete(links[j], ZLRS_RADIO_OK, 0U, 0, 0, sim->now_ms);
		}
	}
}

static void sim_run(sim_t *sim, uint32_t frames)
{
	for (uint32_t i = 0; i < frames; i++) {
		sim_slot(sim, ZLRS_SLOT_CTL_DOWN);
		sim_slot(sim, ZLRS_SLOT_CTL_UP);
		sim_slot(sim, ZLRS_SLOT_DATA_A);
		/* Data replay: the receiver dedups on the data sequence number and delivers once */
		if (sim->replay_data_next && sim->down_len > 0U &&
		    (sim->down[0] >> 2) == (uint8_t)ZLRS_PKT_DATA_UP) {
			sim->replay_data_next = false;
			zlrs_link_rx(&sim->rx.data, sim->down, sim->down_len, -60, 0, sim->now_ms);
		}
		sim_slot(sim, ZLRS_SLOT_DATA_B);

		zlrs_link_tick(&sim->tx.ctl, sim->now_ms);
		zlrs_link_tick(&sim->tx.data, sim->now_ms);
		zlrs_link_tick(&sim->rx.ctl, sim->now_ms);
		zlrs_link_tick(&sim->rx.data, sim->now_ms);

		sim->now_ms += sim->frame_ms;
	}
}

#define SIM_UNTIL(sim, cond, limit)                                                                \
	do {                                                                                       \
		for (int _i = 0; _i < (limit); _i++) {                                             \
			if (cond) {                                                                \
				break;                                                             \
			}                                                                          \
			sim_run((sim), 1U);                                                        \
		}                                                                                  \
	} while (0)

/* Push a frame straight into a link's receive buffer, bypassing the simulation */
static void sim_inject(sim_t *sim, zlrs_link_t *link, const uint8_t *frame, uint16_t len)
{
	zlrs_link_rx(link, frame, len, -60, 0, sim->now_ms);
}

static uint16_t build_data_frame(uint8_t *out, uint8_t type, uint16_t seq, uint8_t flags,
				 const uint8_t *payload, uint16_t len, const uint8_t *key)
{
	zlrs_frame_t frame = {
		.type = type,
		.flags = flags,
		.seq = seq,
		.payload = payload,
		.payload_len = len,
	};
	const uint16_t total = zlrs_frame_len(len, ZLRS_TAG_LEN);

	if (zlrs_frame_encode(&frame, out, total) != total - ZLRS_TAG_LEN) {
		return 0U;
	}
	zlrs_tag(key, ZLRS_KEY_LEN, out, ZLRS_FRAME_HDR_LEN + len, &out[ZLRS_FRAME_HDR_LEN + len]);

	return total;
}

/* Pair, set up the session and copy the pairing record to the data link (what the app does) */
static void pair_and_connect(sim_t *sim)
{
	zlrs_link_press(&sim->tx.ctl, sim->now_ms);
	zlrs_link_press(&sim->rx.ctl, sim->now_ms);

	SIM_UNTIL(sim,
		  sim->tx.ctl.state == ZLRS_STATE_SEARCHING &&
			  sim->rx.ctl.state == ZLRS_STATE_SEARCHING,
		  2000);

	/* Copy the pairing record (dev_id and nonces) to the data link, as the app does */
	zlrs_link_set_peer(&sim->tx.data, sim->rx.dev_id, sim->tx.ctl.pair_salt, sim->now_ms);
	zlrs_link_set_peer(&sim->rx.data, sim->tx.dev_id, sim->rx.ctl.pair_salt, sim->now_ms);

	SIM_UNTIL(sim,
		  sim->tx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim->rx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim->tx.data.state == ZLRS_STATE_CONNECTED &&
			  sim->rx.data.state == ZLRS_STATE_CONNECTED,
		  2000);
}

/*
 * Enable the third node. The three-node cases share it, differing only in parameters:
 *   - a bystander pair: its own dev_id, peer and salt
 *   - an attacker: a stolen dev_id (the main TX's) with the main RX as peer; a correct salt makes
 *     it a device that heard the pairing (able to impersonate), otherwise it only has the sync word
 */
static void sim_enable_nth(sim_t *sim, uint8_t role, const uint8_t dev_id[ZLRS_DEV_ID_LEN],
			   const uint8_t peer_id[ZLRS_DEV_ID_LEN],
			   const uint8_t salt[ZLRS_PAIR_SALT_LEN])
{
	node_init(&sim->nth, role, sim->root_key, dev_id, 0x33333333U);
	zlrs_link_set_peer(&sim->nth.ctl, peer_id, salt, sim->now_ms);
	zlrs_link_set_peer(&sim->nth.data, peer_id, salt, sim->now_ms);
	sim->nth_enabled = true;
}

/* ---- cases --------------------------------------------------------------- */

/*
 * Three nodes, first case: a bystander pair. Different radio sync words mean they cannot hear
 * each other at the physical layer, which is the basis of doc/10-security.md 7.2.
 */
static void test_third_node_isolation(void)
{
	sim_t sim;
	uint8_t chunk[16];
	static const uint8_t other_id[ZLRS_DEV_ID_LEN] = {0x55, 0x66, 0x77, 0x88};
	static const uint8_t other_peer[ZLRS_DEV_ID_LEN] = {0x99, 0xaa, 0xbb, 0xcc};
	static const uint8_t other_salt[ZLRS_PAIR_SALT_LEN] = {9U, 9U, 9U, 9U, 9U, 9U, 9U, 9U};

	printf("link: third node isolation\n");
	sim_init(&sim);
	pair_and_connect(&sim);

	sim_enable_nth(&sim, ZLRS_ROLE_TX, other_id, other_peer, other_salt);
	sim_run(&sim, 200U);

	/* The third node transmits but never hears the main pair */
	CHECK(sim.nth.ctl.stats.tx_frames > 0U);
	CHECK(sim.nth.ctl.stats.rx_frames == 0U);
	CHECK(sim.nth.ctl.stats.rx_bad_tag == 0U);

	/* And the main pair hears nothing of it either: not even a bad tag, so the radio filtered it */
	CHECK(sim.tx.ctl.stats.rx_bad_tag == 0U);
	CHECK(sim.rx.ctl.stats.rx_bad_tag == 0U);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* The main pair still passes data through */
	memset(chunk, 0x5aU, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/*
 * Three nodes, second case: an attacker has the main TX's dev_id and the sync word from the air,
 * but never heard the pairing (wrong salt). Its frames reach the receiver and fail the tag: this is
 * what mixing the pairing nonce into pair_key buys (doc/10-security.md 3).
 */
static void test_intruder_without_pair_salt(void)
{
	sim_t sim;
	uint8_t chunk[8];
	static const uint8_t wrong_salt[ZLRS_PAIR_SALT_LEN] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
	};
	uint32_t bad_before;

	printf("link: intruder without pairing salt\n");
	sim_init(&sim);
	pair_and_connect(&sim);

	/* The real TX goes quiet so the RX falls back to SEARCHING: the attacker's moment */
	sim.drop_down = 1000000;
	SIM_UNTIL(&sim, sim.rx.ctl.state == ZLRS_STATE_SEARCHING, 400);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);

	sim_enable_nth(&sim, ZLRS_ROLE_TX, sim.tx.dev_id, sim.rx.dev_id, wrong_salt);
	/* Sync words go out in the clear: copying one is enough to get frames into the receiver */
	memcpy(sim.nth.ctl.sync_word, sim.tx.ctl.sync_word, ZLRS_SYNC_WORD_LEN);
	memcpy(sim.nth.data.sync_word, sim.tx.data.sync_word, ZLRS_SYNC_WORD_LEN);

	bad_before = sim.rx.ctl.stats.rx_bad_tag;
	sim_run(&sim, 300U);

	/* Received, but nothing verified and no session came up */
	CHECK(sim.rx.ctl.stats.rx_bad_tag > bad_before);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(!sim.rx.ctl.session_valid);

	/* The real TX returns: the main pair rebuilds its link and the attacker cannot get in */
	sim.drop_down = 0;
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim.rx.ctl.state == ZLRS_STATE_CONNECTED,
		  4000);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* The main pair still passes data through */
	memset(chunk, 0x6bU, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/*
 * A group member that heard the pairing derives the same keys and can impersonate the real TX
 * to inject RC. That is the inherent cost documented in doc/10-security.md 7.2; this case pins it.
 */
static void test_group_member_impersonates(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];

	printf("link: group member impersonation (documented)\n");
	sim_init(&sim);
	pair_and_connect(&sim);

	/* Real TX quiet: drop all its deliveries and the RX falls back to SEARCHING after 100 ms */
	sim.drop_down = 1000000;
	SIM_UNTIL(&sim, sim.rx.ctl.state == ZLRS_STATE_SEARCHING, 400);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);

	/* The impersonator: same dev_id as the main TX, with the salt it heard */
	sim_enable_nth(&sim, ZLRS_ROLE_TX, sim.tx.dev_id, sim.rx.dev_id, sim.tx.ctl.pair_salt);

	SIM_UNTIL(&sim, sim.rx.ctl.state == ZLRS_STATE_CONNECTED, 2000);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* Inject one RC frame: the RX takes it for the real TX's stick data */
	memset(rc, 0x7bU, sizeof(rc));
	zlrs_link_set_rc(&sim.nth.ctl, rc, sizeof(rc));
	SIM_UNTIL(&sim, sim.rx.rc_count > 0U, 400);
	CHECK(sim.rx.rc_count > 0U);
	CHECK(sim.rx.rc_len == sizeof(rc));
	CHECK(memcmp(sim.rx.rc, rc, sizeof(rc)) == 0);
}

/* 32-bit ms timestamp wrap: timeouts use unsigned differences and must survive crossing zero */
static void test_time_wrap(void)
{
	sim_t sim;
	uint8_t chunk[8];

	printf("link: time wrap\n");
	sim_init(&sim);
	sim.now_ms = 0xFFFFFF00U; /* crosses 2^32 after about 42 frames */
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	sim_run(&sim, 200U);
	CHECK(sim.now_ms < 0x1000U); /* it really wrapped */
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	memset(chunk, 0x66U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/* 16-bit sequence wrap: the window compares signed differences, so new frames never look old */
static void test_seq_wrap(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t chunk[8];
	uint32_t rc_before;

	printf("link: sequence wrap\n");
	sim_init(&sim);
	pair_and_connect(&sim);

	/* Move the cursor near the wrap point instead of simulating 65536 frames */
	sim.tx.ctl.tx_seq = 0xFFF0U;
	sim.rx.ctl.last_rx_seq = 0xFFF0U;
	sim.rx.ctl.lq_last_seq = 0xFFF0U;
	memset(rc, 0x21U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));

	sim_run(&sim, 40U);
	CHECK(sim.tx.ctl.tx_seq < 0xFFF0U); /* the sequence really wrapped */

	rc_before = sim.rx.rc_count;
	sim_run(&sim, 20U);
	CHECK(sim.rx.rc_count > rc_before);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* The data link's sequence numbers wrap the same way */
	memset(chunk, 0x77U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

static void test_pairing(void)
{
	sim_t sim;
	uint8_t word_tx[ZLRS_SYNC_WORD_LEN];
	uint8_t word_rx[ZLRS_SYNC_WORD_LEN];
	uint8_t data_tx[ZLRS_SYNC_WORD_LEN];
	uint8_t data_rx[ZLRS_SYNC_WORD_LEN];

	printf("link: pairing\n");
	sim_init(&sim);

	/* Without a press the button does nothing */
	sim_run(&sim, 50U);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_UNPAIRED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_UNPAIRED);
	CHECK(sim.rx.rc_count == 0U);

	zlrs_link_press(&sim.tx.ctl, sim.now_ms);
	zlrs_link_press(&sim.rx.ctl, sim.now_ms);
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_SEARCHING &&
			  sim.rx.ctl.state == ZLRS_STATE_SEARCHING,
		  2000);

	CHECK(sim.tx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(sim.tx.ctl.paired && sim.rx.ctl.paired);
	CHECK(memcmp(sim.tx.ctl.peer_id, sim.rx.dev_id, ZLRS_DEV_ID_LEN) == 0);
	CHECK(memcmp(sim.rx.ctl.peer_id, sim.tx.dev_id, ZLRS_DEV_ID_LEN) == 0);

	memcpy(word_tx, sim.tx.ctl.sync_word, sizeof(word_tx));
	memcpy(word_rx, sim.rx.ctl.sync_word, sizeof(word_rx));
	memcpy(data_tx, sim.tx.data.sync_word, sizeof(data_tx));
	memcpy(data_rx, sim.rx.data.sync_word, sizeof(data_rx));
	CHECK(memcmp(word_tx, word_rx, sizeof(word_tx)) == 0); /* both ends agree */
	CHECK(memcmp(data_tx, data_rx, sizeof(data_tx)) == 0);
	CHECK(memcmp(word_tx, data_tx, sizeof(word_tx)) != 0); /* domain separation */

	/* Run a few more superframes until a SYNC shows up on air */
	sim_run(&sim, 5U);
	CHECK(sim.sync_len > 0U);

	/* The pairing window expires on its own */
	sim_init(&sim);
	zlrs_link_press(&sim.tx.ctl, sim.now_ms);
	sim_run(&sim, (30000U / SIM_FRAME_MS) + 200U);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_UNPAIRED);
	CHECK(sim.tx.ctl.stats.tx_frames > 10U); /* PAIR_REQ went out the whole time */
	CHECK(!sim.tx.ctl.paired);
}

/* Little-endian u32 into a payload: the pairing messages carry the two nonces that way */
static void pair_put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/*
 * A repeated PAIR_REQ must not restart the responder's half of the handshake: the initiator
 * repeats the request until it hears an answer, and the confirmation it finally sends echoes the
 * responder nonce from that answer. Drawing a fresh nonce per repeat made that check reject the
 * confirmation, so the initiator went to SEARCHING while the responder stayed in PAIRING.
 */
static void test_pairing_repeat_req(void)
{
	sim_t sim;
	uint8_t payload[2U * ZLRS_DEV_ID_LEN + ZLRS_PAIR_SALT_LEN + 1U + ZLRS_RC_PAYLOAD_LEN];
	uint8_t frame[ZLRS_FRAME_MAX_LEN];
	const uint32_t nonce_a = 0x11111111U;
	const uint32_t other_a = 0x22222222U;
	uint32_t first_b = 0U;
	uint16_t len;

	printf("link: pairing survives repeated requests\n");
	sim_init(&sim);

	zlrs_link_press(&sim.rx.ctl, sim.now_ms);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_PAIRING);

	for (int round = 0; round < 2; round++) {
		memset(payload, 0, sizeof(payload));
		memcpy(&payload[0], sim.tx.dev_id, ZLRS_DEV_ID_LEN);
		pair_put_u32(&payload[2U * ZLRS_DEV_ID_LEN], nonce_a);
		len = build_data_frame(frame, ZLRS_PKT_PAIR_REQ, 0U, 0U, payload, sizeof(payload),
				       sim.tx.ctl.auth_key);
		CHECK(len > 0U);
		sim_inject(&sim, &sim.rx.ctl, frame, len);

		if (round == 0) {
			first_b = sim.rx.ctl.pair_nonce_b;
			CHECK(sim.rx.ctl.pair_wait_cfm);
		} else {
			/* Same initiator nonce: keep the answer that is already on its way */
			CHECK(sim.rx.ctl.pair_nonce_b == first_b);
		}
	}

	/* A new initiator nonce is a new handshake and does get a fresh responder nonce */
	memset(payload, 0, sizeof(payload));
	memcpy(&payload[0], sim.tx.dev_id, ZLRS_DEV_ID_LEN);
	pair_put_u32(&payload[2U * ZLRS_DEV_ID_LEN], other_a);
	len = build_data_frame(frame, ZLRS_PKT_PAIR_REQ, 0U, 0U, payload, sizeof(payload),
			       sim.tx.ctl.auth_key);
	CHECK(len > 0U);
	sim_inject(&sim, &sim.rx.ctl, frame, len);
	CHECK(sim.rx.ctl.pair_nonce == other_a);
	CHECK(sim.rx.ctl.pair_nonce_b != first_b);

	/* The confirmation reaches the responder: it commits the salt and moves on */
	{
		const uint32_t a = sim.rx.ctl.pair_nonce;
		const uint32_t b = sim.rx.ctl.pair_nonce_b;

		memset(payload, 0, sizeof(payload));
		memcpy(&payload[0], sim.tx.dev_id, ZLRS_DEV_ID_LEN);
		memcpy(&payload[ZLRS_DEV_ID_LEN], sim.rx.dev_id, ZLRS_DEV_ID_LEN);
		pair_put_u32(&payload[2U * ZLRS_DEV_ID_LEN], a);
		pair_put_u32(&payload[2U * ZLRS_DEV_ID_LEN + 4U], b);
		len = build_data_frame(frame, ZLRS_PKT_PAIR_CFM, 0U, 0U, payload, sizeof(payload),
				       sim.tx.ctl.auth_key);
		CHECK(len > 0U);
		sim_inject(&sim, &sim.rx.ctl, frame, len);
		CHECK(sim.rx.ctl.paired);
		CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);

		/* The responder then repeats its answer; the initiator answers it again */
		memset(payload, 0, sizeof(payload));
		memcpy(&payload[0], sim.tx.dev_id, ZLRS_DEV_ID_LEN);
		memcpy(&payload[ZLRS_DEV_ID_LEN], sim.rx.dev_id, ZLRS_DEV_ID_LEN);
		pair_put_u32(&payload[2U * ZLRS_DEV_ID_LEN], a);
		pair_put_u32(&payload[2U * ZLRS_DEV_ID_LEN + 4U], b);
		len = build_data_frame(frame, ZLRS_PKT_PAIR_RESP, 0U, 0U, payload, sizeof(payload),
				       sim.tx.ctl.auth_key);
		CHECK(len > 0U);

		sim.tx.ctl.paired = true;
		sim.tx.ctl.state = ZLRS_STATE_SEARCHING;
		sim.tx.ctl.pair_nonce = a;
		sim.tx.ctl.pair_cfm_pending = false;
		sim_inject(&sim, &sim.tx.ctl, frame, len);
		CHECK(sim.tx.ctl.pair_cfm_pending);
	}
}

/*
 * A confirm that never arrives must not strand the pairing. The two ends run their own slot phases
 * on air, so one PAIR_CFM per handshake is a coin flip: the initiator repeats the confirm, and the
 * responder keeps repeating its answer until it hears one.
 */
static void test_pairing_lost_confirm(void)
{
	sim_t sim;

	printf("link: pairing survives a lost confirm\n");
	sim_init(&sim);

	zlrs_link_press(&sim.tx.ctl, sim.now_ms);
	zlrs_link_press(&sim.rx.ctl, sim.now_ms);

	/* Let the request and the answer through, then drop the first confirms on the floor */
	SIM_UNTIL(&sim, sim.rx.ctl.pair_wait_cfm, 200);
	CHECK(sim.rx.ctl.pair_wait_cfm);
	sim.drop_down = 4;

	/* The responder hears a later repeat and finishes */
	SIM_UNTIL(&sim, sim.rx.ctl.state == ZLRS_STATE_SEARCHING, 400);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(sim.rx.ctl.paired);
	CHECK(sim.rx.ctl.stats.pair_cfm_rx >= 1U);
	CHECK(sim.tx.ctl.stats.pair_cfm_tx > 1U);
	CHECK(sim.tx.ctl.stats.pair_req_tx > 0U);
	CHECK(sim.rx.ctl.stats.pair_resp_tx > 1U); /* the answer repeated while it waited */

	SIM_UNTIL(&sim, sim.tx.ctl.state == ZLRS_STATE_SEARCHING, 400);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(sim.tx.ctl.paired);
	/* Both ends keep the same pair salt, or the keys they derive would differ */
	CHECK(memcmp(sim.tx.ctl.pair_salt, sim.rx.ctl.pair_salt, ZLRS_PAIR_SALT_LEN) == 0);
	CHECK(memcmp(sim.tx.ctl.sync_word, sim.rx.ctl.sync_word, ZLRS_SYNC_WORD_LEN) == 0);
}

/*
 * The pair key comes from a KDF context built out of both ids in a canonical order. That order has
 * to come out the same on both ends for *every* id pair: with these ids the first byte says "no
 * swap" on one end while a later byte says "swap" on the other, which is how two ends that agreed
 * on the whole record still derived different keys and never reached CONNECTED.
 */
static void test_pairing_id_order(void)
{
	static const uint8_t dev_a[ZLRS_DEV_ID_LEN] = {0xf8, 0x89, 0x3f, 0x95};
	static const uint8_t dev_b[ZLRS_DEV_ID_LEN] = {0x58, 0xaa, 0x34, 0x89};
	const uint8_t salt[ZLRS_PAIR_SALT_LEN] = {0x70, 0xb3, 0xcf, 0x1c, 0x25, 0x4e, 0x51, 0x99};
	sim_t sim;

	printf("link: pairing derives one key for both ends\n");
	sim_init(&sim);

	zlrs_link_set_dev_id(&sim.tx.ctl, dev_a);
	zlrs_link_set_dev_id(&sim.rx.ctl, dev_b);
	zlrs_link_set_peer(&sim.tx.ctl, dev_b, salt, sim.now_ms);
	zlrs_link_set_peer(&sim.rx.ctl, dev_a, salt, sim.now_ms);
	CHECK(memcmp(sim.tx.ctl.pair_key, sim.rx.ctl.pair_key, ZLRS_KEY_LEN) == 0);
	CHECK(memcmp(sim.tx.ctl.sync_word, sim.rx.ctl.sync_word, ZLRS_SYNC_WORD_LEN) == 0);

	/* ... and with the roles the other way round, which is the direction that used to break */
	zlrs_link_set_dev_id(&sim.tx.ctl, dev_b);
	zlrs_link_set_dev_id(&sim.rx.ctl, dev_a);
	zlrs_link_set_peer(&sim.tx.ctl, dev_a, salt, sim.now_ms);
	zlrs_link_set_peer(&sim.rx.ctl, dev_b, salt, sim.now_ms);
	CHECK(memcmp(sim.tx.ctl.pair_key, sim.rx.ctl.pair_key, ZLRS_KEY_LEN) == 0);
}

/*
 * A lost session confirm must not strand the link either: the responder repeats its answer while it
 * has no session, and the initiator keeps a window on the sync channel for a moment after entering
 * CONNECTED, so one lost confirm costs a few frames instead of waiting for the resync cadence.
 */
static void test_session_lost_confirm(void)
{
	sim_t sim;

	printf("link: session survives a lost confirm\n");
	sim_init(&sim);

	zlrs_link_press(&sim.tx.ctl, sim.now_ms);
	zlrs_link_press(&sim.rx.ctl, sim.now_ms);
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_SEARCHING &&
			  sim.rx.ctl.state == ZLRS_STATE_SEARCHING,
		  2000);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);

	/* Wait for the answer, then drop the single frame that would confirm it */
	SIM_UNTIL(&sim, sim.rx.ctl.session_pending, 400);
	CHECK(sim.rx.ctl.session_pending);
	sim.drop_down = 1;
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim.rx.ctl.state == ZLRS_STATE_CONNECTED,
		  600);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.tx.ctl.stats.sess_cfm_tx > 1U);
	CHECK(sim.rx.ctl.stats.sess_resp_tx > 1U);
	CHECK(sim.tx.ctl.fhss.index == sim.rx.ctl.fhss.index);
}

static void test_fresh_session_uplink(void)
{
	sim_t sim;
	const zlrs_radio_req_t *listen;
	const zlrs_radio_req_t *send;
	uint8_t chunk[16] = {0};

	printf("link: fresh-session uplink frequency matches on both links\n");
	sim_init(&sim);
	pair_and_connect(&sim);
	sim.tx.ctl.state_ms = sim.rx.ctl.state_ms = sim.now_ms;
	sim.tx.data.state_ms = sim.rx.data.state_ms = sim.now_ms;
	sim.rx.data.cfg.arq = 0U;

	listen = zlrs_link_slot(&sim.tx.ctl, ZLRS_SLOT_CTL_UP, sim.now_ms);
	send = zlrs_link_slot(&sim.rx.ctl, ZLRS_SLOT_CTL_UP, sim.now_ms);
	CHECK(listen != NULL && send != NULL);
	if (listen != NULL && send != NULL) {
		CHECK(!listen->is_tx && send->is_tx);
		CHECK(listen->freq_hz == send->freq_hz);
	}
	CHECK(zlrs_link_write(&sim.rx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	listen = zlrs_link_slot(&sim.tx.data, ZLRS_SLOT_DATA_B, sim.now_ms);
	send = zlrs_link_slot(&sim.rx.data, ZLRS_SLOT_DATA_B, sim.now_ms);
	CHECK(listen != NULL && send != NULL);
	if (listen != NULL && send != NULL) {
		CHECK(!listen->is_tx && send->is_tx);
		CHECK(listen->freq_hz == send->freq_hz);
	}
}

static void test_data_reacquire_epoch(void)
{
	sim_t sim;
	uint8_t chunk[64] = {0};
	uint32_t syncs_before;

	printf("link: silent data reacquires a mismatched frame and hop epoch\n");
	sim_init(&sim);
	sim.tx.data.cfg.resync_every_n = sim.rx.data.cfg.resync_every_n = 5U;
	pair_and_connect(&sim);
	sim_run(&sim, 100U);
	sim.rx.data.frame_index += 2U;
	zlrs_fhss_set_index(&sim.rx.data.fhss, sim.rx.data.fhss.index + 13U);
	sim.rx.data.last_rx_ms = sim.now_ms - sim.rx.data.cfg.sync_period_ms;
	syncs_before = sim.rx.data.stats.rx_syncs;
	sim_run(&sim, 20U);
	CHECK(sim.rx.data.stats.rx_syncs > syncs_before);
	CHECK(sim.rx.data.frame_index == sim.tx.data.frame_index);
	CHECK(sim.rx.data.fhss.index == sim.tx.data.fhss.index);
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	CHECK(zlrs_link_write(&sim.rx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len == sizeof(chunk) && sim.tx.stream_len == sizeof(chunk),
		  200);
	CHECK(sim.rx.stream_len == sizeof(chunk));
	CHECK(sim.tx.stream_len == sizeof(chunk));
}

static void test_session(void)
{
	sim_t sim;
	uint8_t saved[ZLRS_FRAME_MAX_LEN];
	uint16_t saved_len;
	uint32_t nonce;

	printf("link: session\n");
	sim_init(&sim);
	pair_and_connect(&sim);

	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.tx.data.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.data.state == ZLRS_STATE_CONNECTED);

	/* Session keys are directional: our out is the peer's in, and out != in on one device */
	CHECK(memcmp(sim.tx.ctl.session_key_out, sim.rx.ctl.session_key_in, ZLRS_KEY_LEN) == 0);
	CHECK(memcmp(sim.tx.ctl.session_key_in, sim.rx.ctl.session_key_out, ZLRS_KEY_LEN) == 0);
	CHECK(memcmp(sim.tx.ctl.session_key_out, sim.tx.ctl.session_key_in, ZLRS_KEY_LEN) != 0);
	/* Both nonces reach both ends: the TX's nonce_t and the RX's nonce_r */
	CHECK(sim.tx.ctl.sess_nonce_t == sim.rx.ctl.sess_nonce_t);
	CHECK(sim.tx.ctl.sess_nonce_r == sim.rx.ctl.sess_nonce_r);
	CHECK(memcmp(sim.tx.data.session_key_out, sim.rx.data.session_key_in, ZLRS_KEY_LEN) == 0);
	CHECK(memcmp(sim.tx.ctl.session_key_out, sim.tx.data.session_key_out, ZLRS_KEY_LEN) != 0);

	/* Once connected the RX stops listening for SYNC: replaying the current one changes nothing */
	nonce = sim.rx.ctl.sess_nonce_t;
	saved_len = sim.sync_len;
	CHECK(saved_len > 0U);
	saved_len = (saved_len > sizeof(saved)) ? (uint16_t)sizeof(saved) : saved_len;
	memcpy(saved, sim.sync, saved_len);
	sim_inject(&sim, &sim.rx.ctl, saved, saved_len);
	CHECK(sim.rx.ctl.sess_nonce_t == nonce);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
}

/*
 * Reflection: our own frame comes straight back. With directional tag keys (out to send, in to
 * verify) such frames must die at the tag check, never count as the peer's.
 */
static void test_reflection(void)
{
	sim_t sim;
	uint8_t frame[ZLRS_FRAME_MAX_LEN];
	uint16_t len;
	uint32_t bad_tag;
	uint32_t counter;
	uint32_t nonce;

	printf("link: reflection\n");
	sim_init(&sim);
	pair_and_connect(&sim);

	/* Grab a real TX -> RX frame off the air (RC) */
	sim_slot(&sim, ZLRS_SLOT_CTL_DOWN);
	CHECK(sim.down_len > 0U);
	CHECK((sim.down[0] >> 2) == (uint8_t)ZLRS_PKT_RC);
	len = sim.down_len;
	memcpy(frame, sim.down, len);

	/* Grab a real RX -> TX frame off the air (TLM) */
	sim_slot(&sim, ZLRS_SLOT_CTL_UP);
	CHECK(sim.up_len > 0U);
	CHECK((sim.up[0] >> 2) == (uint8_t)ZLRS_PKT_TLM);

	/* TX hears its own RC: the tag was computed local-first, verification is peer-first: drop it */
	counter = sim.tx.ctl.stats.rx_frames;
	bad_tag = sim.tx.ctl.stats.rx_bad_tag;
	sim_inject(&sim, &sim.tx.ctl, frame, len);
	CHECK(sim.tx.ctl.stats.rx_bad_tag == bad_tag + 1U);
	CHECK(sim.tx.ctl.stats.rx_frames == counter);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);

	/* RX hears its own TLM: same, it must not be taken for the TX's telemetry */
	counter = sim.rx.tlm_count;
	bad_tag = sim.rx.ctl.stats.rx_bad_tag;
	sim_inject(&sim, &sim.rx.ctl, sim.up, sim.up_len);
	CHECK(sim.rx.ctl.stats.rx_bad_tag == bad_tag + 1U);
	CHECK(sim.rx.tlm_count == counter);

	/* Make the TX declare peer loss and roll a new nonce, which ages the SYNC on air */
	nonce = sim.tx.ctl.sess_nonce_t;
	sim.drop_up = 500;
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_SEARCHING && sim.tx.ctl.sess_nonce_t != nonce,
		  2000);
	sim.drop_up = 0;
	CHECK(sim.tx.ctl.sess_nonce_t != nonce);

	/* TX hears its own old SYNC: it must not drag the session back to the old nonce */
	CHECK(sim.sync_len > 0U);
	nonce = sim.tx.ctl.sess_nonce_t;
	bad_tag = sim.tx.ctl.stats.rx_bad_tag;
	sim_inject(&sim, &sim.tx.ctl, sim.sync, sim.sync_len);
	CHECK(sim.tx.ctl.stats.rx_bad_tag == bad_tag + 1U);
	CHECK(sim.tx.ctl.sess_nonce_t == nonce);
}

/*
 * Re-pairing replaces the whole subtree: pair_key mixes in the two pairing nonces, so sync words,
 * hop tables and tag keys all change and frames from the old pairing never verify again.
 */
static void test_pairing_rotation(void)
{
	sim_t sim;
	uint8_t salt_before[ZLRS_PAIR_SALT_LEN];
	uint8_t auth_before[ZLRS_KEY_LEN];
	uint8_t sync[ZLRS_FRAME_MAX_LEN];
	uint16_t sync_len;
	uint32_t bad_tag;
	uint32_t nonce;

	printf("link: pairing rotation\n");
	sim_init(&sim);
	pair_and_connect(&sim);

	CHECK(sim.sync_len > 0U);
	sync_len = sim.sync_len;
	memcpy(sync, sim.sync, sync_len);
	memcpy(salt_before, sim.tx.ctl.pair_salt, sizeof(salt_before));
	memcpy(auth_before, sim.tx.ctl.link_auth_out, sizeof(auth_before));

	/* Pair again (exactly the path a re-bind takes) */
	zlrs_link_press(&sim.tx.ctl, sim.now_ms);
	zlrs_link_press(&sim.rx.ctl, sim.now_ms);
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_SEARCHING &&
			  sim.rx.ctl.state == ZLRS_STATE_SEARCHING,
		  2000);

	/* New pairing material, so the sync words and tag keys change with it */
	CHECK(memcmp(sim.tx.ctl.pair_salt, salt_before, ZLRS_PAIR_SALT_LEN) != 0);
	CHECK(memcmp(sim.tx.ctl.link_auth_out, auth_before, ZLRS_KEY_LEN) != 0);
	/* Both ends still derive the same set */
	CHECK(memcmp(sim.tx.ctl.sync_word, sim.rx.ctl.sync_word, ZLRS_SYNC_WORD_LEN) == 0);
	CHECK(memcmp(sim.tx.ctl.link_auth_out, sim.rx.ctl.link_auth_in, ZLRS_KEY_LEN) == 0);

	/* A SYNC recorded under the old pairing: the keys changed, so it must be dropped */
	bad_tag = sim.rx.ctl.stats.rx_bad_tag;
	nonce = sim.rx.ctl.sess_nonce_t;
	sim_inject(&sim, &sim.rx.ctl, sync, sync_len);
	CHECK(sim.rx.ctl.stats.rx_bad_tag == bad_tag + 1U);
	CHECK(sim.rx.ctl.sess_nonce_t == nonce);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);
}

/*
 * Replaying an old SYNC (the weakness tracked in 09-protocol): the RX only adopts a session after
 * a confirm tagged with the new session key, so a replayed SYNC only earns a response carrying a
 * fresh nonce_r and no old keys. An old SESS_RESP fails at the TX for the same reason.
 */
static void test_session_replay(void)
{
	sim_t sim;
	uint8_t sync[ZLRS_FRAME_MAX_LEN];
	uint16_t sync_len;
	uint8_t resp[ZLRS_FRAME_MAX_LEN];
	uint16_t resp_len;
	uint8_t key_before[ZLRS_KEY_LEN];
	uint32_t nonce_r;

	printf("link: session replay\n");
	sim_init(&sim);
	pair_and_connect(&sim);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* Record the first session's SYNC and session response */
	CHECK(sim.sync_len > 0U);
	CHECK(sim.sess_resp_len > 0U);
	sync_len = sim.sync_len;
	resp_len = sim.sess_resp_len;
	memcpy(sync, sim.sync, sync_len);
	memcpy(resp, sim.sess_resp, resp_len);

	/* Drop both ends back to SEARCHING: exactly the replay window */
	sim.drop_down = 500;
	sim.drop_up = 500;
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_SEARCHING &&
			  sim.rx.ctl.state == ZLRS_STATE_SEARCHING,
		  2000);
	sim.drop_down = 0;
	sim.drop_up = 0;
	CHECK(!sim.rx.ctl.session_valid);

	/* Replay the old SYNC to the RX: it may arm a candidate, never adopt it */
	nonce_r = sim.rx.ctl.sess_nonce_r;
	sim_inject(&sim, &sim.rx.ctl, sync, sync_len);
	CHECK(!sim.rx.ctl.session_valid);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(sim.rx.ctl.sess_nonce_r != nonce_r); /* a fresh nonce_r, not the old session's */
	CHECK(sim.rx.ctl.session_pending);

	/* Replay the old SESS_RESP to the TX: the echoed nonce_t is stale, so it must be ignored */
	memcpy(key_before, sim.tx.ctl.session_key_out, ZLRS_KEY_LEN);
	sim_inject(&sim, &sim.tx.ctl, resp, resp_len);
	CHECK(memcmp(sim.tx.ctl.session_key_out, key_before, ZLRS_KEY_LEN) == 0);

	/* When the real TX returns the link comes up as usual */
	sim.tx.ctl.last_sync_ms = 0U; /* do not wait a whole sync period */
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim.rx.ctl.state == ZLRS_STATE_CONNECTED,
		  4000);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.session_valid);
	CHECK(memcmp(sim.tx.ctl.session_key_out, sim.rx.ctl.session_key_in, ZLRS_KEY_LEN) == 0);
}

static void test_control_payloads(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t tlm[8];

	printf("link: rc / tlm\n");
	sim_init(&sim);
	pair_and_connect(&sim);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	for (uint8_t i = 0; i < sizeof(rc); i++) {
		rc[i] = (uint8_t)(i * 3U + 1U);
	}
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	sim_run(&sim, 2U);
	CHECK(sim.rx.rc_count > 0U);
	CHECK(sim.rx.rc_len == sizeof(rc));
	CHECK(memcmp(sim.rx.rc, rc, sizeof(rc)) == 0);

	for (uint8_t i = 0; i < sizeof(tlm); i++) {
		tlm[i] = (uint8_t)(0xA0U + i);
	}
	zlrs_link_set_tlm(&sim.rx.ctl, tlm, sizeof(tlm));
	sim_run(&sim, 2U);
	CHECK(sim.tx.tlm_count > 0U);
	CHECK(sim.tx.tlm_len == sizeof(tlm));
	CHECK(memcmp(sim.tx.tlm, tlm, sizeof(tlm)) == 0);
}

static void test_data_passthrough(void)
{
	sim_t sim;
	uint8_t payload[400];
	uint8_t reverse[300];

	printf("link: data passthrough\n");
	sim_init(&sim);
	pair_and_connect(&sim);
	CHECK(sim.tx.data.state == ZLRS_STATE_CONNECTED);

	for (uint16_t i = 0; i < sizeof(payload); i++) {
		payload[i] = (uint8_t)(i * 7U + 5U);
	}
	CHECK(zlrs_link_write(&sim.tx.data, payload, sizeof(payload)) == sizeof(payload));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(payload), 2000);
	CHECK(sim.rx.stream_len == sizeof(payload));
	CHECK(memcmp(sim.rx.stream, payload, sizeof(payload)) == 0);

	for (uint16_t i = 0; i < sizeof(reverse); i++) {
		reverse[i] = (uint8_t)(255U - (i % 251U));
	}
	CHECK(zlrs_link_write(&sim.rx.data, reverse, sizeof(reverse)) == sizeof(reverse));
	SIM_UNTIL(&sim, sim.tx.stream_len >= sizeof(reverse), 2000);
	CHECK(sim.tx.stream_len == sizeof(reverse));
	CHECK(memcmp(sim.tx.stream, reverse, sizeof(reverse)) == 0);
}

/*
 * A write larger than the receiver's reassembly buffer must still arrive whole. The sender marks
 * "more" while it has bytes left, so one long write used to become one run the peer could not
 * hold: the reassembler dropped it (correctly) and the bytes were simply gone.
 */
static void test_data_long_write(void)
{
	sim_t sim;
	static uint8_t payload[1024];

	printf("link: data write longer than the reassembly buffer\n");
	sim_init(&sim);
	pair_and_connect(&sim);

	for (uint16_t i = 0; i < sizeof(payload); i++) {
		payload[i] = (uint8_t)(i * 3U + 11U);
	}
	CHECK(zlrs_link_write(&sim.tx.data, payload, sizeof(payload)) == sizeof(payload));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(payload), 6000);
	CHECK(sim.rx.stream_len == sizeof(payload));
	CHECK(memcmp(sim.rx.stream, payload, sizeof(payload)) == 0);
	CHECK(sim.rx.data.stats.rx_frag_drop == 0U);
}
static void test_rx_rejects(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t bad[ZLRS_FRAME_MAX_LEN];
	uint8_t tiny[2] = {0x00, 0x01};
	uint32_t dup_before;
	uint32_t tag_before;
	uint32_t frame_before;
	uint32_t count_before;

	printf("link: bad frames\n");
	sim_init(&sim);
	pair_and_connect(&sim);

	memset(rc, 0x5a, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	sim_run(&sim, 3U);
	CHECK(sim.rx.rc_count > 0U);
	CHECK(sim.down_len > 0U);

	/* Replay the same frame: the sequence number is not new, so it counts as rx_dup */
	count_before = sim.rx.rc_count;
	dup_before = sim.rx.ctl.stats.rx_dup;
	sim_inject(&sim, &sim.rx.ctl, sim.down, sim.down_len);
	CHECK(sim.rx.ctl.stats.rx_dup == dup_before + 1U);
	CHECK(sim.rx.rc_count == count_before);

	/* Change the tag: verification fails */
	tag_before = sim.rx.ctl.stats.rx_bad_tag;
	memcpy(bad, sim.down, sim.down_len);
	bad[sim.down_len - 1U] ^= 0xFFU;
	sim_inject(&sim, &sim.rx.ctl, bad, sim.down_len);
	CHECK(sim.rx.ctl.stats.rx_bad_tag == tag_before + 1U);

	/* Too short: rejected outright */
	frame_before = sim.rx.ctl.stats.rx_bad_frame;
	sim_inject(&sim, &sim.rx.ctl, tiny, sizeof(tiny));
	CHECK(sim.rx.ctl.stats.rx_bad_frame == frame_before + 1U);
}

static void test_frag_drop(void)
{
	sim_t sim;
	uint8_t frame[ZLRS_FRAME_MAX_LEN];
	uint8_t chunk[6] = {1U, 2U, 3U, 4U, 5U, 6U};
	const uint8_t *key;
	uint32_t drop_before;
	uint16_t seq;
	uint16_t len;

	printf("link: fragmentation recovery\n");
	sim_init(&sim);
	pair_and_connect(&sim);
	CHECK(sim.rx.data.state == ZLRS_STATE_CONNECTED);

	key = sim.rx.data.session_key_in; /* the RX verifies the TX's frames with in */
	seq = (uint16_t)(sim.rx.data.last_rx_seq + 1U);

	/* Gap: drop the whole run */
	drop_before = sim.rx.data.stats.rx_frag_drop;
	len = build_data_frame(frame, ZLRS_PKT_DATA_UP, seq, ZLRS_FLAG_MORE, chunk, sizeof(chunk),
			       key);
	CHECK(len > 0U);
	sim_inject(&sim, &sim.rx.data, frame, len);
	CHECK(sim.rx.data.reasm.len == sizeof(chunk));

	seq = (uint16_t)(seq + 2U);
	len = build_data_frame(frame, ZLRS_PKT_DATA_UP, seq, ZLRS_FLAG_MORE, chunk, sizeof(chunk),
			       key);
	sim_inject(&sim, &sim.rx.data, frame, len);
	CHECK(sim.rx.data.stats.rx_frag_drop == drop_before + 1U);
	CHECK(sim.rx.data.reasm.len == 0U);
	CHECK(sim.rx.stream_len == 0U);

	/* Timeout: the leftovers go and a new run starts */
	seq = (uint16_t)(seq + 1U);
	len = build_data_frame(frame, ZLRS_PKT_DATA_UP, seq, ZLRS_FLAG_MORE, chunk, sizeof(chunk),
			       key);
	sim_inject(&sim, &sim.rx.data, frame, len);
	CHECK(sim.rx.data.reasm.len == sizeof(chunk));

	sim.now_ms += ZLRS_FRAG_TIMEOUT_MS + 5U;
	seq = (uint16_t)(seq + 1U);
	len = build_data_frame(frame, ZLRS_PKT_DATA_UP, seq, ZLRS_FLAG_MORE, chunk, sizeof(chunk),
			       key);
	sim_inject(&sim, &sim.rx.data, frame, len);
	CHECK(sim.rx.data.reasm.len == sizeof(chunk)); /* only the new fragment is left */
	CHECK(sim.rx.data.stats.rx_frag_drop == drop_before + 1U);
}

static void test_recovery(void)
{
	sim_t sim;

	printf("link: loss and recovery\n");
	sim_init(&sim);
	pair_and_connect(&sim);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* Cut both directions: the control link falls back to SEARCHING on its timeout */
	sim.drop_down = 100000;
	sim.drop_up = 100000;
	sim_run(&sim, 100U); /* 600 ms */

	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(sim.rx.ctl_state == ZLRS_STATE_SEARCHING); /* on_state fired, which drives failsafe */
	CHECK(sim.tx.ctl.paired && sim.rx.ctl.paired);   /* the pairing record survives */

	/* Restore delivery: a new SYNC reconnects, no re-pairing needed */
	sim.drop_down = 0;
	sim.drop_up = 0;
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim.rx.ctl.state == ZLRS_STATE_CONNECTED,
		  2000);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.tx.ctl.stats.tx_frames > 0U);
}

static void test_hop_lockstep(void)
{
	sim_t sim;
	uint16_t ctl_before;
	uint16_t data_before;

	printf("link: hopping lockstep\n");
	sim_init(&sim);
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);

	ctl_before = sim.tx.ctl.fhss.index;
	data_before = sim.tx.data.fhss.index;
	sim_run(&sim, 50U);

	CHECK(sim.tx.ctl.fhss.index == sim.rx.ctl.fhss.index);
	CHECK(sim.tx.data.fhss.index == sim.rx.data.fhss.index);
	CHECK(sim.tx.ctl.fhss.index != ctl_before);
	CHECK(sim.tx.data.fhss.index != data_before);
	CHECK(zlrs_fhss_current_hz(&sim.tx.ctl.fhss) == zlrs_fhss_current_hz(&sim.rx.ctl.fhss));
	CHECK(zlrs_fhss_current_hz(&sim.tx.data.fhss) == zlrs_fhss_current_hz(&sim.rx.data.fhss));
}

static void test_unconfigured(void)
{
	node_t node;
	static const uint8_t dev_id[ZLRS_DEV_ID_LEN] = {1U, 2U, 3U, 4U};

	printf("link: unconfigured\n");

	node_init(&node, ZLRS_ROLE_TX, NULL, dev_id, 0x33333333U);
	CHECK(node.ctl.state == ZLRS_STATE_UNCONFIGURED);
	CHECK(zlrs_link_slot(&node.ctl, ZLRS_SLOT_CTL_DOWN, 0U) == NULL);
	CHECK(zlrs_link_slot(&node.ctl, ZLRS_SLOT_CTL_UP, 0U) == NULL);
	CHECK(zlrs_link_slot(&node.data, ZLRS_SLOT_DATA_A, 0U) == NULL);
	CHECK(zlrs_link_slot(&node.data, ZLRS_SLOT_DATA_B, 0U) == NULL);

	zlrs_link_press(&node.ctl, 0U);
	CHECK(node.ctl.state == ZLRS_STATE_UNCONFIGURED);
}

static void test_sync_rate(void)
{
	sim_t sim;
	uint32_t before;
	uint32_t sent;

	printf("link: sync rate\n");
	sim_init(&sim);
	zlrs_link_press(&sim.tx.ctl, sim.now_ms);
	zlrs_link_press(&sim.rx.ctl, sim.now_ms);
	SIM_UNTIL(&sim, sim.tx.ctl.state == ZLRS_STATE_SEARCHING, 2000);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_SEARCHING);

	/* The peer hears nothing: at this point a SYNC is the TX's only outgoing frame */
	sim.drop_down = 100000;
	before = sim.tx.ctl.stats.tx_frames;
	sim_run(&sim, 3U * SIM_FRAMES_PER_SEC);
	sent = sim.tx.ctl.stats.tx_frames - before;
	CHECK(sent >= 3U && sent <= 5U);
}

/* Uplink LQ: the sequence gap gives the expected count, so drops must lower the percentage */
static void test_link_quality(void)
{
	sim_t sim;

	printf("link: uplink lq\n");

	sim_init(&sim);
	pair_and_connect(&sim);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* More than a window without drops: LQ has to be full */
	sim_run(&sim, 200U);
	CHECK(sim.rx.ctl.stats.lq_percent == 100U);

	/*
	 * Drop one downlink delivery in eight (twenty in a row would trigger a reconnect, another case),
	 * which over 160 superframes fills an LQ window (128 frames) and settles it
	 */
	for (uint8_t i = 0U; i < 20U; i++) {
		sim.drop_down = 1;
		sim_run(&sim, 8U);
	}
	CHECK(sim.rx.ctl.stats.lq_percent < 100U);
	CHECK(sim.rx.ctl.stats.lq_percent > 50U);

	/* No more drops: after a full window LQ returns to full */
	sim_run(&sim, 200U);
	CHECK(sim.rx.ctl.stats.lq_percent == 100U);
}

/* Passthrough backpressure: bytes dropped on a full air queue have to be counted */
static void test_backpressure(void)
{
	sim_t sim;
	uint8_t chunk[128];
	uint16_t accepted = 0U;

	printf("link: backpressure\n");

	sim_init(&sim);
	memset(chunk, 0x5AU, sizeof(chunk));

	/* Fill the air-side queue exactly, and everything beyond must count as dropped */
	for (uint16_t i = 0U; i < ZLRS_LINK_TX_QUEUE_LEN / sizeof(chunk); i++) {
		accepted =
			(uint16_t)(accepted + zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)));
	}
	CHECK(accepted == ZLRS_LINK_TX_QUEUE_LEN);
	CHECK(sim.tx.data.stats.data_drop_bytes == 0U);

	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == 0U);
	CHECK(sim.tx.data.stats.data_drop_bytes == sizeof(chunk));

	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == 0U);
	CHECK(sim.tx.data.stats.data_drop_bytes == 2U * sizeof(chunk));
}

/* ARQ: drops are filled in by retransmits and the receiver dedups on the data sequence */
static void test_arq_retransmit(void)
{
	sim_t sim;
	uint8_t want[512];

	printf("link: arq retransmit\n");

	sim_init(&sim);
	sim.tx.data.cfg.arq = 1U;
	sim.rx.data.cfg.arq = 1U;
	pair_and_connect(&sim);
	CHECK(sim.tx.data.state == ZLRS_STATE_CONNECTED);

	for (uint16_t i = 0U; i < sizeof(want); i++) {
		want[i] = (uint8_t)(i * 7U + 3U);
	}
	CHECK(zlrs_link_write(&sim.tx.data, want, sizeof(want)) == sizeof(want));

	/* Drop one data frame in three: retransmits have to deliver all 512 bytes */
	for (uint8_t round = 0U; round < 10U; round++) {
		sim.drop_data_down = 1;
		sim_run(&sim, 8U);
	}
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(want), 4000);

	CHECK(sim.rx.stream_len == sizeof(want));
	CHECK(memcmp(sim.rx.stream, want, sizeof(want)) == 0);
	CHECK(sim.tx.data.stats.tx_retries > 0U);
	CHECK(sim.tx.data.stats.tx_arq_drop == 0U);
	CHECK(sim.tx.data.stats.data_drop_bytes == 0U);
	CHECK(sim.rx.data.stats.rx_out_of_order > 0U);
}

/*
 * ARQ: a run survives a retransmit that lands past the reassembly timeout. On a lossy slot the
 * head frame of a run is repeated for tens of milliseconds, and a receiver that expires the half
 * run in the meantime throws away bytes its ACK already covered — the peer is never told, so the
 * passthrough loses a chunk in the middle of every message.
 */
static void test_arq_slow_run(void)
{
	sim_t sim;
	uint8_t want[512];

	printf("link: arq slow run\n");

	sim_init(&sim);
	sim.tx.data.cfg.arq = 1U;
	sim.rx.data.cfg.arq = 1U;
	pair_and_connect(&sim);
	CHECK(sim.tx.data.state == ZLRS_STATE_CONNECTED);

	for (uint16_t i = 0U; i < sizeof(want); i++) {
		want[i] = (uint8_t)(i * 5U + 1U);
	}
	CHECK(zlrs_link_write(&sim.tx.data, want, sizeof(want)) == sizeof(want));

	/* The first frame of the run lands, the rest of the air goes quiet for longer than the
	 * reassembly timeout before the retransmit gets through */
	sim_run(&sim, 1U);
	sim.drop_data_down = 30;
	sim_run(&sim, 30U);
	sim.drop_data_down = 0;

	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(want), 4000);

	CHECK(sim.rx.stream_len == sizeof(want));
	CHECK(memcmp(sim.rx.stream, want, sizeof(want)) == 0);
	CHECK(sim.tx.data.stats.tx_retries > 0U);
	CHECK(sim.tx.data.stats.data_drop_bytes == 0U);
}

/* ARQ: a duplicated data frame is delivered exactly once */
static void test_arq_dedup(void)
{
	sim_t sim;
	uint8_t want[96];

	printf("link: arq dedup\n");

	sim_init(&sim);
	sim.tx.data.cfg.arq = 1U;
	sim.rx.data.cfg.arq = 1U;
	pair_and_connect(&sim);

	for (uint16_t i = 0U; i < sizeof(want); i++) {
		want[i] = (uint8_t)(0xC0U + i);
	}
	CHECK(zlrs_link_write(&sim.tx.data, want, sizeof(want)) == sizeof(want));

	sim.replay_data_next = true;
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(want), 2000);
	sim_run(&sim, 20U);

	CHECK(sim.rx.stream_len == sizeof(want));
	CHECK(memcmp(sim.rx.stream, want, sizeof(want)) == 0);
	CHECK(sim.rx.data.stats.rx_dup > 0U);
}

/* ARQ: exhausted retries drop that run and count it, without blocking later data */
static void test_arq_giveup(void)
{
	sim_t sim;
	uint8_t chunk[200];

	printf("link: arq give-up\n");

	sim_init(&sim);
	sim.tx.data.cfg.arq = 1U;
	sim.rx.data.cfg.arq = 1U;
	pair_and_connect(&sim);

	memset(chunk, 0x33U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));

	/* Downlink dead: the run is restarted (counted), and nothing is thrown away */
	sim.drop_data_down = 100000;
	sim_run(&sim, 400U);

	CHECK(sim.tx.data.stats.tx_retries > 0U);
	CHECK(sim.tx.data.stats.tx_arq_drop >= 1U);
	CHECK(sim.tx.data.stats.data_drop_bytes == 0U);
	CHECK(sim.tx.data.queue_count == sizeof(chunk));

	/* Link restored: after the resync handshake every byte still arrives, in order */
	sim.drop_data_down = 0;
	SIM_UNTIL(&sim, sim.tx.data.queue_count == 0U, 4000);
	CHECK(sim.tx.data.queue_count == 0U);
	CHECK(sim.tx.data.stats.data_drop_bytes == 0U);
	CHECK(sim.rx.stream_len == sizeof(chunk));
	for (uint16_t i = 0U; i < sim.rx.stream_len; i++) {
		CHECK(sim.rx.stream[i] == 0x33U);
	}
}

/* Both directions must recover even when neither has received any acknowledgement. */
static void test_arq_bidirectional_recovery(void)
{
	sim_t sim;
	uint8_t payload[32];

	printf("link: arq simultaneous recovery\n");
	sim_init(&sim);
	sim.tx.data.cfg.arq = 1U;
	sim.rx.data.cfg.arq = 1U;
	pair_and_connect(&sim);
	memset(payload, 0x5a, sizeof(payload));
	CHECK(zlrs_link_write(&sim.tx.data, payload, sizeof(payload)) == sizeof(payload));
	CHECK(zlrs_link_write(&sim.rx.data, payload, sizeof(payload)) == sizeof(payload));
	sim.drop_down = 100000;
	sim.drop_up = 100000;
	sim_run(&sim, 500U);
	CHECK(sim.tx.data.arq_resync && sim.rx.data.arq_resync);
	sim.drop_down = 0;
	sim.drop_up = 0;
	sim_run(&sim, 2000U);
	CHECK(sim.tx.data.queue_count == 0U && sim.rx.data.queue_count == 0U);
	CHECK(!sim.tx.data.arq_resync && !sim.rx.data.arq_resync);
	CHECK(sim.rx.stream_len == sizeof(payload) && sim.tx.stream_len == sizeof(payload));
	CHECK(memcmp(sim.rx.stream, payload, sizeof(payload)) == 0);
	CHECK(memcmp(sim.tx.stream, payload, sizeof(payload)) == 0);
}

/* Recovery cannot forget a delivered prefix or discard a partial message already ACKed. */
static void test_arq_recovery_preserves_progress(void)
{
	for (unsigned int partial = 0U; partial < 2U; partial++) {
		sim_t sim;
		uint8_t payload[256];

		printf("link: arq recovery preserves %s\n", partial ? "partial run" : "prefix");
		sim_init(&sim);
		sim.tx.data.cfg.arq = 1U;
		sim.rx.data.cfg.arq = 1U;
		pair_and_connect(&sim);
		for (size_t i = 0U; i < sizeof(payload); i++) {
			payload[i] = (uint8_t)i;
		}
		if (partial) {
			CHECK(zlrs_link_write(&sim.tx.data, payload, sizeof(payload)) ==
			      sizeof(payload));
			sim_run(&sim, 1U);
			CHECK(sim.rx.data.reasm.active);
			CHECK(sim.rx.stream_len == 0U);
		} else {
			CHECK(zlrs_link_write(&sim.tx.data, payload, 128U) == 128U);
			SIM_UNTIL(&sim, sim.tx.data.queue_count == 0U, 2000);
			CHECK(sim.rx.stream_len == 128U);
			CHECK(zlrs_link_write(&sim.tx.data, payload + 128U, 128U) == 128U);
		}
		CHECK(sim.tx.data.arq_seq == 1U);
		sim.drop_data_down = 100000;
		sim_run(&sim, 500U);
		CHECK(sim.tx.data.stats.tx_arq_drop > 0U);
		sim.drop_data_down = 0;
		sim_run(&sim, 2000U);
		CHECK(sim.rx.stream_len == sizeof(payload));
		CHECK(memcmp(sim.rx.stream, payload, sizeof(payload)) == 0);
		CHECK(sim.tx.data.queue_count == 0U);
	}
}

/* Lost ACKs must not make recovery deliver an already accepted message twice. */
static void test_arq_recovery_lost_ack(void)
{
	sim_t sim;
	uint8_t payload[32];

	printf("link: arq recovery after lost ACK\n");
	sim_init(&sim);
	sim.tx.data.cfg.arq = 1U;
	sim.rx.data.cfg.arq = 1U;
	pair_and_connect(&sim);
	memset(payload, 0x3c, sizeof(payload));
	sim.drop_up = 100000;
	CHECK(zlrs_link_write(&sim.tx.data, payload, sizeof(payload)) == sizeof(payload));
	sim_run(&sim, 500U);
	CHECK(sim.tx.data.arq_resync);
	CHECK(sim.rx.stream_len == sizeof(payload));
	sim.drop_up = 0;
	sim_run(&sim, 2000U);
	CHECK(sim.tx.data.queue_count == 0U);
	CHECK(sim.rx.stream_len == sizeof(payload));
	CHECK(memcmp(sim.rx.stream, payload, sizeof(payload)) == 0);
}

/* Rate adaptation: windows, hold time and hysteresis, all three or nothing moves */
static void test_rate_adapt(void)
{
	static const zlrs_rate_tier_t tiers[] = {
		{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 2600000U}, .min_lq_to_stay = 85U},
		{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 1300000U}, .min_lq_to_stay = 60U},
		{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 650000U}, .min_lq_to_stay = 35U},
		{.mod = {.modulation = ZLRS_MOD_FLRC, .rate_bps = 325000U}, .min_lq_to_stay = 0U},
	};
	zlrs_rate_ctl_t ctl;
	uint8_t next = 0xFFU;
	uint32_t now = 1000U;

	printf("link: rate adaptation\n");

	zlrs_rate_init(&ctl, tiers, 4U, 1U, now); /* start at 1.3 Mbps */
	CHECK(zlrs_rate_current_bps(&ctl) == 1300000U);

	/* During the hold time no windows count and nothing moves */
	CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(zlrs_rate_current_bps(&ctl) == 1300000U);

	/* Hold time over: three bad windows in a row drop one tier (slower) */
	now += 5000U;
	CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(next == 2U);
	CHECK(zlrs_rate_current_bps(&ctl) == 650000U);

	/* After a change nothing moves during the hold time */
	now += 100U;
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	}
	CHECK(zlrs_rate_current_bps(&ctl) == 650000U);

	/* Hold time over: three more bad windows drop again */
	now += 5000U;
	CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(next == 3U);
	CHECK(zlrs_rate_current_bps(&ctl) == 325000U);

	/* Slowest tier: no further drop, however bad it gets */
	now += 5000U;
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(!zlrs_rate_update(&ctl, 0U, now, &next));
	}
	CHECK(zlrs_rate_current_bps(&ctl) == 325000U);

	/* Raising needs the hysteresis margin: 0.65M wants 35+10=45, so 40% neither raises nor drops */
	now += 5000U;
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(!zlrs_rate_update(&ctl, 40U, now, &next));
	}
	CHECK(zlrs_rate_current_bps(&ctl) == 325000U);

	/* 75% >= the next tier's 60 plus hysteresis 10: three windows in a row raise it */
	now += 5000U;
	CHECK(!zlrs_rate_update(&ctl, 75U, now, &next));
	CHECK(!zlrs_rate_update(&ctl, 75U, now, &next));
	CHECK(zlrs_rate_update(&ctl, 75U, now, &next));
	CHECK(next == 2U);
	CHECK(zlrs_rate_current_bps(&ctl) == 650000U);
}

/* The TX configures failsafe values and ships them while pairing: the RX stores and emits them */
static void test_failsafe_push(void)
{
	sim_t sim;
	uint8_t want[ZLRS_RC_PAYLOAD_LEN];

	printf("link: failsafe push\n");

	sim_init(&sim);
	for (uint8_t i = 0U; i < sizeof(want); i++) {
		want[i] = (uint8_t)(0x40U + i);
	}
	zlrs_link_set_failsafe(&sim.tx.ctl, want, sizeof(want));

	pair_and_connect(&sim);

	/* The link layer stored them */
	CHECK(sim.rx.ctl.failsafe_len == sizeof(want));
	CHECK(memcmp(sim.rx.ctl.failsafe, want, sizeof(want)) == 0);
	/* The application callback fired too (the RX writes NVS and uses them) */
	CHECK(sim.rx.failsafe_count == 1U);
	CHECK(sim.rx.failsafe_len == sizeof(want));
	CHECK(memcmp(sim.rx.failsafe, want, sizeof(want)) == 0);

	/* Values with the wrong length are refused */
	zlrs_link_set_failsafe(&sim.tx.ctl, want, sizeof(want) - 1U);
	CHECK(sim.tx.ctl.failsafe_len == sizeof(want));

	/* With nothing set on the TX, the RX keeps its own values instead of being zeroed */
	sim_init(&sim);
	pair_and_connect(&sim);
	CHECK(sim.rx.ctl.failsafe_len == 0U);
	CHECK(sim.rx.failsafe_count == 0U);
}

/*
 * The data link is best-effort: it sends nothing while idle and has no peer-loss check
 * (doc/09-protocol.md 3.7), so even a long idle period must not rebuild the session repeatedly:
 * otherwise the first data after idle would hit a handshake and waste a sync period.
 */
static void test_data_idle(void)
{
	sim_t sim;
	uint8_t chunk[8];
	uint32_t nonce_before;

	printf("link: data link idle\n");
	sim_init(&sim);
	pair_and_connect(&sim);
	CHECK(sim.tx.data.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.data.state == ZLRS_STATE_CONNECTED);

	nonce_before = sim.tx.data.sess_nonce_t;
	CHECK(zlrs_link_slot(&sim.tx.data, ZLRS_SLOT_DATA_A, sim.now_ms) == NULL);
	CHECK(zlrs_link_slot(&sim.rx.data, ZLRS_SLOT_DATA_A, sim.now_ms) != NULL);
	zlrs_link_complete(&sim.rx.data, ZLRS_RADIO_OK, 0U, 0, 0, sim.now_ms);
	CHECK(zlrs_link_slot(&sim.rx.data, ZLRS_SLOT_DATA_B, sim.now_ms) == NULL);
	CHECK(zlrs_link_slot(&sim.tx.data, ZLRS_SLOT_DATA_B, sim.now_ms) != NULL);
	zlrs_link_complete(&sim.tx.data, ZLRS_RADIO_OK, 0U, 0, 0, sim.now_ms);
	sim_run(&sim, 3000U); /* about 18 s of idling */
	CHECK(sim.tx.data.sess_nonce_t == nonce_before);
	CHECK(sim.tx.data.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.data.state == ZLRS_STATE_CONNECTED);

	/* Data still flows after the idle period */
	memset(chunk, 0x44U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/* Cadence thinning at a high control rate: telemetry every 4 frames, data every 5, still in step */
static void test_cadence(void)
{
	sim_t sim;
	uint8_t chunk[32];
	uint32_t tlm_before;
	uint32_t tlm_after;

	printf("link: cadence (sparse tlm / data)\n");

	sim_init(&sim);
	/* Both ends need the same cadence: same firmware, same settings */
	sim.tx.ctl.cfg.tlm_every_n = 4U;
	sim.rx.ctl.cfg.tlm_every_n = 4U;
	sim.tx.data.cfg.data_every_n = 5U;
	sim.rx.data.cfg.data_every_n = 5U;

	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* Give the RX a telemetry payload: an empty one is a keepalive and does not call on_tlm */
	{
		uint8_t tlm[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

		zlrs_link_set_tlm(&sim.rx.ctl, tlm, sizeof(tlm));
	}

	/* Run 100 superframes: telemetry should appear about 25 times, not 100 */
	tlm_before = sim.tx.tlm_count;
	sim_run(&sim, 100U);
	tlm_after = sim.tx.tlm_count - tlm_before;
	CHECK(tlm_after >= 20U && tlm_after <= 30U);

	/* Hopping stays in lockstep */
	CHECK(sim.tx.ctl.fhss.index == sim.rx.ctl.fhss.index);
	CHECK(sim.tx.data.fhss.index == sim.rx.data.fhss.index);

	/* The data window thinned out and still carries traffic */
	memset(chunk, 0x5AU, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/*
 * Bench switch zero: P0, frames carry no tag. Confirm timing and frame format first and set the
 * whole key path aside (anyone can inject, so bench only).
 */
static void test_isolation_no_tag(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t chunk[16];

	printf("link: isolation without tag (P0)\n");

	sim_init(&sim);
	sim.tx.ctl.cfg.profile = ZLRS_PROFILE_P0;
	sim.rx.ctl.cfg.profile = ZLRS_PROFILE_P0;
	sim.tx.data.cfg.profile = ZLRS_PROFILE_P0;
	sim.rx.data.cfg.profile = ZLRS_PROFILE_P0;
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	memset(rc, 0x61U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	sim_run(&sim, 4U);
	CHECK(sim.rx.rc_count > 0U);
	CHECK(memcmp(sim.rx.rc, rc, sizeof(rc)) == 0);
	CHECK(sim.rx.ctl.stats.rx_bad_tag == 0U);

	/* A frame on air is header plus payload, no tag */
	sim_slot(&sim, ZLRS_SLOT_CTL_DOWN);
	CHECK(sim.down_len == (uint16_t)(ZLRS_FRAME_HDR_LEN + sizeof(rc)));

	/* Passthrough keeps working */
	memset(chunk, 0x62U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/*
 * Bench switch one: hopping off. Every frame stays on the sync channel (entry 0 of the hop
 * table), so timing and the hop table can be read separately: it may only change frequencies.
 */
static void test_isolation_fixed_channel(void)
{
	sim_features_t features = sim_features_default;
	const zlrs_fhss_domain_t *domain = &zlrs_fhss_domains[ZLRS_DOMAIN_FCC915];
	const uint32_t sync_hz =
		domain->start_hz + (uint32_t)(domain->channel_count / 2U) * domain->step_hz;
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t chunk[16];

	printf("link: isolation without hopping\n");

	features.hopping = 0U;
	sim_init_with(&sim, &features);
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	memset(rc, 0x31U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	sim_run(&sim, 300U);

	/* The index sits at 0 (= the sync channel) on both ends */
	CHECK(sim.tx.ctl.fhss.index == 0U);
	CHECK(sim.rx.ctl.fhss.index == 0U);
	CHECK(zlrs_fhss_current_hz(&sim.tx.ctl.fhss) == sync_hz);
	CHECK(zlrs_fhss_current_hz(&sim.rx.ctl.fhss) == sync_hz);

	/* RC and passthrough keep working */
	CHECK(sim.rx.rc_count > 0U);
	CHECK(memcmp(sim.rx.rc, rc, sizeof(rc)) == 0);
	memset(chunk, 0x32U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
	CHECK(sim.rx.ctl.stats.rx_bad_tag == 0U);
}

/*
 * Bench switch two: handshake off. The pairing-level link_auth is the session key, the RX
 * counts a SYNC as established and keeps reporting with SESS_RESP until the peer speaks.
 * Losing a few report frames must still connect: that is the only new timing here.
 */
static void test_isolation_no_handshake(void)
{
	sim_features_t features = sim_features_default;
	sim_t sim;
	uint8_t chunk[16];
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];

	printf("link: isolation without session handshake\n");

	features.handshake = 0U;
	sim_init_with(&sim, &features);

	/* Drop two report frames: the RX keeps reporting until the TX hears it */
	sim.drop_up = 2;
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.tx.data.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.data.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.sess_resp_len > 0U); /* SESS_RESP is used, not SESS_CFM */

	/* The session key is the pairing key: no per-session nonce derivation */
	CHECK(memcmp(sim.tx.ctl.session_key_out, sim.tx.ctl.link_auth_out, ZLRS_KEY_LEN) == 0);
	CHECK(memcmp(sim.tx.ctl.session_key_in, sim.rx.ctl.link_auth_out, ZLRS_KEY_LEN) == 0);
	CHECK(!sim.rx.ctl.session_pending);
	CHECK(!sim.tx.ctl.sess_cfm_due);

	/* RC and passthrough keep working */
	memset(rc, 0x41U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	sim_run(&sim, 10U);
	CHECK(sim.rx.rc_count > 0U);
	CHECK(memcmp(sim.rx.rc, rc, sizeof(rc)) == 0);
	memset(chunk, 0x42U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
	CHECK(sim.rx.ctl.stats.rx_bad_tag == 0U);
	CHECK(sim.tx.ctl.stats.rx_bad_tag == 0U);
}

/*
 * Bench switch three: telemetry off. The control link degrades to downlink RC only: the TX opens
 * no telemetry window (the app drops peer loss with it, doc/12-debug.md 3) and the RX sends none.
 */
static void test_isolation_no_telemetry(void)
{
	sim_features_t features = sim_features_default;
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];

	printf("link: isolation without telemetry\n");

	features.telemetry = 0U;
	sim_init_with(&sim, &features);
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* The app turns the TX peer-loss check off with telemetry: there is nothing to hear */
	sim.tx.ctl.cfg.peer_timeout_ms = 0U;
	memset(rc, 0x51U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	sim_run(&sim, 500U);

	CHECK(sim.tx.tlm_count == 0U);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.rc_count > 0U);
	CHECK(memcmp(sim.rx.rc, rc, sizeof(rc)) == 0);
}

/*
 * Periodic SYNC while connected (doc/09-protocol.md 5.1): crystal drift slowly pulls the hop
 * indices apart and this frame pulls the RX back. The case shifts the index by 4 to fake drift.
 */
static void test_hop_resync(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t chunk[8];
	uint32_t resync_before;

	printf("link: hop resync (connected-state sync)\n");

	sim_init(&sim);
	sim.tx.ctl.cfg.resync_every_n = 25U;
	sim.rx.ctl.cfg.resync_every_n = 25U;
	sim.tx.data.cfg.resync_every_n = 50U;
	sim.rx.data.cfg.resync_every_n = 50U;
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	memset(rc, 0x81U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	sim_run(&sim, 30U);
	CHECK(sim.rx.rc_count > 0U);
	CHECK(sim.rx.ctl.fhss.index == sim.tx.ctl.fhss.index);

	/* Fake drift: the RX index is off by 4, so the next frames miss the frequency */
	sim.rx.ctl.fhss.index =
		(uint16_t)((sim.rx.ctl.fhss.index + 4U) % sim.rx.ctl.fhss.sequence_count);
	CHECK(sim.rx.ctl.fhss.index != sim.tx.ctl.fhss.index);

	resync_before = sim.rx.ctl.stats.hop_resync;
	SIM_UNTIL(&sim, sim.rx.ctl.fhss.index == sim.tx.ctl.fhss.index, 200);
	CHECK(sim.rx.ctl.fhss.index == sim.tx.ctl.fhss.index);
	CHECK(sim.rx.ctl.stats.hop_resync > resync_before);
	CHECK(sim.rx.ctl.stats.rx_syncs > 0U);

	/* Once the index is pulled back, traffic flows again */
	memset(chunk, 0x82U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
	CHECK(sim.rx.ctl.fhss.index == sim.tx.ctl.fhss.index);
	CHECK(sim.rx.data.fhss.index == sim.tx.data.fhss.index);
}

/*
 * Sub-band mix (doc/09-protocol.md 5.1): the TX advances it unilaterally, the RX adopts it while
 * handling a SYNC, and falls back to the default after missing new versions for too long.
 */
/*
 * Sub-band mixing end to end: both links carry two tables, and after the TX moves to 100% RC and
 * telemetry still get through, which is what the pure C side can prove: both ends pick one band.
 */
static void test_sub_mix_data_path(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];

	printf("link: sub-band mix data path\n");

	sim_init(&sim);
	sim.tx.ctl.cfg.second_domain = ZLRS_DOMAIN_ISM2400;
	sim.rx.ctl.cfg.second_domain = ZLRS_DOMAIN_ISM2400;
	sim.tx.ctl.cfg.resync_every_n = 5U;
	sim.rx.ctl.cfg.resync_every_n = 5U;
	sim.tx.data.cfg.second_domain = ZLRS_DOMAIN_ISM2400;
	sim.rx.data.cfg.second_domain = ZLRS_DOMAIN_ISM2400;
	sim.tx.data.cfg.resync_every_n = 5U;
	sim.rx.data.cfg.resync_every_n = 5U;
	pair_and_connect(&sim);

	/* Both tables are ready */
	CHECK(zlrs_fhss_sub_ready(&sim.tx.ctl.fhss));
	CHECK(zlrs_fhss_sub_ready(&sim.rx.ctl.fhss));

	/* TX moves to 100%: the whole control link falls back to the second table (2.4G) */
	zlrs_link_set_mix(&sim.tx.ctl, 3U);
	memset(rc, 0x91U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	SIM_UNTIL(&sim, sim.rx.ctl.sub_mix == 3U, 400);
	CHECK(sim.rx.ctl.sub_mix == 3U);

	/* Both ends sit on the same frequency and RC still arrives */
	CHECK(zlrs_fhss_sub_current_hz(&sim.tx.ctl.fhss) ==
	      zlrs_fhss_sub_current_hz(&sim.rx.ctl.fhss));
	sim_run(&sim, 40U);
	CHECK(sim.rx.rc_count > 0U);
}

static void test_sub_mix(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint32_t changes_before;

	printf("link: sub-band mix over SYNC\n");

	sim_init(&sim);
	sim.tx.ctl.cfg.resync_every_n = 5U;
	sim.rx.ctl.cfg.resync_every_n = 5U;
	sim.tx.data.cfg.resync_every_n = 5U;
	sim.rx.data.cfg.resync_every_n = 5U;
	pair_and_connect(&sim);

	CHECK(sim.tx.ctl.sub_mix == 0U);
	CHECK(sim.rx.ctl.sub_mix == 0U);

	/* TX advances to 50% and the RX adopts it from the next SYNC */
	zlrs_link_set_mix(&sim.tx.ctl, 2U);
	CHECK(sim.tx.ctl.sub_mix == 2U);
	changes_before = sim.rx.ctl.stats.mix_changes;
	memset(rc, 0x81U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	SIM_UNTIL(&sim, sim.rx.ctl.sub_mix == 2U, 400);
	CHECK(sim.rx.ctl.sub_mix == 2U);
	CHECK(sim.rx.ctl.stats.mix_changes > changes_before);

	/* On the RX the default is only the fallback target, not the current level */
	zlrs_link_set_mix(&sim.rx.ctl, 0U);
	CHECK(sim.rx.ctl.sub_mix == 2U);
	CHECK(sim.rx.ctl.sub_mix_default == 0U);

	/* Isolate mix fallback from the shorter peer-loss/re-handshake timers. */
	sim.tx.ctl.cfg.peer_timeout_ms = 0U;
	sim.rx.ctl.cfg.peer_timeout_ms = 0U;
	/* Cut the TX's SYNC (only one side changed): the RX falls back after 3 sync periods */
	sim.tx.ctl.cfg.resync_every_n = 0U;
	sim.tx.ctl.sub_mix = 3U;
	sim.rx.ctl.sub_mix_seen_ms = sim.now_ms;
	SIM_UNTIL(&sim, sim.rx.ctl.sub_mix == sim.rx.ctl.sub_mix_default, 4000);
	CHECK(sim.rx.ctl.sub_mix == 0U);
	CHECK(sim.rx.ctl.stats.mix_fallbacks > 0U);
}

/*
 * Automatic reconnect after a peer reboot (the fix for the limitation in doc/09-protocol.md 3.5):
 * the RX loses all link state and keeps only its NVS pairing record while the TX keeps running,
 * and the periodic SYNC makes it handshake again. Both links have to come back on their own.
 */
static void test_peer_restart(void)
{
	sim_t sim;
	uint8_t salt[ZLRS_PAIR_SALT_LEN];
	uint8_t rx_id[ZLRS_DEV_ID_LEN];
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t chunk[8];

	printf("link: peer restart reconnect\n");

	sim_init(&sim);
	sim.tx.ctl.cfg.resync_every_n = 20U;
	sim.rx.ctl.cfg.resync_every_n = 20U;
	sim.tx.data.cfg.resync_every_n = 20U;
	sim.rx.data.cfg.resync_every_n = 20U;
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	memcpy(salt, sim.rx.ctl.pair_salt, sizeof(salt));
	memcpy(rx_id, sim.rx.dev_id, sizeof(rx_id));

	/* RX reboot: all link state gone, only the pairing record (dev_id and nonces) survives */
	node_init(&sim.rx, ZLRS_ROLE_RX, sim.root_key, rx_id, 0x22222222U);
	sim.rx.ctl.cfg.resync_every_n = 20U;
	sim.rx.data.cfg.resync_every_n = 20U;
	zlrs_link_set_peer(&sim.rx.ctl, sim.tx.dev_id, salt, sim.now_ms);
	zlrs_link_set_peer(&sim.rx.data, sim.tx.dev_id, salt, sim.now_ms);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(sim.rx.data.state == ZLRS_STATE_SEARCHING);

	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim.rx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim.tx.data.state == ZLRS_STATE_CONNECTED &&
			  sim.rx.data.state == ZLRS_STATE_CONNECTED,
		  4000);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.tx.data.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.data.state == ZLRS_STATE_CONNECTED);
	/* After the reconnect the keys still match */
	CHECK(memcmp(sim.tx.ctl.session_key_out, sim.rx.ctl.session_key_in, ZLRS_KEY_LEN) == 0);

	/* RC and passthrough keep working */
	memset(rc, 0x91U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	sim_run(&sim, 4U);
	CHECK(sim.rx.rc_count > 0U);
	CHECK(memcmp(sim.rx.rc, rc, sizeof(rc)) == 0);
	memset(chunk, 0x92U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/*
 * A replayed old SYNC must not move the session: the tag is pairing-level, so a recorded SYNC
 * verifies. That is why switching sessions needs recent total silence and staying needs a new seq.
 */
static void test_sync_replay_guard(void)
{
	sim_t sim;
	uint8_t sync[ZLRS_FRAME_MAX_LEN];
	uint16_t sync_len;
	uint8_t key_before[ZLRS_KEY_LEN];
	uint32_t nonce_before;
	uint32_t sync_nonce;

	printf("link: connected-state sync replay\n");

	sim_init(&sim);
	sim.tx.ctl.cfg.resync_every_n = 20U;
	sim.rx.ctl.cfg.resync_every_n = 20U;
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);

	/* Record the current session's SYNC and put it back after the TX has changed sessions */
	CHECK(sim.sync_len > 0U);
	sync_len = sim.sync_len;
	memcpy(sync, sim.sync, sync_len);

	/* Cut the uplink so the TX declares peer loss, rolls a nonce and reconnects */
	sim.drop_up = 1000000;
	SIM_UNTIL(&sim, sim.tx.ctl.state == ZLRS_STATE_SEARCHING, 2000);
	sim.drop_up = 0;
	CHECK(sim.tx.ctl.state == ZLRS_STATE_SEARCHING);
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim.rx.ctl.state == ZLRS_STATE_CONNECTED,
		  4000);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);

	/* This frame's nonce belongs to the previous session */
	nonce_before = sim.rx.ctl.sess_nonce_t;
	memcpy(key_before, sim.rx.ctl.session_key_in, ZLRS_KEY_LEN);
	sync_nonce = (uint32_t)sync[7] | ((uint32_t)sync[8] << 8) | ((uint32_t)sync[9] << 16) |
		     ((uint32_t)sync[10] << 24);
	CHECK(sync_nonce != nonce_before);

	/* The session is healthy: the old SYNC can only be dropped */
	const uint32_t frame_before = sim.rx.ctl.frame_index;
	sim_inject(&sim, &sim.rx.ctl, sync, sync_len);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.sess_nonce_t == nonce_before);
	CHECK(sim.rx.ctl.frame_index == frame_before);
	CHECK(memcmp(sim.rx.ctl.session_key_in, key_before, ZLRS_KEY_LEN) == 0);
}

/*
 * Modulation is per link (doc/07-architecture.md 3): control runs the LoRa long-range tier while
 * data stays on 2.4G FLRC. LoRa sync words are a separately derived byte (top bit as the private
 * bit) and cannot reuse FLRC's four bytes.
 */
static void test_lora_control(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t tlm[8];
	uint8_t chunk[8];

	printf("link: lora control modulation\n");

	sim_init(&sim);
	sim.tx.ctl.cfg.mod = zlrs_mod_lora(7U, 125000U, 1U, 8U);
	sim.rx.ctl.cfg.mod = zlrs_mod_lora(7U, 125000U, 1U, 8U);
	pair_and_connect(&sim);

	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.tx.data.state == ZLRS_STATE_CONNECTED);

	/* The derived one-byte sync word: same on both ends, private bit set, control and data separated */
	CHECK(sim.tx.ctl.lora_sync == sim.rx.ctl.lora_sync);
	CHECK((sim.tx.ctl.lora_sync & 0x80U) != 0U);
	CHECK(sim.tx.ctl.lora_sync != sim.tx.data.lora_sync);
	CHECK(sim.tx.ctl.cfg.mod.sync_byte == sim.tx.ctl.lora_sync);
	/* A LoRa tier has no rate field */
	CHECK(sim.tx.ctl.cfg.mod.rate_bps == 0U);
	/* The data link's modulation is untouched */
	CHECK(sim.tx.data.cfg.mod.modulation == (uint8_t)ZLRS_MOD_FLRC);

	/* RC, telemetry and passthrough keep working */
	memset(rc, 0xA1U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	memset(tlm, 0xB1U, sizeof(tlm));
	zlrs_link_set_tlm(&sim.rx.ctl, tlm, sizeof(tlm));
	sim_run(&sim, 4U);
	CHECK(sim.rx.rc_count > 0U);
	CHECK(memcmp(sim.rx.rc, rc, sizeof(rc)) == 0);
	CHECK(sim.tx.tlm_count > 0U);
	CHECK(memcmp(sim.tx.tlm, tlm, sizeof(tlm)) == 0);
	memset(chunk, 0xC1U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/*
 * The protocol side of 1000 Hz (doc/09-protocol.md 4.1): at a 1 ms frame the superframe, hop
 * lockstep, window thinning and periodic resync all have to hold. The radio needs direct driver
 * access to reach 1000 Hz (RAC schedules in ms); this case pins the protocol and time constants.
 */
static void test_high_frame_rate(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t tlm[8];
	uint8_t chunk[8];

	printf("link: 1000 Hz frame cadence\n");

	sim_init(&sim);
	sim.frame_ms = 1U; /* one frame is 1 ms */
	/* Only the 2.6 Mbps tier fits a 1 ms frame; the cadence factors come from the plan */
	sim.tx.ctl.cfg.mod = zlrs_mod_flrc(2600000U);
	sim.rx.ctl.cfg.mod = zlrs_mod_flrc(2600000U);
	sim.tx.ctl.cfg.tlm_every_n = 6U; /* about 167 times per second */
	sim.rx.ctl.cfg.tlm_every_n = 6U;
	sim.tx.data.cfg.data_every_n = 10U; /* 100 times per second */
	sim.rx.data.cfg.data_every_n = 10U;
	sim.tx.ctl.cfg.resync_every_n = 100U; /* resync every 100 ms */
	sim.rx.ctl.cfg.resync_every_n = 100U;
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.tx.data.state == ZLRS_STATE_CONNECTED);

	memset(rc, 0xD1U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	memset(tlm, 0xD2U, sizeof(tlm));
	zlrs_link_set_tlm(&sim.rx.ctl, tlm, sizeof(tlm));
	sim_run(&sim, 300U); /* 300 ms */

	/* Over 300 ms the 2.6 Mbps tier should lose almost nothing */
	CHECK(sim.rx.rc_count > 250U);
	CHECK(memcmp(sim.rx.rc, rc, sizeof(rc)) == 0);
	CHECK(sim.tx.tlm_count > 30U);
	/* Hopping stays in lockstep and resync frames appeared on their 100-frame cadence */
	CHECK(sim.tx.ctl.fhss.index == sim.rx.ctl.fhss.index);
	CHECK(sim.rx.ctl.stats.rx_syncs > 0U);
	/* The data window thins to every 10 frames and the passthrough still works */
	memset(chunk, 0xD3U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

static void test_handshake_cadence(void)
{
	sim_t sim;
	sim_init(&sim);
	sim.tx.ctl.frame_index = 70000U;
	sim.tx.data.frame_index = 80000U;
	sim.rx.ctl.frame_index = 17U;
	sim.rx.data.frame_index = 29U;
	sim.tx.ctl.cfg.resync_every_n = sim.rx.ctl.cfg.resync_every_n = 5U;
	sim.tx.data.cfg.resync_every_n = sim.rx.data.cfg.resync_every_n = 5U;
	sim.tx.data.cfg.data_every_n = sim.rx.data.cfg.data_every_n = 3U;
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.frame_index == sim.rx.ctl.frame_index);
	CHECK(sim.tx.data.frame_index == sim.rx.data.frame_index);
	CHECK(sim.tx.data.fhss.index == sim.rx.data.fhss.index);
	uint8_t payload[64];
	memset(payload, 0x5a, sizeof(payload));
	CHECK(zlrs_link_write(&sim.tx.data, payload, sizeof(payload)) == sizeof(payload));
	SIM_UNTIL(&sim, sim.rx.stream_len == sizeof(payload), 100);
	CHECK(sim.rx.stream_len == sizeof(payload));
	CHECK(memcmp(sim.rx.stream, payload, sizeof(payload)) == 0);
}

static void test_skipped_slots(void)
{
	sim_t sim;
	sim_init(&sim);
	pair_and_connect(&sim);
	zlrs_link_t *links[] = {&sim.tx.ctl, &sim.rx.ctl, &sim.tx.data, &sim.rx.data};

	for (unsigned int i = 0; i < 4U; i++) {
		zlrs_link_t *link = links[i];
		link->state = ZLRS_STATE_CONNECTED;
		link->cfg.data_every_n = 3U;
		link->cfg.resync_every_n = 5U;
		link->announce = false;
		link->sess_cfm_due = false;
		zlrs_link_t skipped = *link;
		const uint16_t seq_before = skipped.tx_seq;
		const uint8_t first = i < 2U ? ZLRS_SLOT_CTL_DOWN : ZLRS_SLOT_DATA_A;

		for (uint32_t frame = 0; frame < 37U; frame++) {
			for (uint8_t slot = first; slot < first + 2U; slot++) {
				if (zlrs_link_slot(link, slot, frame * SIM_FRAME_MS) != NULL) {
					zlrs_link_complete(link, ZLRS_RADIO_FAIL, 0U, 0, 0,
							   frame * SIM_FRAME_MS);
				}
				zlrs_link_skip_slot(&skipped, slot);
			}
			CHECK(skipped.frame_index == link->frame_index);
			CHECK(skipped.fhss.index == link->fhss.index);
			CHECK(skipped.stats.hop_advances == link->stats.hop_advances);
		}
		CHECK(skipped.tx_seq == seq_before);
		CHECK(!skipped.req_pending);
		CHECK(skipped.state == ZLRS_STATE_CONNECTED);
	}
}

void test_link(void)
{
	test_skipped_slots();
	test_handshake_cadence();
	test_pairing();
	test_pairing_repeat_req();
	test_pairing_lost_confirm();
	test_pairing_id_order();
	test_session();
	test_session_lost_confirm();
	test_fresh_session_uplink();
	test_data_reacquire_epoch();
	test_reflection();
	test_pairing_rotation();
	test_session_replay();
	test_third_node_isolation();
	test_intruder_without_pair_salt();
	test_group_member_impersonates();
	test_time_wrap();
	test_seq_wrap();
	test_control_payloads();
	test_data_passthrough();
	test_data_long_write();
	test_rx_rejects();
	test_frag_drop();
	test_recovery();
	test_hop_lockstep();
	test_unconfigured();
	test_sync_rate();
	test_link_quality();
	test_backpressure();
	test_arq_retransmit();
	test_arq_slow_run();
	test_arq_dedup();
	test_arq_giveup();
	test_arq_bidirectional_recovery();
	test_arq_recovery_preserves_progress();
	test_arq_recovery_lost_ack();
	test_rate_adapt();
	test_failsafe_push();
	test_data_idle();
	test_cadence();
	test_isolation_no_tag();
	test_isolation_fixed_channel();
	test_isolation_no_handshake();
	test_isolation_no_telemetry();
	test_hop_resync();
	test_sub_mix();
	test_sub_mix_data_path();
	test_peer_restart();
	test_sync_replay_guard();
	test_lora_control();
	test_high_frame_rate();
}

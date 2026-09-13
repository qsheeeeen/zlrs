/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 双节点链路仿真：
 *   - 两个节点，一个 TX 角色、一个 RX 角色，各开控制链路与数据链路
 *   - "空中"是一次函数调用，可以注入丢包、损坏与重放
 *   - 超帧顺序与 App 里一致：CTL_DOWN -> CTL_UP -> DATA_A -> DATA_B，每帧 6 ms
 */

#include "test.h"

#include <string.h>

#include "zlrs/link/link.h"

#define SIM_FRAME_MS 6U
#define SIM_RATE_CTL 1300000U
#define SIM_RATE_DATA 2600000U
#define SIM_DATA_MTU 128U
#define SIM_FRAMES_PER_SEC (1000U / SIM_FRAME_MS)

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

	/* 回调记录 */
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint16_t rc_len;
	uint32_t rc_count;
	uint8_t tlm[ZLRS_LINK_TLM_MAX_LEN];
	uint16_t tlm_len;
	uint32_t tlm_count;
	uint8_t stream[2048];
	uint32_t stream_len;
	uint8_t ctl_state;
	uint8_t data_state;
};

typedef struct {
	node_t tx;
	node_t rx;
	uint32_t now_ms;
	int32_t drop_down; /* 还需丢弃多少次 TX->RX 投递 */
	int32_t drop_up;   /* 还需丢弃多少次 RX->TX 投递 */
	bool corrupt_next;
	bool replay_next;
	uint8_t down[ZLRS_FRAME_MAX_LEN];
	uint16_t down_len;
	uint8_t up[ZLRS_FRAME_MAX_LEN];
	uint16_t up_len;
	uint8_t beacon[ZLRS_FRAME_MAX_LEN];
	uint16_t beacon_len;
} sim_t;

/* ---- 回调与假射频 -------------------------------------------------------- */

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

static void on_state(void *ctx, uint8_t state)
{
	link_ctx_t *lc = ctx;

	if (lc->kind == ZLRS_LINK_CONTROL) {
		lc->node->ctl_state = state;
	} else {
		lc->node->data_state = state;
	}
}

/* ---- 节点与仿真环境 ------------------------------------------------------ */

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
		.rate_bps = SIM_RATE_CTL,
		.tx_power_dbm = 10,
		.rx_timeout_ms = 2U,
		.beacon_period_ms = 1000U,
		.peer_timeout_ms = (role == ZLRS_ROLE_TX) ? 250U : 100U,
		.pair_period_ms = 100U,
		.pair_window_ms = 30000U,
		.data_mtu = 0U,
	};
	zlrs_link_events_t ctl_ev = {
		.on_rc = on_rc,
		.on_tlm = on_tlm,
		.on_state = on_state,
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
	data_cfg.rate_bps = SIM_RATE_DATA;
	data_cfg.peer_timeout_ms = 0U; /* 数据链路不做失联判定 */
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

static void sim_init(sim_t *sim)
{
	static uint8_t root_key[ZLRS_KEY_LEN];
	static const uint8_t tx_id[ZLRS_DEV_ID_LEN] = { 0x11, 0x22, 0x33, 0x44 };
	static const uint8_t rx_id[ZLRS_DEV_ID_LEN] = { 0xaa, 0xbb, 0xcc, 0xdd };

	memset(sim, 0, sizeof(*sim));
	for (uint8_t i = 0; i < sizeof(root_key); i++) {
		root_key[i] = (uint8_t)(i + 1U);
	}

	node_init(&sim->tx, ZLRS_ROLE_TX, root_key, tx_id, 0x11111111U);
	node_init(&sim->rx, ZLRS_ROLE_RX, root_key, rx_id, 0x22222222U);
}

static zlrs_link_t *link_of(node_t *node, uint8_t slot)
{
	return (slot == ZLRS_SLOT_CTL_DOWN || slot == ZLRS_SLOT_CTL_UP) ? &node->ctl
								       : &node->data;
}

static void sim_deliver(sim_t *sim, zlrs_link_t *src_link, const zlrs_radio_req_t *src,
			zlrs_link_t *dst_link, const zlrs_radio_req_t *dst, int32_t *drop)
{
	uint16_t len = src->size;

	zlrs_link_complete(src_link, ZLRS_RADIO_OK, 0U, 0, sim->now_ms);

	if (*drop > 0) {
		(*drop)--;
		zlrs_link_complete(dst_link, ZLRS_RADIO_OK, 0U, 0, sim->now_ms);
		return;
	}

	if (len > ZLRS_FRAME_MAX_LEN) {
		len = ZLRS_FRAME_MAX_LEN;
	}
	if (src_link == &sim->tx.ctl || src_link == &sim->tx.data) {
		memcpy(sim->down, src->buf, len);
		sim->down_len = len;
	} else {
		memcpy(sim->up, src->buf, len);
		sim->up_len = len;
	}
	if ((src->buf[0] >> 2) == (uint8_t)ZLRS_PKT_BEACON && sim->beacon_len == 0U) {
		memcpy(sim->beacon, src->buf, len);
		sim->beacon_len = len;
	}

	memcpy(dst->buf, src->buf, len);
	if (sim->corrupt_next) {
		dst->buf[len - 1U] ^= 0xFFU;
		sim->corrupt_next = false;
	}
	zlrs_link_complete(dst_link, ZLRS_RADIO_OK, len, -60, sim->now_ms);

	if (sim->replay_next) {
		sim->replay_next = false;
		zlrs_link_complete(dst_link, ZLRS_RADIO_OK, len, -60, sim->now_ms);
	}
}

static void sim_slot(sim_t *sim, uint8_t slot)
{
	zlrs_link_t *a = link_of(&sim->tx, slot);
	zlrs_link_t *b = link_of(&sim->rx, slot);
	const zlrs_radio_req_t *ra = zlrs_link_slot(a, slot, sim->now_ms);
	const zlrs_radio_req_t *rb = zlrs_link_slot(b, slot, sim->now_ms);

	if (ra != NULL && ra->is_tx && rb != NULL && !rb->is_tx) {
		sim_deliver(sim, a, ra, b, rb, &sim->drop_down);
	} else if (rb != NULL && rb->is_tx && ra != NULL && !ra->is_tx) {
		sim_deliver(sim, b, rb, a, ra, &sim->drop_up);
	} else {
		if (ra != NULL) {
			zlrs_link_complete(a, ZLRS_RADIO_OK, 0U, 0, sim->now_ms);
		}
		if (rb != NULL) {
			zlrs_link_complete(b, ZLRS_RADIO_OK, 0U, 0, sim->now_ms);
		}
	}
}

static void sim_run(sim_t *sim, uint32_t frames)
{
	for (uint32_t i = 0; i < frames; i++) {
		sim_slot(sim, ZLRS_SLOT_CTL_DOWN);
		sim_slot(sim, ZLRS_SLOT_CTL_UP);
		sim_slot(sim, ZLRS_SLOT_DATA_A);
		sim_slot(sim, ZLRS_SLOT_DATA_B);

		zlrs_link_tick(&sim->tx.ctl, sim->now_ms);
		zlrs_link_tick(&sim->tx.data, sim->now_ms);
		zlrs_link_tick(&sim->rx.ctl, sim->now_ms);
		zlrs_link_tick(&sim->rx.data, sim->now_ms);

		sim->now_ms += SIM_FRAME_MS;
	}
}

#define SIM_UNTIL(sim, cond, limit)                       \
	do {                                              \
		for (int _i = 0; _i < (limit); _i++) {    \
			if (cond) {                       \
				break;                    \
			}                                 \
			sim_run((sim), 1U);               \
		}                                         \
	} while (0)

/* 直接往某个链路的接收缓冲里塞一帧，绕开仿真投递 */
static void sim_inject(sim_t *sim, zlrs_link_t *link, const uint8_t *frame, uint16_t len)
{
	zlrs_link_rx(link, frame, len, -60, sim->now_ms);
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
	zlrs_tag(key, ZLRS_KEY_LEN, out, ZLRS_FRAME_HDR_LEN + len,
		 &out[ZLRS_FRAME_HDR_LEN + len]);

	return total;
}

/* 配对 + 建会话，并把配对记录同步给数据链路（App 侧要做的事） */
static void pair_and_connect(sim_t *sim)
{
	zlrs_link_press(&sim->tx.ctl, sim->now_ms);
	zlrs_link_press(&sim->rx.ctl, sim->now_ms);

	SIM_UNTIL(sim,
		  sim->tx.ctl.state == ZLRS_STATE_SEARCHING &&
			  sim->rx.ctl.state == ZLRS_STATE_SEARCHING,
		  2000);

	zlrs_link_set_peer(&sim->tx.data, sim->rx.dev_id, sim->now_ms);
	zlrs_link_set_peer(&sim->rx.data, sim->tx.dev_id, sim->now_ms);

	SIM_UNTIL(sim,
		  sim->tx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim->rx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim->tx.data.state == ZLRS_STATE_CONNECTED &&
			  sim->rx.data.state == ZLRS_STATE_CONNECTED,
		  2000);
}

/* ---- 用例 ---------------------------------------------------------------- */

static void test_pairing(void)
{
	sim_t sim;
	uint8_t word_tx[ZLRS_SYNC_WORD_LEN];
	uint8_t word_rx[ZLRS_SYNC_WORD_LEN];
	uint8_t data_tx[ZLRS_SYNC_WORD_LEN];
	uint8_t data_rx[ZLRS_SYNC_WORD_LEN];

	printf("link: pairing\n");
	sim_init(&sim);

	/* 没按键时不参与 */
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
	CHECK(memcmp(word_tx, word_rx, sizeof(word_tx)) == 0); /* 两端一致 */
	CHECK(memcmp(data_tx, data_rx, sizeof(data_tx)) == 0);
	CHECK(memcmp(word_tx, data_tx, sizeof(word_tx)) != 0); /* 域分离 */

	/* 再跑几个超帧，等 BEACON 出现在空中 */
	sim_run(&sim, 5U);
	CHECK(sim.beacon_len > 0U);

	/* 配对窗口到点自动退出 */
	sim_init(&sim);
	zlrs_link_press(&sim.tx.ctl, sim.now_ms);
	sim_run(&sim, (30000U / SIM_FRAME_MS) + 200U);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_UNPAIRED);
	CHECK(sim.tx.ctl.stats.tx_frames > 10U); /* 期间一直在发 PAIR_REQ */
	CHECK(!sim.tx.ctl.paired);
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

	CHECK(memcmp(sim.tx.ctl.session_key, sim.rx.ctl.session_key, ZLRS_KEY_LEN) == 0);
	CHECK(sim.tx.ctl.session_nonce == sim.rx.ctl.session_nonce);
	CHECK(memcmp(sim.tx.data.session_key, sim.rx.data.session_key, ZLRS_KEY_LEN) == 0);
	CHECK(memcmp(sim.tx.ctl.session_key, sim.tx.data.session_key, ZLRS_KEY_LEN) != 0);

	/* 重放旧 BEACON 不换会话 */
	nonce = sim.rx.ctl.session_nonce;
	saved_len = sim.beacon_len;
	CHECK(saved_len > 0U);
	saved_len = (saved_len > sizeof(saved)) ? (uint16_t)sizeof(saved) : saved_len;
	memcpy(saved, sim.beacon, saved_len);
	sim_inject(&sim, &sim.rx.ctl, saved, saved_len);
	CHECK(sim.rx.ctl.session_nonce == nonce);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
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

static void test_rx_rejects(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t bad[ZLRS_FRAME_MAX_LEN];
	uint8_t tiny[2] = { 0x00, 0x01 };
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

	/* 重放同一帧：序列号不新，计入 rx_dup */
	count_before = sim.rx.rc_count;
	dup_before = sim.rx.ctl.stats.rx_dup;
	sim_inject(&sim, &sim.rx.ctl, sim.down, sim.down_len);
	CHECK(sim.rx.ctl.stats.rx_dup == dup_before + 1U);
	CHECK(sim.rx.rc_count == count_before);

	/* 改 tag：校验失败 */
	tag_before = sim.rx.ctl.stats.rx_bad_tag;
	memcpy(bad, sim.down, sim.down_len);
	bad[sim.down_len - 1U] ^= 0xFFU;
	sim_inject(&sim, &sim.rx.ctl, bad, sim.down_len);
	CHECK(sim.rx.ctl.stats.rx_bad_tag == tag_before + 1U);

	/* 长度不足：直接拒绝 */
	frame_before = sim.rx.ctl.stats.rx_bad_frame;
	sim_inject(&sim, &sim.rx.ctl, tiny, sizeof(tiny));
	CHECK(sim.rx.ctl.stats.rx_bad_frame == frame_before + 1U);
}

static void test_frag_drop(void)
{
	sim_t sim;
	uint8_t frame[ZLRS_FRAME_MAX_LEN];
	uint8_t chunk[6] = { 1U, 2U, 3U, 4U, 5U, 6U };
	const uint8_t *key;
	uint32_t drop_before;
	uint16_t seq;
	uint16_t len;

	printf("link: fragmentation recovery\n");
	sim_init(&sim);
	pair_and_connect(&sim);
	CHECK(sim.rx.data.state == ZLRS_STATE_CONNECTED);

	key = sim.rx.data.session_key;
	seq = (uint16_t)(sim.rx.data.last_rx_seq + 1U);

	/* 缺口：整段丢弃 */
	drop_before = sim.rx.data.stats.rx_frag_drop;
	len = build_data_frame(frame, ZLRS_PKT_DATA_UP, seq, ZLRS_FLAG_MORE, chunk,
			       sizeof(chunk), key);
	CHECK(len > 0U);
	sim_inject(&sim, &sim.rx.data, frame, len);
	CHECK(sim.rx.data.reasm.len == sizeof(chunk));

	seq = (uint16_t)(seq + 2U);
	len = build_data_frame(frame, ZLRS_PKT_DATA_UP, seq, ZLRS_FLAG_MORE, chunk,
			       sizeof(chunk), key);
	sim_inject(&sim, &sim.rx.data, frame, len);
	CHECK(sim.rx.data.stats.rx_frag_drop == drop_before + 1U);
	CHECK(sim.rx.data.reasm.len == 0U);
	CHECK(sim.rx.stream_len == 0U);

	/* 超时：残留丢弃，新分片重新起头 */
	seq = (uint16_t)(seq + 1U);
	len = build_data_frame(frame, ZLRS_PKT_DATA_UP, seq, ZLRS_FLAG_MORE, chunk,
			       sizeof(chunk), key);
	sim_inject(&sim, &sim.rx.data, frame, len);
	CHECK(sim.rx.data.reasm.len == sizeof(chunk));

	sim.now_ms += ZLRS_FRAG_TIMEOUT_MS + 5U;
	seq = (uint16_t)(seq + 1U);
	len = build_data_frame(frame, ZLRS_PKT_DATA_UP, seq, ZLRS_FLAG_MORE, chunk,
			       sizeof(chunk), key);
	sim_inject(&sim, &sim.rx.data, frame, len);
	CHECK(sim.rx.data.reasm.len == sizeof(chunk)); /* 只剩新片 */
	CHECK(sim.rx.data.stats.rx_frag_drop == drop_before + 1U);
}

static void test_recovery(void)
{
	sim_t sim;

	printf("link: loss and recovery\n");
	sim_init(&sim);
	pair_and_connect(&sim);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* 断掉双向投递：控制链路按各自超时回到 SEARCHING */
	sim.drop_down = 100000;
	sim.drop_up = 100000;
	sim_run(&sim, 100U); /* 600 ms */

	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(sim.rx.ctl_state == ZLRS_STATE_SEARCHING); /* on_state 通知过（失控保护用） */
	CHECK(sim.tx.ctl.paired && sim.rx.ctl.paired); /* 配对记录还在 */

	/* 恢复投递：靠新的 BEACON 重新连上，不需要重新配对 */
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
	CHECK(zlrs_fhss_current_hz(&sim.tx.ctl.fhss) ==
	      zlrs_fhss_current_hz(&sim.rx.ctl.fhss));
	CHECK(zlrs_fhss_current_hz(&sim.tx.data.fhss) ==
	      zlrs_fhss_current_hz(&sim.rx.data.fhss));
}

static void test_unconfigured(void)
{
	node_t node;
	static const uint8_t dev_id[ZLRS_DEV_ID_LEN] = { 1U, 2U, 3U, 4U };

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

static void test_beacon_rate(void)
{
	sim_t sim;
	uint32_t before;
	uint32_t sent;

	printf("link: beacon rate\n");
	sim_init(&sim);
	zlrs_link_press(&sim.tx.ctl, sim.now_ms);
	zlrs_link_press(&sim.rx.ctl, sim.now_ms);
	SIM_UNTIL(&sim, sim.tx.ctl.state == ZLRS_STATE_SEARCHING, 2000);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_SEARCHING);

	/* 对端收不到：此时 TX 只剩 BEACON 一种发送 */
	sim.drop_down = 100000;
	before = sim.tx.ctl.stats.tx_frames;
	sim_run(&sim, 3U * SIM_FRAMES_PER_SEC);
	sent = sim.tx.ctl.stats.tx_frames - before;
	CHECK(sent >= 3U && sent <= 5U);
}

void test_link(void)
{
	test_pairing();
	test_session();
	test_control_payloads();
	test_data_passthrough();
	test_rx_rejects();
	test_frag_drop();
	test_recovery();
	test_hop_lockstep();
	test_unconfigured();
	test_beacon_rate();
}

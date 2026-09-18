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
#include "zlrs/link/rate.h"

#define SIM_FRAME_MS 6U
#define SIM_RATE_CTL 1300000U
#define SIM_RATE_DATA 2600000U
#define SIM_DATA_MTU 128U
#define SIM_FRAMES_PER_SEC (1000U / SIM_FRAME_MS)
#define SIM_MAX_NODES 3U /* 主对两个 + 可选的第三个（同场设备或攻击者） */

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
	node_t nth;        /* 第三个节点：同场设备或攻击者，只有三节点用例启用 */
	bool nth_enabled;
	uint32_t now_ms;
	/* 一个超帧多少毫秒：默认 6（≈167 Hz），1000 Hz 的用例把它设成 1 */
	uint32_t frame_ms;
	uint8_t root_key[ZLRS_KEY_LEN];
	int32_t drop_down; /* 还需丢弃多少次 TX->RX 投递 */
	int32_t drop_up;   /* 还需丢弃多少次 RX->TX 投递 */
	int32_t drop_data_down; /* 只丢 TX->RX 的数据帧（ARQ 用例用） */
	bool replay_data_next;  /* 把下一个数据帧在接收端再投一次（去重用例用） */
	bool corrupt_next;
	bool replay_next;
	uint8_t down[ZLRS_FRAME_MAX_LEN];
	uint16_t down_len;
	uint8_t up[ZLRS_FRAME_MAX_LEN];
	uint16_t up_len;
	uint8_t sync[ZLRS_FRAME_MAX_LEN];
	uint16_t sync_len;
	uint8_t sess_resp[ZLRS_FRAME_MAX_LEN]; /* 会话响应（重放用例用） */
	uint16_t sess_resp_len;
} sim_t;

/*
 * 上板调试开关：默认全是"生产"档，单个用例可以按需关掉某一项，验证关掉之后
 * 链路仍然工作（见 doc/12-debug.md 第 3 节）。建节点之前设置。
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
		.mod = { .modulation = ZLRS_MOD_FLRC, .rate_bps = SIM_RATE_CTL },
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
	data_cfg.peer_timeout_ms = 0U; /* 数据链路不判失联，见 doc/09-protocol.md 3.7 */
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

/* features = NULL 用生产档；上板调试档由用例显式传进来 */
static void sim_init_with(sim_t *sim, const sim_features_t *features)
{
	static uint8_t root_key[ZLRS_KEY_LEN];
	static const uint8_t tx_id[ZLRS_DEV_ID_LEN] = { 0x11, 0x22, 0x33, 0x44 };
	static const uint8_t rx_id[ZLRS_DEV_ID_LEN] = { 0xaa, 0xbb, 0xcc, 0xdd };

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
	return (slot == ZLRS_SLOT_CTL_DOWN || slot == ZLRS_SLOT_CTL_UP) ? &node->ctl
								       : &node->data;
}

/*
 * 空口上发生的事：只有频段与同步字都对得上的接收方才收得到（射频层过滤）。
 * 比的是事务里实际用的同步字——配对阶段用的是公开常量，不是派生出来的那对。
 */
static bool sim_phy_match(const zlrs_radio_req_t *src, const zlrs_radio_req_t *dst,
			  const zlrs_link_t *src_link, const zlrs_link_t *dst_link)
{
	return src_link->cfg.domain == dst_link->cfg.domain &&
	       memcmp(src->sync_word, dst->sync_word, ZLRS_SYNC_WORD_LEN) == 0;
}

/* 记录主对发出去的帧：sync / 会话响应抓取与上下行缓冲都只认主对 */
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

/* 把一帧交给某个接收请求（等价于射频把这串字节写进它的接收缓冲） */
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
	bool served[SIM_MAX_NODES] = { false };
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
			/* 主对的丢包开关只作用于主对（0 = TX 节点，1 = RX 节点） */
			const bool primary = (i < 2U && j < 2U);
			int32_t *drop = NULL;

			if (j == i || reqs[j] == NULL || reqs[j]->is_tx || served[j] ||
			    !sim_phy_match(reqs[i], reqs[j], links[i], links[j])) {
				continue;
			}

			if (primary) {
				if (i == 0U && j == 1U) {
					drop = (slot == ZLRS_SLOT_DATA_A &&
						sim->drop_data_down > 0)
						       ? &sim->drop_data_down
						       : &sim->drop_down;
				} else if (i == 1U && j == 0U) {
					drop = &sim->drop_up;
				}
			}

			if (drop != NULL && *drop > 0) {
				(*drop)--;
				zlrs_link_complete(links[j], ZLRS_RADIO_OK, 0U, 0, 0,
						   sim->now_ms);
				served[j] = true;
				continue;
			}

			sim_feed(sim, links[j], reqs[j], reqs[i]->buf, len, primary);
			served[j] = true;
		}
	}

	/* 没收到东西的接收请求，也要收尾 */
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
		/* 数据帧重放：接收端应该按数据序号去重，不重复投递 */
		if (sim->replay_data_next && sim->down_len > 0U &&
		    (sim->down[0] >> 2) == (uint8_t)ZLRS_PKT_DATA_UP) {
			sim->replay_data_next = false;
			zlrs_link_rx(&sim->rx.data, sim->down, sim->down_len, -60, 0,
				     sim->now_ms);
		}
		sim_slot(sim, ZLRS_SLOT_DATA_B);

		zlrs_link_tick(&sim->tx.ctl, sim->now_ms);
		zlrs_link_tick(&sim->tx.data, sim->now_ms);
		zlrs_link_tick(&sim->rx.ctl, sim->now_ms);
		zlrs_link_tick(&sim->rx.data, sim->now_ms);

		sim->now_ms += sim->frame_ms;
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

	/* 配对记录（dev_id + 配对 nonce）同步给数据链路，App 侧要做的事 */
	zlrs_link_set_peer(&sim->tx.data, sim->rx.dev_id, sim->tx.ctl.pair_salt,
			   sim->now_ms);
	zlrs_link_set_peer(&sim->rx.data, sim->tx.dev_id, sim->rx.ctl.pair_salt,
			   sim->now_ms);

	SIM_UNTIL(sim,
		  sim->tx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim->rx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim->tx.data.state == ZLRS_STATE_CONNECTED &&
			  sim->rx.data.state == ZLRS_STATE_CONNECTED,
		  2000);
}

/*
 * 启用第三个节点。三节点用例共用它，差别只在参数：
 *   - 同场另一对设备：自己的 dev_id / peer / salt
 *   - 攻击者：偷来的 dev_id（主对 TX 的）+ 主对 RX 当 peer；salt 对/错决定它是
 *     "听过那次配对"（能冒充）还是只抓到了同步字（过不了 tag）
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

/* ---- 用例 ---------------------------------------------------------------- */

/*
 * 三节点之一：同场另一对设备。射频同步字不同，两边在物理层就互相听不到——
 * 这是 doc/09-protocol.md 第 2 节与 doc/10-security.md 7.2"组内其它设备误占链路"那条声明的依据。
 */
static void test_third_node_isolation(void)
{
	sim_t sim;
	uint8_t chunk[16];
	static const uint8_t other_id[ZLRS_DEV_ID_LEN] = { 0x55, 0x66, 0x77, 0x88 };
	static const uint8_t other_peer[ZLRS_DEV_ID_LEN] = { 0x99, 0xaa, 0xbb, 0xcc };
	static const uint8_t other_salt[ZLRS_PAIR_SALT_LEN] = { 9U, 9U, 9U, 9U, 9U, 9U, 9U, 9U };

	printf("link: third node isolation\n");
	sim_init(&sim);
	pair_and_connect(&sim);

	sim_enable_nth(&sim, ZLRS_ROLE_TX, other_id, other_peer, other_salt);
	sim_run(&sim, 200U);

	/* 第三节点发得出去，但一帧都听不到主对 */
	CHECK(sim.nth.ctl.stats.tx_frames > 0U);
	CHECK(sim.nth.ctl.stats.rx_frames == 0U);
	CHECK(sim.nth.ctl.stats.rx_bad_tag == 0U);

	/* 主对也一帧都没收到它的：连坏 tag 都没有，说明没进射频 */
	CHECK(sim.tx.ctl.stats.rx_bad_tag == 0U);
	CHECK(sim.rx.ctl.stats.rx_bad_tag == 0U);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* 主对照常透传 */
	memset(chunk, 0x5aU, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/*
 * 三节点之二：攻击者偷到主对 TX 的 dev_id，也从空中抓到了同步字，但没听过那次
 * 配对（salt 是错的）。帧能进接收机，tag 过不去——这就是 pair_key 掺配对 nonce
 * 换来的东西（doc/10-security.md 第 3 节）。
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

	/* 真 TX 先静默，让 RX 掉回 SEARCHING——这正是攻击者要抓的时机 */
	sim.drop_down = 1000000;
	SIM_UNTIL(&sim, sim.rx.ctl.state == ZLRS_STATE_SEARCHING, 400);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);

	sim_enable_nth(&sim, ZLRS_ROLE_TX, sim.tx.dev_id, sim.rx.dev_id, wrong_salt);
	/* 同步字是公开发送的：攻击者抄下来就能把帧送进接收机 */
	memcpy(sim.nth.ctl.sync_word, sim.tx.ctl.sync_word, ZLRS_SYNC_WORD_LEN);
	memcpy(sim.nth.data.sync_word, sim.tx.data.sync_word, ZLRS_SYNC_WORD_LEN);

	bad_before = sim.rx.ctl.stats.rx_bad_tag;
	sim_run(&sim, 300U);

	/* 收到了，但一帧都没验过，会话也没被建起来 */
	CHECK(sim.rx.ctl.stats.rx_bad_tag > bad_before);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(!sim.rx.ctl.session_valid);

	/* 真 TX 回来：主对重新建链，攻击者插不进来 */
	sim.drop_down = 0;
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim.rx.ctl.state == ZLRS_STATE_CONNECTED,
		  4000);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* 主对照常透传 */
	memset(chunk, 0x6bU, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/*
 * 听过配对的组内成员能算出同一套钥匙，于是能冒充真 TX 注入 RC——这是
 * doc/10-security.md 7.2 声明的固有代价（没有逐设备身份），用例把它钉住。
 */
static void test_group_member_impersonates(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];

	printf("link: group member impersonation (documented)\n");
	sim_init(&sim);
	pair_and_connect(&sim);

	/* 真 TX 静默：丢光它到 RX 的投递，RX 100 ms 后掉回 SEARCHING */
	sim.drop_down = 1000000;
	SIM_UNTIL(&sim, sim.rx.ctl.state == ZLRS_STATE_SEARCHING, 400);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);

	/* 冒充者：dev_id 与主对 TX 相同，salt 是它听过的那一份 */
	sim_enable_nth(&sim, ZLRS_ROLE_TX, sim.tx.dev_id, sim.rx.dev_id,
		       sim.tx.ctl.pair_salt);

	SIM_UNTIL(&sim, sim.rx.ctl.state == ZLRS_STATE_CONNECTED, 2000);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* 注入一帧 RC：RX 会当成真 TX 的遥控数据收下 */
	memset(rc, 0x7bU, sizeof(rc));
	zlrs_link_set_rc(&sim.nth.ctl, rc, sizeof(rc));
	SIM_UNTIL(&sim, sim.rx.rc_count > 0U, 400);
	CHECK(sim.rx.rc_count > 0U);
	CHECK(sim.rx.rc_len == sizeof(rc));
	CHECK(memcmp(sim.rx.rc, rc, sizeof(rc)) == 0);
}

/* 32 位毫秒时间戳回绕：超时与失联判定都按无符号差值算，跨过 0 不能抽风 */
static void test_time_wrap(void)
{
	sim_t sim;
	uint8_t chunk[8];

	printf("link: time wrap\n");
	sim_init(&sim);
	sim.now_ms = 0xFFFFFF00U; /* 约 42 帧之后翻过 2^32 */
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	sim_run(&sim, 200U);
	CHECK(sim.now_ms < 0x1000U); /* 确实翻过去了 */
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	memset(chunk, 0x66U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/* 16 位序号回绕：接收窗口按有符号差值比大小，跨过 0xFFFF 不能把新帧当旧帧 */
static void test_seq_wrap(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t chunk[8];
	uint32_t rc_before;

	printf("link: sequence wrap\n");
	sim_init(&sim);
	pair_and_connect(&sim);

	/* 把游标挪到回绕点前面，省掉 65536 帧的仿真 */
	sim.tx.ctl.tx_seq = 0xFFF0U;
	sim.rx.ctl.last_rx_seq = 0xFFF0U;
	sim.rx.ctl.lq_last_seq = 0xFFF0U;
	memset(rc, 0x21U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));

	sim_run(&sim, 40U);
	CHECK(sim.tx.ctl.tx_seq < 0xFFF0U); /* 序号真的翻过去了 */

	rc_before = sim.rx.rc_count;
	sim_run(&sim, 20U);
	CHECK(sim.rx.rc_count > rc_before);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* 数据链路的序号也一样 */
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

	/* 再跑几个超帧，等 SYNC 出现在空中 */
	sim_run(&sim, 5U);
	CHECK(sim.sync_len > 0U);

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

	/* 会话密钥按方向分开：本机的 out 就是对端的 in，同一台设备的 out != in */
	CHECK(memcmp(sim.tx.ctl.session_key_out, sim.rx.ctl.session_key_in,
		     ZLRS_KEY_LEN) == 0);
	CHECK(memcmp(sim.tx.ctl.session_key_in, sim.rx.ctl.session_key_out,
		     ZLRS_KEY_LEN) == 0);
	CHECK(memcmp(sim.tx.ctl.session_key_out, sim.tx.ctl.session_key_in,
		     ZLRS_KEY_LEN) != 0);
	/* 两个 nonce 两端都拿到：TX 出的 nonce_t、RX 出的 nonce_r */
	CHECK(sim.tx.ctl.sess_nonce_t == sim.rx.ctl.sess_nonce_t);
	CHECK(sim.tx.ctl.sess_nonce_r == sim.rx.ctl.sess_nonce_r);
	CHECK(memcmp(sim.tx.data.session_key_out, sim.rx.data.session_key_in,
		     ZLRS_KEY_LEN) == 0);
	CHECK(memcmp(sim.tx.ctl.session_key_out, sim.tx.data.session_key_out,
		     ZLRS_KEY_LEN) != 0);

	/* 会话建立后 RX 不再听 SYNC：重放当前那帧不能动会话 */
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
 * 反射：把本机自己发出去的帧原样打回本机。tag 钥匙按方向分开之后，发帧用 out、
 * 校验用 in，这些帧必须停在 tag 这一步，不能算成"对端发来的"。
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

	/* 取空口上真实的一帧 TX -> RX（RC） */
	sim_slot(&sim, ZLRS_SLOT_CTL_DOWN);
	CHECK(sim.down_len > 0U);
	CHECK((sim.down[0] >> 2) == (uint8_t)ZLRS_PKT_RC);
	len = sim.down_len;
	memcpy(frame, sim.down, len);

	/* 取空口上真实的一帧 RX -> TX（TLM） */
	sim_slot(&sim, ZLRS_SLOT_CTL_UP);
	CHECK(sim.up_len > 0U);
	CHECK((sim.up[0] >> 2) == (uint8_t)ZLRS_PKT_TLM);

	/* TX 收到自己发的 RC：tag 是"本机在前"算的，TX 校验用"对端在前" —— 必须丢 */
	counter = sim.tx.ctl.stats.rx_frames;
	bad_tag = sim.tx.ctl.stats.rx_bad_tag;
	sim_inject(&sim, &sim.tx.ctl, frame, len);
	CHECK(sim.tx.ctl.stats.rx_bad_tag == bad_tag + 1U);
	CHECK(sim.tx.ctl.stats.rx_frames == counter);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);

	/* RX 收到自己发的 TLM：同理，不能当成 TX 的遥测收下 */
	counter = sim.rx.tlm_count;
	bad_tag = sim.rx.ctl.stats.rx_bad_tag;
	sim_inject(&sim, &sim.rx.ctl, sim.up, sim.up_len);
	CHECK(sim.rx.ctl.stats.rx_bad_tag == bad_tag + 1U);
	CHECK(sim.rx.tlm_count == counter);

	/* 让 TX 判一次失联、换一个会话 nonce，空口上那帧 SYNC 就"过期"了 */
	nonce = sim.tx.ctl.sess_nonce_t;
	sim.drop_up = 500;
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_SEARCHING &&
			  sim.tx.ctl.sess_nonce_t != nonce,
		  2000);
	sim.drop_up = 0;
	CHECK(sim.tx.ctl.sess_nonce_t != nonce);

	/* TX 收到自己发过的旧 SYNC：不能把会话换回旧 nonce 上 */
	CHECK(sim.sync_len > 0U);
	nonce = sim.tx.ctl.sess_nonce_t;
	bad_tag = sim.tx.ctl.stats.rx_bad_tag;
	sim_inject(&sim, &sim.tx.ctl, sim.sync, sim.sync_len);
	CHECK(sim.tx.ctl.stats.rx_bad_tag == bad_tag + 1U);
	CHECK(sim.tx.ctl.sess_nonce_t == nonce);
}

/*
 * 换绑（重新配对）要换掉整棵子树：pair_key 掺了配对时的两个 nonce，所以同步字、
 * 跳频表、tag 钥匙全换，旧配对录下来的帧再也验证不过。
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

	/* 再配对一次（换绑走的就是这条路） */
	zlrs_link_press(&sim.tx.ctl, sim.now_ms);
	zlrs_link_press(&sim.rx.ctl, sim.now_ms);
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_SEARCHING &&
			  sim.rx.ctl.state == ZLRS_STATE_SEARCHING,
		  2000);

	/* 配对材料换了，下层的同步字与 tag 钥匙跟着换 */
	CHECK(memcmp(sim.tx.ctl.pair_salt, salt_before, ZLRS_PAIR_SALT_LEN) != 0);
	CHECK(memcmp(sim.tx.ctl.link_auth_out, auth_before, ZLRS_KEY_LEN) != 0);
	/* 两端仍然算出同一套 */
	CHECK(memcmp(sim.tx.ctl.sync_word, sim.rx.ctl.sync_word, ZLRS_SYNC_WORD_LEN) == 0);
	CHECK(memcmp(sim.tx.ctl.link_auth_out, sim.rx.ctl.link_auth_in,
		     ZLRS_KEY_LEN) == 0);

	/* 旧配对录下的 SYNC：钥匙已经换了，必须被丢掉 */
	bad_tag = sim.rx.ctl.stats.rx_bad_tag;
	nonce = sim.rx.ctl.sess_nonce_t;
	sim_inject(&sim, &sim.rx.ctl, sync, sync_len);
	CHECK(sim.rx.ctl.stats.rx_bad_tag == bad_tag + 1U);
	CHECK(sim.rx.ctl.sess_nonce_t == nonce);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);
}

/*
 * 旧 SYNC 的重放（原 02 §5.4 那条弱点）：RX 只在收到用新会话钥匙打的确认后才
 * 采用会话，所以重放录下来的旧 SYNC 只能换来一条带全新 nonce_r 的响应——旧会话
 * 的钥匙一条都拿不到。同理，旧 SESS_RESP 打给 TX 也会因为 nonce_t 对不上被忽略。
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

	/* 录下第一段会话的 SYNC 与会话响应 */
	CHECK(sim.sync_len > 0U);
	CHECK(sim.sess_resp_len > 0U);
	sync_len = sim.sync_len;
	resp_len = sim.sess_resp_len;
	memcpy(sync, sim.sync, sync_len);
	memcpy(resp, sim.sess_resp, resp_len);

	/* 让两边都掉回 SEARCHING：这正是重放攻击的时机 */
	sim.drop_down = 500;
	sim.drop_up = 500;
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_SEARCHING &&
			  sim.rx.ctl.state == ZLRS_STATE_SEARCHING,
		  2000);
	sim.drop_down = 0;
	sim.drop_up = 0;
	CHECK(!sim.rx.ctl.session_valid);

	/* 重放旧 SYNC 给 RX：只能武装一个候选，不能采用 */
	nonce_r = sim.rx.ctl.sess_nonce_r;
	sim_inject(&sim, &sim.rx.ctl, sync, sync_len);
	CHECK(!sim.rx.ctl.session_valid);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_SEARCHING);
	CHECK(sim.rx.ctl.sess_nonce_r != nonce_r); /* 现抽的 nonce_r，不是旧会话那把 */
	CHECK(sim.rx.ctl.session_pending);

	/* 重放旧 SESS_RESP 给 TX：回带的 nonce_t 不是当前请求，必须被忽略 */
	memcpy(key_before, sim.tx.ctl.session_key_out, ZLRS_KEY_LEN);
	sim_inject(&sim, &sim.tx.ctl, resp, resp_len);
	CHECK(memcmp(sim.tx.ctl.session_key_out, key_before, ZLRS_KEY_LEN) == 0);

	/* 真的 TX 回来时照常建链 */
	sim.tx.ctl.last_sync_ms = 0U; /* 不等一个 SYNC 周期 */
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim.rx.ctl.state == ZLRS_STATE_CONNECTED,
		  4000);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.session_valid);
	CHECK(memcmp(sim.tx.ctl.session_key_out, sim.rx.ctl.session_key_in,
		     ZLRS_KEY_LEN) == 0);
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

	key = sim.rx.data.session_key_in; /* RX 校验 TX 发来的帧，用 in */
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

	/* 恢复投递：靠新的 SYNC 重新连上，不需要重新配对 */
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

	/* 对端收不到：此时 TX 只剩 SYNC 一种发送 */
	sim.drop_down = 100000;
	before = sim.tx.ctl.stats.tx_frames;
	sim_run(&sim, 3U * SIM_FRAMES_PER_SEC);
	sent = sim.tx.ctl.stats.tx_frames - before;
	CHECK(sent >= 3U && sent <= 5U);
}

/* 上行 LQ：按序号算应有帧数，丢包后百分比要掉下来 */
static void test_link_quality(void)
{
	sim_t sim;

	printf("link: uplink lq\n");

	sim_init(&sim);
	pair_and_connect(&sim);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* 一个窗口以上不丢包：LQ 应该是满的 */
	sim_run(&sim, 200U);
	CHECK(sim.rx.ctl.stats.lq_percent == 100U);

	/*
	 * 每 8 个超帧丢 1 个下行投递（连丢 20 个会触发失联重连，那是另一个用例），
	 * 160 个超帧里够跑满一个 LQ 窗口（128 帧）并结算
	 */
	for (uint8_t i = 0U; i < 20U; i++) {
		sim.drop_down = 1;
		sim_run(&sim, 8U);
	}
	CHECK(sim.rx.ctl.stats.lq_percent < 100U);
	CHECK(sim.rx.ctl.stats.lq_percent > 50U);

	/* 不再丢包：跑满一个窗口后 LQ 回到满值 */
	sim_run(&sim, 200U);
	CHECK(sim.rx.ctl.stats.lq_percent == 100U);
}

/* 透传背压：空口队列满了以后丢掉的字节要计数 */
static void test_backpressure(void)
{
	sim_t sim;
	uint8_t chunk[128];
	uint16_t accepted = 0U;

	printf("link: backpressure\n");

	sim_init(&sim);
	memset(chunk, 0x5AU, sizeof(chunk));

	/* 队列 512 字节：先填满，再多写的部分应该被计成丢弃 */
	for (uint8_t i = 0U; i < 4U; i++) {
		accepted = (uint16_t)(accepted + zlrs_link_write(&sim.tx.data, chunk,
								 sizeof(chunk)));
	}
	CHECK(accepted == ZLRS_LINK_TX_QUEUE_LEN);
	CHECK(sim.tx.data.stats.data_drop_bytes == 0U);

	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == 0U);
	CHECK(sim.tx.data.stats.data_drop_bytes == sizeof(chunk));

	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == 0U);
	CHECK(sim.tx.data.stats.data_drop_bytes == 2U * sizeof(chunk));
}

/* ARQ：丢帧靠重传补齐，接收端按数据序号去重 */
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

	/* 每 3 个数据帧丢 1 个：重传必须把 512 字节一个不差地补齐 */
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

/* ARQ：接收端收到重复的数据帧时只投递一次 */
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

/* ARQ：重传用尽后放弃这一段，并把它计成丢弃（不阻塞后面的数据） */
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

	/* 下行走不了：第一段（128 字节）重传用尽后必须放弃并计数，而不是永远卡住 */
	sim.drop_data_down = 100000;
	sim_run(&sim, 400U);

	CHECK(sim.tx.data.stats.tx_retries > 0U);
	CHECK(sim.tx.data.stats.tx_arq_drop >= 1U);
	CHECK(sim.tx.data.stats.data_drop_bytes >= 128U);
	CHECK(sim.tx.data.stats.data_drop_bytes < sizeof(chunk));

	/* 链路恢复：重同步握手之后，剩下的字节要接着送出去 */
	sim.drop_data_down = 0;
	SIM_UNTIL(&sim, sim.tx.data.queue_count == 0U, 4000);
	CHECK(sim.tx.data.queue_count == 0U);
	CHECK(sim.tx.data.stats.data_drop_bytes < sizeof(chunk));
	CHECK(sim.rx.stream_len > 0U);
	CHECK(sim.rx.stream_len <= sizeof(chunk));
	for (uint16_t i = 0U; i < sim.rx.stream_len; i++) {
		CHECK(sim.rx.stream[i] == 0x33U);
	}
}

/* 速率自适应：连续窗口 + 保持时间 + 滞回，三个条件缺一不可 */
static void test_rate_adapt(void)
{
	static const zlrs_rate_tier_t tiers[] = {
		{ .mod = { .modulation = ZLRS_MOD_FLRC, .rate_bps = 2600000U },
		  .min_lq_to_stay = 85U },
		{ .mod = { .modulation = ZLRS_MOD_FLRC, .rate_bps = 1300000U },
		  .min_lq_to_stay = 60U },
		{ .mod = { .modulation = ZLRS_MOD_FLRC, .rate_bps = 650000U },
		  .min_lq_to_stay = 35U },
		{ .mod = { .modulation = ZLRS_MOD_FLRC, .rate_bps = 325000U },
		  .min_lq_to_stay = 0U },
	};
	zlrs_rate_ctl_t ctl;
	uint8_t next = 0xFFU;
	uint32_t now = 1000U;

	printf("link: rate adaptation\n");

	zlrs_rate_init(&ctl, tiers, 4U, 1U, now); /* 从 1.3 Mbps 起 */
	CHECK(zlrs_rate_current_bps(&ctl) == 1300000U);

	/* 保持时间内：窗口计数都不算，直接不动 */
	CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(zlrs_rate_current_bps(&ctl) == 1300000U);

	/* 保持时间过了：连续 3 个"差"窗口才降一档（更慢） */
	now += 5000U;
	CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(next == 2U);
	CHECK(zlrs_rate_current_bps(&ctl) == 650000U);

	/* 换档后保持时间内不再动 */
	now += 100U;
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	}
	CHECK(zlrs_rate_current_bps(&ctl) == 650000U);

	/* 保持时间过了，连续三个差窗口继续降 */
	now += 5000U;
	CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(!zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(zlrs_rate_update(&ctl, 20U, now, &next));
	CHECK(next == 3U);
	CHECK(zlrs_rate_current_bps(&ctl) == 325000U);

	/* 最慢一档：再差也不动了 */
	now += 5000U;
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(!zlrs_rate_update(&ctl, 0U, now, &next));
	}
	CHECK(zlrs_rate_current_bps(&ctl) == 325000U);

	/* 升档要留滞回余量：升到 0.65M 档要 35+10=45，40% 不够，也不能降 */
	now += 5000U;
	for (uint8_t i = 0U; i < 5U; i++) {
		CHECK(!zlrs_rate_update(&ctl, 40U, now, &next));
	}
	CHECK(zlrs_rate_current_bps(&ctl) == 325000U);

	/* 75% ≥ 当前档-1 的门槛 60 + 滞回 10：连续三个窗口后升档 */
	now += 5000U;
	CHECK(!zlrs_rate_update(&ctl, 75U, now, &next));
	CHECK(!zlrs_rate_update(&ctl, 75U, now, &next));
	CHECK(zlrs_rate_update(&ctl, 75U, now, &next));
	CHECK(next == 2U);
	CHECK(zlrs_rate_current_bps(&ctl) == 650000U);
}

/* 失控值由 TX 配置、配对时下发：RX 收到并存下来，用于自己的失控输出 */
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

	/* 链路层存下来了 */
	CHECK(sim.rx.ctl.failsafe_len == sizeof(want));
	CHECK(memcmp(sim.rx.ctl.failsafe, want, sizeof(want)) == 0);
	/* 应用侧的回调也收到了（RX 要写 NVS 并用于输出） */
	CHECK(sim.rx.failsafe_count == 1U);
	CHECK(sim.rx.failsafe_len == sizeof(want));
	CHECK(memcmp(sim.rx.failsafe, want, sizeof(want)) == 0);

	/* 长度不对的值不接收 */
	zlrs_link_set_failsafe(&sim.tx.ctl, want, sizeof(want) - 1U);
	CHECK(sim.tx.ctl.failsafe_len == sizeof(want));

	/* TX 没设过值时 RX 保持自己那份，不会被一串 0 覆盖 */
	sim_init(&sim);
	pair_and_connect(&sim);
	CHECK(sim.rx.ctl.failsafe_len == 0U);
	CHECK(sim.rx.failsafe_count == 0U);
}

/*
 * 数据链路是尽力而为：空闲时不发空帧，也不判失联（见 doc/09-protocol.md 3.7）。
 * 所以空闲再久也不能反复重建会话——否则一有数据要发就先撞上握手，头几帧白等一个
 * SYNC 周期。
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
	sim_run(&sim, 3000U); /* 约 18 s 空闲 */
	CHECK(sim.tx.data.sess_nonce_t == nonce_before);
	CHECK(sim.tx.data.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.data.state == ZLRS_STATE_CONNECTED);

	/* 空闲之后照样能透传 */
	memset(chunk, 0x44U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/* 控制帧率拉高后的节奏稀疏化：遥测每 4 帧、数据窗口每 5 帧，两端仍同步 */
static void test_cadence(void)
{
	sim_t sim;
	uint8_t chunk[32];
	uint32_t tlm_before;
	uint32_t tlm_after;

	printf("link: cadence (sparse tlm / data)\n");

	sim_init(&sim);
	/* 两端必须配一样的节奏：同固件、同配置 */
	sim.tx.ctl.cfg.tlm_every_n = 4U;
	sim.rx.ctl.cfg.tlm_every_n = 4U;
	sim.tx.data.cfg.data_every_n = 5U;
	sim.rx.data.cfg.data_every_n = 5U;

	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* 给 RX 一份遥测载荷：空载荷是保活帧，不会触发 on_tlm */
	{
		uint8_t tlm[8] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U };

		zlrs_link_set_tlm(&sim.rx.ctl, tlm, sizeof(tlm));
	}

	/* 跑 100 个超帧：遥测应该只有约 25 次，而不是 100 次 */
	tlm_before = sim.tx.tlm_count;
	sim_run(&sim, 100U);
	tlm_after = sim.tx.tlm_count - tlm_before;
	CHECK(tlm_after >= 20U && tlm_after <= 30U);

	/* 跳频仍然锁步 */
	CHECK(sim.tx.ctl.fhss.index == sim.rx.ctl.fhss.index);
	CHECK(sim.tx.data.fhss.index == sim.rx.data.fhss.index);

	/* 数据窗口稀疏了但照样通 */
	memset(chunk, 0x5AU, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/*
 * 上板调试档之零：P0——帧不带 tag。先只确认收发时序与帧格式，把密钥路径整段
 * 排除掉（代价是任何人都能注入，只能台架用）。
 */
static void test_bringup_no_tag(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t chunk[16];

	printf("link: bring-up without tag (P0)\n");

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

	/* 空口上的帧就是 header + 载荷，没有 tag */
	sim_slot(&sim, ZLRS_SLOT_CTL_DOWN);
	CHECK(sim.down_len == (uint16_t)(ZLRS_FRAME_HDR_LEN + sizeof(rc)));

	/* 透传照常 */
	memset(chunk, 0x62U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

/*
 * 上板调试档之一：关掉逐包跳频。所有帧都落在同步信道上（跳频表的第 0 个位置），
 * 收发时序与跳频表可以分开看——这一档只应该影响用哪个频点，不影响别的行为。
 */
static void test_bringup_fixed_channel(void)
{
	sim_features_t features = sim_features_default;
	const zlrs_fhss_domain_t *domain = &zlrs_fhss_domains[ZLRS_DOMAIN_FCC915];
	const uint32_t sync_hz =
		domain->start_hz + (uint32_t)(domain->channel_count / 2U) * domain->step_hz;
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t chunk[16];

	printf("link: bring-up without hopping\n");

	features.hopping = 0U;
	sim_init_with(&sim, &features);
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	memset(rc, 0x31U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	sim_run(&sim, 300U);

	/* 索引一直停在 0（= 同步信道），两端都一样 */
	CHECK(sim.tx.ctl.fhss.index == 0U);
	CHECK(sim.rx.ctl.fhss.index == 0U);
	CHECK(zlrs_fhss_current_hz(&sim.tx.ctl.fhss) == sync_hz);
	CHECK(zlrs_fhss_current_hz(&sim.rx.ctl.fhss) == sync_hz);

	/* 遥控与透传照常 */
	CHECK(sim.rx.rc_count > 0U);
	CHECK(memcmp(sim.rx.rc, rc, sizeof(rc)) == 0);
	memset(chunk, 0x32U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
	CHECK(sim.rx.ctl.stats.rx_bad_tag == 0U);
}

/*
 * 上板调试档之二：关掉三次握手。配对级的 link_auth 直接当会话钥匙，RX 收到
 * SYNC 就算建立，再用 SESS_RESP 反复"报到"直到听见对端的第一帧。
 * 报到帧丢掉几次也要能连上——这是这一档唯一的新时序。
 */
static void test_bringup_no_handshake(void)
{
	sim_features_t features = sim_features_default;
	sim_t sim;
	uint8_t chunk[16];
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];

	printf("link: bring-up without session handshake\n");

	features.handshake = 0U;
	sim_init_with(&sim, &features);

	/* 报到帧丢两帧：RX 必须继续报到，直到 TX 听见 */
	sim.drop_up = 2;
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.tx.data.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.data.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.sess_resp_len > 0U); /* 用的是 SESS_RESP，不是 SESS_CFM */

	/* 会话钥匙就是配对级钥匙：没有逐会话的 nonce 派生 */
	CHECK(memcmp(sim.tx.ctl.session_key_out, sim.tx.ctl.link_auth_out, ZLRS_KEY_LEN) ==
	      0);
	CHECK(memcmp(sim.tx.ctl.session_key_in, sim.rx.ctl.link_auth_out, ZLRS_KEY_LEN) ==
	      0);
	CHECK(!sim.rx.ctl.session_pending);
	CHECK(!sim.tx.ctl.sess_cfm_due);

	/* 遥控、透传照常 */
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
 * 上板调试档之三：关掉上行遥测。控制链路退化成"只下行 RC"：TX 不再开遥测接收
 * 窗口（失联判定由 App 一并关掉，见 doc/12-debug.md 第 3 节），RX 也不再发。
 */
static void test_bringup_no_telemetry(void)
{
	sim_features_t features = sim_features_default;
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];

	printf("link: bring-up without telemetry\n");

	features.telemetry = 0U;
	sim_init_with(&sim, &features);
	pair_and_connect(&sim);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);

	/* App 侧在关掉遥测时把 TX 的失联判定一并关掉：没东西可听 */
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
 * 连接态周期 SYNC（doc/09-protocol.md 5.1）：两端晶振差会让跳频索引慢慢错开，
 * RX 踩空之后靠这个帧把索引拉回来。这里直接把索引挪开 4 格来模拟漂移。
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

	/* 制造漂移：RX 的索引偏了 4 格，接下来的帧全都对不上频点 */
	sim.rx.ctl.fhss.index = (uint16_t)((sim.rx.ctl.fhss.index + 4U) %
					   sim.rx.ctl.fhss.sequence_count);
	CHECK(sim.rx.ctl.fhss.index != sim.tx.ctl.fhss.index);

	resync_before = sim.rx.ctl.stats.hop_resync;
	SIM_UNTIL(&sim, sim.rx.ctl.fhss.index == sim.tx.ctl.fhss.index, 200);
	CHECK(sim.rx.ctl.fhss.index == sim.tx.ctl.fhss.index);
	CHECK(sim.rx.ctl.stats.hop_resync > resync_before);
	CHECK(sim.rx.ctl.stats.rx_syncs > 0U);

	/* 拉回索引之后照常收发 */
	memset(chunk, 0x82U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
	CHECK(sim.rx.ctl.fhss.index == sim.tx.ctl.fhss.index);
	CHECK(sim.rx.data.fhss.index == sim.tx.data.fhss.index);
}

/*
 * 子带混合比（doc/09-protocol.md 5.1）：TX 单方面推进，RX 在处理 SYNC 时采用；
 * 太久没听到新版 SYNC 就退回默认比例。
 */
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

	/* TX 推进到 50%，RX 从下一条 SYNC 起采用 */
	zlrs_link_set_mix(&sim.tx.ctl, 2U);
	CHECK(sim.tx.ctl.sub_mix == 2U);
	changes_before = sim.rx.ctl.stats.mix_changes;
	memset(rc, 0x81U, sizeof(rc));
	zlrs_link_set_rc(&sim.tx.ctl, rc, sizeof(rc));
	SIM_UNTIL(&sim, sim.rx.ctl.sub_mix == 2U, 400);
	CHECK(sim.rx.ctl.sub_mix == 2U);
	CHECK(sim.rx.ctl.stats.mix_changes > changes_before);

	/* RX 侧改默认值只是回退目标，不直接改当前比例 */
	zlrs_link_set_mix(&sim.rx.ctl, 0U);
	CHECK(sim.rx.ctl.sub_mix == 2U);
	CHECK(sim.rx.ctl.sub_mix_default == 0U);

	/* 掐掉 TX 的 SYNC（模拟只改了 TX 一侧）：RX 在 3 个 SYNC 周期后回退 */
	sim.tx.ctl.cfg.resync_every_n = 0U;
	sim.tx.ctl.sub_mix = 3U;
	sim.rx.ctl.sub_mix_seen_ms = sim.now_ms;
	SIM_UNTIL(&sim, sim.rx.ctl.sub_mix == sim.rx.ctl.sub_mix_default, 4000);
	CHECK(sim.rx.ctl.sub_mix == 0U);
	CHECK(sim.rx.ctl.stats.mix_fallbacks > 0U);
}

/*
 * 对端重启后的自动重连（doc/09-protocol.md 3.5 那条已知限制的解法）：RX 重启
 * 后链路状态全丢，只剩 NVS 里的配对记录；TX 这边还在跑，周期 SYNC 会让它重新
 * 握手。两条链路都要能自己回来。
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

	/* RX 重启：链路状态全丢，只留下"配对记录"（dev_id + 配对 nonce） */
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
	/* 重连之后钥匙仍然配套 */
	CHECK(memcmp(sim.tx.ctl.session_key_out, sim.rx.ctl.session_key_in, ZLRS_KEY_LEN) ==
	      0);

	/* 遥控与透传照常 */
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
 * 重放的旧 SYNC 不能动会话：tag 是配对级的，录下来的旧 SYNC 能验过，
 * 所以"换会话"这条路要求最近完全听不到对端，同会话那条要求序号更新。
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

	/* 录下当前会话的 SYNC，等 TX 换过会话之后再放回去 */
	CHECK(sim.sync_len > 0U);
	sync_len = sim.sync_len;
	memcpy(sync, sim.sync, sync_len);

	/* 掐掉上行，让 TX 判一次失联、换一个 nonce，再重连 */
	sim.drop_up = 1000000;
	SIM_UNTIL(&sim, sim.tx.ctl.state == ZLRS_STATE_SEARCHING, 2000);
	sim.drop_up = 0;
	CHECK(sim.tx.ctl.state == ZLRS_STATE_SEARCHING);
	SIM_UNTIL(&sim,
		  sim.tx.ctl.state == ZLRS_STATE_CONNECTED &&
			  sim.rx.ctl.state == ZLRS_STATE_CONNECTED,
		  4000);
	CHECK(sim.tx.ctl.state == ZLRS_STATE_CONNECTED);

	/* 现在这帧的 nonce 是上一段会话的 */
	nonce_before = sim.rx.ctl.sess_nonce_t;
	memcpy(key_before, sim.rx.ctl.session_key_in, ZLRS_KEY_LEN);
	sync_nonce = (uint32_t)sync[7] | ((uint32_t)sync[8] << 8) |
		       ((uint32_t)sync[9] << 16) | ((uint32_t)sync[10] << 24);
	CHECK(sync_nonce != nonce_before);

	/* 会话还在正常跑：旧 SYNC 只能被丢掉 */
	sim_inject(&sim, &sim.rx.ctl, sync, sync_len);
	CHECK(sim.rx.ctl.state == ZLRS_STATE_CONNECTED);
	CHECK(sim.rx.ctl.sess_nonce_t == nonce_before);
	CHECK(memcmp(sim.rx.ctl.session_key_in, key_before, ZLRS_KEY_LEN) == 0);
}

/*
 * 调制按链路各自选（doc/07-architecture.md 第 3 节）：控制链路跑 LoRa 远距档，
 * 数据链路留在 2.4G 跑 FLRC。LoRa 的同步字是另派生的 1 字节（最高位当 private
 * bit），不能沿用 FLRC 那 4 字节。
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

	/* 派生的 1 字节同步字：两端一致，带上 private bit，控制与数据域分离 */
	CHECK(sim.tx.ctl.lora_sync == sim.rx.ctl.lora_sync);
	CHECK((sim.tx.ctl.lora_sync & 0x80U) != 0U);
	CHECK(sim.tx.ctl.lora_sync != sim.tx.data.lora_sync);
	CHECK(sim.tx.ctl.cfg.mod.sync_byte == sim.tx.ctl.lora_sync);
	/* LoRa 档没有"速率"这个字段 */
	CHECK(sim.tx.ctl.cfg.mod.rate_bps == 0U);
	/* 数据链路没被带着改调制 */
	CHECK(sim.tx.data.cfg.mod.modulation == (uint8_t)ZLRS_MOD_FLRC);

	/* 遥控、遥测与透传照常 */
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
 * 1000 Hz 的协议侧（doc/09-protocol.md 4.1）：帧周期压到 1 ms 时，超帧、跳频
 * 锁步、遥测/数据窗口的稀疏化、连接态重同步都不能乱。射频侧要直调驱动才能真的
 * 跑到 1000 Hz（RAC 的排期粒度是毫秒），这里只钉协议与时间常数。
 */
static void test_high_frame_rate(void)
{
	sim_t sim;
	uint8_t rc[ZLRS_RC_PAYLOAD_LEN];
	uint8_t tlm[8];
	uint8_t chunk[8];

	printf("link: 1000 Hz frame cadence\n");

	sim_init(&sim);
	sim.frame_ms = 1U; /* 一帧 1 ms */
	/* 1 ms 一帧只有 2.6 Mbps 档装得下；节奏系数就是计划算出来的那两个 */
	sim.tx.ctl.cfg.mod = zlrs_mod_flrc(2600000U);
	sim.rx.ctl.cfg.mod = zlrs_mod_flrc(2600000U);
	sim.tx.ctl.cfg.tlm_every_n = 6U; /* ≈167 次/秒 */
	sim.rx.ctl.cfg.tlm_every_n = 6U;
	sim.tx.data.cfg.data_every_n = 10U; /* =100 次/秒 */
	sim.rx.data.cfg.data_every_n = 10U;
	sim.tx.ctl.cfg.resync_every_n = 100U; /* 100 ms 一次重同步 */
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

	/* 300 ms 里 2.6 Mbps 那一档几乎不该丢帧 */
	CHECK(sim.rx.rc_count > 250U);
	CHECK(memcmp(sim.rx.rc, rc, sizeof(rc)) == 0);
	CHECK(sim.tx.tlm_count > 30U);
	/* 跳频仍然锁步，重同步帧也按 100 帧的节奏出现过 */
	CHECK(sim.tx.ctl.fhss.index == sim.rx.ctl.fhss.index);
	CHECK(sim.rx.ctl.stats.rx_syncs > 0U);
	/* 数据窗口稀疏到每 10 帧一次，但透传照常 */
	memset(chunk, 0xD3U, sizeof(chunk));
	CHECK(zlrs_link_write(&sim.tx.data, chunk, sizeof(chunk)) == sizeof(chunk));
	SIM_UNTIL(&sim, sim.rx.stream_len >= sizeof(chunk), 400);
	CHECK(sim.rx.stream_len == sizeof(chunk));
}

void test_link(void)
{
	test_pairing();
	test_session();
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
	test_rx_rejects();
	test_frag_drop();
	test_recovery();
	test_hop_lockstep();
	test_unconfigured();
	test_sync_rate();
	test_link_quality();
	test_backpressure();
	test_arq_retransmit();
	test_arq_dedup();
	test_arq_giveup();
	test_rate_adapt();
	test_failsafe_push();
	test_data_idle();
	test_cadence();
	test_bringup_no_tag();
	test_bringup_fixed_channel();
	test_bringup_no_handshake();
	test_bringup_no_telemetry();
	test_hop_resync();
	test_sub_mix();
	test_peer_restart();
	test_sync_replay_guard();
	test_lora_control();
	test_high_frame_rate();
}

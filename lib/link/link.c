/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/link/link.h"

#include <string.h>

/* 配对模式下使用的公开同步字，槽 3 */
static const uint8_t pairing_sync_word[ZLRS_SYNC_WORD_LEN] = { 'Z', 'L', 'R', 'S' };

/* 派生标签 */
#define LABEL_PAIR "zlrs/pair"
#define LABEL_AUTH "zlrs/auth"
#define LABEL_FHSS "zlrs/fhss"
#define LABEL_SYNC_CONTROL "zlrs/sync/control"
#define LABEL_SYNC_DATA "zlrs/sync/data"
#define LABEL_SYNC_LORA_CONTROL "zlrs/sync/lora/control"
#define LABEL_SYNC_LORA_DATA "zlrs/sync/lora/data"
#define LABEL_LINK_AUTH "zlrs/link-auth"
#define LABEL_SESSION "zlrs/session"

/* 配对帧载荷长度：dev_a(4) + dev_b(4) + nonce_a(4) + nonce_b(4) */
/*
 * 配对载荷：两侧 dev_id(8) + 两个 nonce(8) + 标志(1) + 失控值(22)。
 * 失控值只有发起方（TX）填：设过就置 FLAG_FAILSAFE，响应方照单存下来；
 * 没设过就不置位，响应方保持自己那份，免得被一串 0 覆盖。
 */
#define PAIR_PAYLOAD_LEN (17U + ZLRS_RC_PAYLOAD_LEN)
#define PAIR_FLAGS_OFFSET 16U
#define PAIR_FAILSAFE_OFFSET 17U
#define PAIR_FLAG_FAILSAFE 0x01U

static void derive_session_keys(zlrs_link_t *link, uint32_t nonce_t, uint32_t nonce_r);
static void session_activate(zlrs_link_t *link, uint16_t hop_index);
static void roll_session(zlrs_link_t *link, uint32_t now_ms);
static void lq_account(zlrs_link_t *link, uint16_t seq);

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
	if (link->radio.random != NULL) {
		return link->radio.random(link->radio.ctx);
	}

	/* 没有随机源时用时间与序号凑一个非零值，仅够跑通流程 */
	return (salt * 2654435761U) ^ 0x9e3779b9U;
}

/*
 * 同步信道频率。配对时还没派生密钥、跳频表还没生成，所以直接查频段表；
 * 这个频点是配对与 BEACON 的固定落脚点。
 */
static uint32_t sync_freq_hz(const zlrs_link_t *link)
{
	const zlrs_fhss_domain_t *domain = &zlrs_fhss_domains[link->cfg.domain];
	const uint8_t channel = (uint8_t)(domain->channel_count / 2U);

	return domain->start_hz + (uint32_t)channel * domain->step_hz;
}

/*
 * 这一帧是不是"重同步帧"（见 doc/09-protocol.md 5.1）：连接态下每
 * cfg.resync_every_n 帧来一次，整帧都停在同步信道上——下行发 BEACON，上行让
 * 对端有机会回 SESS_RESP。两端用同一个 frame_index 推，节奏天然一致。
 */
static bool beacon_frame(const zlrs_link_t *link)
{
	return link->cfg.resync_every_n > 0U &&
	       (link->frame_index % link->cfg.resync_every_n) == 0U;
}

/*
 * 这一帧该用哪个频点：重同步帧与"会话确认还欠着"的帧都退回同步信道，
 * 其余按跳频表。
 */
static uint32_t slot_freq_hz(const zlrs_link_t *link)
{
	return (beacon_frame(link) || link->sess_cfm_due)
		       ? sync_freq_hz(link)
		       : zlrs_fhss_current_hz(&link->fhss);
}

static void set_state(zlrs_link_t *link, uint8_t state, uint32_t now_ms)
{
	if (link->state == state) {
		return;
	}

	link->state = state;
	link->state_ms = now_ms;
	/* 简化握手的"我在"只在 CONNECTED 有意义 */
	if (state != ZLRS_STATE_CONNECTED) {
		link->announce = false;
	}
	if (link->ev.on_state != NULL) {
		link->ev.on_state(link->ev.ctx, state);
	}

	/* TX 是链路的权威方：每次回到 SEARCHING 就换一个会话 nonce */
	if (state == ZLRS_STATE_SEARCHING && link->cfg.role == ZLRS_ROLE_TX) {
		roll_session(link, now_ms);
		/* 立刻发一次 BEACON，不等一个周期，缩短重连时间 */
		link->last_beacon_ms = (uint32_t)(now_ms - link->cfg.beacon_period_ms);
	}
}

/*
 * TX 侧每次进入 SEARCHING 抽一个新的 nonce_t，并丢掉上一次的会话。会话钥匙要等
 * RX 回了 nonce_r 才算得出来（见 doc/09-protocol.md 3.5）。
 */
static void roll_session(zlrs_link_t *link, uint32_t now_ms)
{
	link->sess_nonce_t = link_random(link, now_ms);
	link->session_valid = false;
	link->session_pending = false;
	link->sess_cfm_due = false;
	link->sess_resp_due = false;
	link->announce = false;
}

/* 本机与对端 dev_id 按数值排序后拼接，保证两端算出同一个 pair_key */
static void ordered_ids(const zlrs_link_t *link, uint8_t out[2U * ZLRS_DEV_ID_LEN])
{
	const uint8_t *a = link->dev_id;
	const uint8_t *b = link->peer_id;
	bool swap = false;

	for (uint8_t i = 0; i < ZLRS_DEV_ID_LEN && !swap; i++) {
		if (a[i] != b[i]) {
			swap = a[i] > b[i];
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

/* 配对的两个 nonce 同样按数值排序：谁发起都不影响派生结果 */
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
	zlrs_kdf(link->root_key, ZLRS_KEY_LEN, LABEL_AUTH, NULL, 0U, link->auth_key,
		 ZLRS_KEY_LEN);
}

/*
 * 钥匙按方向分：KDF(pair_key, label, ctx || 本机 || 对端) 用于发出去的帧，两个 id
 * 调过来用于校验对端的帧——顺序本身编码方向，两端各算一次即可配对。
 * 不区分方向的话，把自己发的帧原样打回来就会被当成对端的帧收下（反射攻击）。
 */
static void derive_by_endpoints(const zlrs_link_t *link, const char *label,
				const uint8_t *ctx, size_t ctx_len,
				uint8_t out[ZLRS_KEY_LEN], uint8_t in[ZLRS_KEY_LEN])
{
	uint8_t ids_ctx[2U * sizeof(uint32_t) + 2U * ZLRS_DEV_ID_LEN];
	const size_t len = ctx_len + 2U * ZLRS_DEV_ID_LEN;

	if (len > sizeof(ids_ctx)) {
		return;
	}
	if (ctx_len > 0U) {
		memcpy(ids_ctx, ctx, ctx_len);
	}

	/* 本机发：本机在前 */
	memcpy(&ids_ctx[ctx_len], link->dev_id, ZLRS_DEV_ID_LEN);
	memcpy(&ids_ctx[ctx_len + ZLRS_DEV_ID_LEN], link->peer_id, ZLRS_DEV_ID_LEN);
	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, label, ids_ctx, len, out, ZLRS_KEY_LEN);

	/* 对端发：对端在前 */
	memcpy(&ids_ctx[ctx_len], link->peer_id, ZLRS_DEV_ID_LEN);
	memcpy(&ids_ctx[ctx_len + ZLRS_DEV_ID_LEN], link->dev_id, ZLRS_DEV_ID_LEN);
	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, label, ids_ctx, len, in, ZLRS_KEY_LEN);
}

static void derive_pair(zlrs_link_t *link)
{
	uint8_t ctx[2U * ZLRS_DEV_ID_LEN + ZLRS_PAIR_SALT_LEN];
	uint8_t seed[4];
	uint8_t ctrl_word[ZLRS_SYNC_WORD_LEN];
	uint8_t data_word[ZLRS_SYNC_WORD_LEN];
	uint32_t hop_seed;

	/*
	 * 双方 id 与配对时的两个 nonce，排序后拼接：两端算出同一个 pair_key，
	 * 而换一次配对就换一份 nonce，等于换掉下面整棵子树（见 doc/10-security.md
	 * 第 3 节）。
	 */
	ordered_ids(link, ctx);
	ordered_pair_salt(link, &ctx[2U * ZLRS_DEV_ID_LEN]);
	zlrs_kdf(link->root_key, ZLRS_KEY_LEN, LABEL_PAIR, ctx, sizeof(ctx), link->pair_key,
		 ZLRS_KEY_LEN);
	derive_by_endpoints(link, LABEL_LINK_AUTH, NULL, 0U, link->link_auth_out,
			    link->link_auth_in);

	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, LABEL_FHSS, NULL, 0U, seed, sizeof(seed));
	hop_seed = rd32(seed);
	zlrs_fhss_init(&link->fhss, link->cfg.domain, hop_seed);

	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, LABEL_SYNC_CONTROL, NULL, 0U, ctrl_word,
		 sizeof(ctrl_word));
	zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, LABEL_SYNC_DATA, NULL, 0U, data_word,
		 sizeof(data_word));

	memcpy(link->sync_word,
	       (link->cfg.kind == ZLRS_LINK_CONTROL) ? ctrl_word : data_word,
	       ZLRS_SYNC_WORD_LEN);

	/*
	 * LoRa 的同步字是 1 字节 + private bit，不能沿用上面那 4 字节，所以另开
	 * 一条派生标签（还是按角色命名，不按频段）。最高位当 private bit，用来
	 * 区分"我们这一组"与公开的同步字。见 doc/09-protocol.md 5.3。
	 */
	{
		const char *label = (link->cfg.kind == ZLRS_LINK_CONTROL)
					    ? LABEL_SYNC_LORA_CONTROL
					    : LABEL_SYNC_LORA_DATA;
		uint8_t byte = 0U;

		zlrs_kdf(link->pair_key, ZLRS_KEY_LEN, label, NULL, 0U, &byte,
			 sizeof(byte));
		link->lora_sync = (uint8_t)(byte | 0x80U);
		if (link->cfg.mod.modulation == (uint8_t)ZLRS_MOD_LORA) {
			link->cfg.mod.sync_byte = link->lora_sync;
		}
	}

	link->session_valid = false;
	link->session_pending = false;
	link->sess_resp_due = false;
	link->sess_cfm_due = false;
	/*
	 * 简化握手（只在上板调试时关）：不派生会话钥匙，配对级的 link_auth 直接
	 * 当会话钥匙用，BEACON 之后只回一条 SESS_RESP，见 doc/10-security.md 第 3 节。
	 */
	if (link->cfg.handshake == 0U) {
		memcpy(link->session_key_out, link->link_auth_out, ZLRS_KEY_LEN);
		memcpy(link->session_key_in, link->link_auth_in, ZLRS_KEY_LEN);
		link->session_valid = true;
	}
	link->last_rx_seq = 0xFFFFU;
	link->fhss.index = 0U;
}

/* 会话钥匙 = KDF(pair_key, "zlrs/session", nonce_t || nonce_r || 双方 id) */
static void derive_session_keys(zlrs_link_t *link, uint32_t nonce_t, uint32_t nonce_r)
{
	uint8_t ctx[2U * sizeof(uint32_t)];

	wr32(&ctx[0], nonce_t);
	wr32(&ctx[4], nonce_r);
	derive_by_endpoints(link, LABEL_SESSION, ctx, sizeof(ctx), link->session_key_out,
			    link->session_key_in);
}

/*
 * 采用这次会话：钥匙即刻生效，序号、ARQ 与 LQ 窗口都从头开始（上一段会话在飞的
 * 数据作废）。TX 发出 SESS_CFM 前调用，RX 验过 SESS_CFM 后调用。
 */
static void session_activate(zlrs_link_t *link, uint16_t hop_index)
{
	link->session_valid = true;
	link->session_pending = false;
	link->sess_resp_due = false;
	link->last_rx_seq = 0xFFFFU;
	link->tx_seq = 0U;
	/* 新会话的序号从头开始，LQ 窗口不能跨会话累计 */
	link->lq_last_seq = 0U;
	link->stats.lq_rx = 0U;
	link->stats.lq_expected = 0U;
	/* ARQ 窗口也是会话状态：在飞的帧作废，字节留在队列里重发 */
	link->arq_used = 0U;
	link->arq_retries = 0U;
	link->arq_seq = 0U;
	link->arq_rx_started = false;
	link->arq_ack_pending = false;
	zlrs_fhss_set_index(&link->fhss, hop_index);
}

static uint16_t build_frame(zlrs_link_t *link, uint8_t type, uint8_t flags,
			    const uint8_t *payload, uint16_t len, const uint8_t *tag_key)
{
	const uint8_t tag_len = zlrs_profile_tag_len(link->cfg.profile);
	zlrs_frame_t frame;
	uint16_t total;

	if (link->cfg.profile == ZLRS_PROFILE_P2) {
		return 0U; /* P2 的载荷加密不在链路层做，配置成 P2 就不发包 */
	}
	if (len > ZLRS_PAYLOAD_MAX_LEN) {
		return 0U;
	}

	total = zlrs_frame_len(len, tag_len);
	if (total > sizeof(link->tx_buf)) {
		return 0U;
	}

	frame.type = type;
	frame.flags = flags;
	frame.seq = link->tx_seq;
	frame.payload = payload;
	frame.payload_len = len;
	frame.tag = NULL;

	/* encode 只写 header + payload，tag 由这里补在后面 */
	if (zlrs_frame_encode(&frame, link->tx_buf, sizeof(link->tx_buf)) !=
	    (uint16_t)(ZLRS_FRAME_HDR_LEN + len)) {
		return 0U;
	}

	if (tag_len > 0U) {
		const uint16_t mac_len = (uint16_t)(ZLRS_FRAME_HDR_LEN + len);

		zlrs_tag(tag_key, ZLRS_KEY_LEN, link->tx_buf, mac_len,
			 &link->tx_buf[mac_len]);
	}

	link->tx_seq++;

	return total;
}

/* 取一帧的 tag 钥匙：outbound 用 out，否则用 in。配对握手是例外——它靠角色门禁
 * 与类型标签分方向，两端同一把 auth_key（见 doc/10-security.md 第 4 节）。 */
static const uint8_t *tag_key_for(const zlrs_link_t *link, uint8_t type, bool outbound)
{
	const uint8_t *link_auth = outbound ? link->link_auth_out : link->link_auth_in;
	const uint8_t *session = outbound ? link->session_key_out : link->session_key_in;

	switch (type) {
	case ZLRS_PKT_PAIR_REQ:
	case ZLRS_PKT_PAIR_RESP:
	case ZLRS_PKT_PAIR_CFM:
		return link->auth_key;
	case ZLRS_PKT_BEACON:
	/* 会话响应要在会话钥匙生效前就能验，用配对级钥匙 */
	case ZLRS_PKT_SESS_RESP:
		return link_auth;
	/* 确认帧必须用它确认的那把会话钥匙打 tag */
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
	link->req.max_rx_size = is_tx ? 0U : (uint16_t)sizeof(link->rx_buf);
	link->req.timeout_ms = timeout_ms;
	link->req.buf = is_tx ? link->tx_buf : link->rx_buf;
	link->req_is_tx = is_tx;
	link->req_pending = true;

	return &link->req;
}

static const zlrs_radio_req_t *submit_tx(zlrs_link_t *link, uint16_t len,
					 uint32_t freq_hz, uint8_t sync_slot,
					 const uint8_t sync_word[ZLRS_SYNC_WORD_LEN])
{
	if (len == 0U) {
		return NULL;
	}

	return make_req(link, true, len, freq_hz, sync_slot, sync_word, 0U);
}

static const zlrs_radio_req_t *submit_rx(zlrs_link_t *link, uint32_t freq_hz,
					 uint8_t sync_slot,
					 const uint8_t sync_word[ZLRS_SYNC_WORD_LEN],
					 uint16_t timeout_ms)
{
	return make_req(link, false, 0U, freq_hz, sync_slot, sync_word, timeout_ms);
}

/* ---- 发送：各帧类型 ------------------------------------------------------ */

static const zlrs_radio_req_t *send_beacon(zlrs_link_t *link, uint32_t now_ms)
{
	uint8_t payload[8];
	uint16_t len;

	payload[0] = ZLRS_PROTO_VERSION;
	payload[1] = (uint8_t)link->cfg.profile;
	payload[2] = (uint8_t)(link->fhss.index & 0xFFU);
	payload[3] = (uint8_t)(link->fhss.index >> 8);
	wr32(&payload[4], link->sess_nonce_t);

	len = build_frame(link, ZLRS_PKT_BEACON, 0U, payload, sizeof(payload),
			  tag_key_for(link, ZLRS_PKT_BEACON, true));
	link->last_beacon_ms = now_ms;

	/* BEACON 固定发在同步信道 */
	return submit_tx(link, len, sync_freq_hz(link), link->cfg.sync_slot,
			 link->sync_word);
}

/*
 * RX 对 BEACON 的应答：报 nonce_r、回带 nonce_t，用配对级钥匙认证（会话钥匙还没
 * 生效）。简化握手（handshake=0）下它同时是"我在"报到帧：nonce_r 填 0。
 */
static const zlrs_radio_req_t *send_sess_resp(zlrs_link_t *link)
{
	uint8_t payload[2U * sizeof(uint32_t)];
	uint16_t len;

	wr32(&payload[0], link->sess_nonce_t);
	wr32(&payload[4], link->sess_nonce_r);
	len = build_frame(link, ZLRS_PKT_SESS_RESP, 0U, payload, sizeof(payload),
			  tag_key_for(link, ZLRS_PKT_SESS_RESP, true));
	link->sess_resp_due = false;

	/* 仍发在同步信道：TX 还在 SEARCHING，正在那里等回应 */
	return submit_tx(link, len, sync_freq_hz(link), link->cfg.sync_slot,
			 link->sync_word);
}

/*
 * TX 的会话确认：用刚定下来的会话钥匙打 tag、回带 nonce_r，等于告诉 RX
 * "我算出了同一把钥匙"。丢了不要紧：RX 会在下一个 BEACON 重发同一条响应，
 * 这里就再发一次确认。
 */
static const zlrs_radio_req_t *send_sess_cfm(zlrs_link_t *link)
{
	uint8_t payload[sizeof(uint32_t) + sizeof(uint16_t)];
	uint16_t len;

	wr32(&payload[0], link->sess_nonce_r);
	/*
	 * 回带当前的跳频索引：重新握手可能发生在连接态（对端重启），这时对端
	 * 拿到的索引就是本端此刻在用的那个，不用再从 BEACON 里猜。
	 */
	payload[4] = (uint8_t)(link->fhss.index & 0xFFU);
	payload[5] = (uint8_t)(link->fhss.index >> 8);
	len = build_frame(link, ZLRS_PKT_SESS_CFM, 0U, payload, sizeof(payload),
			  tag_key_for(link, ZLRS_PKT_SESS_CFM, true));
	link->sess_cfm_due = false;

	return submit_tx(link, len, sync_freq_hz(link), link->cfg.sync_slot,
			 link->sync_word);
}

static const zlrs_radio_req_t *send_rc(zlrs_link_t *link)
{
	uint16_t len = build_frame(link, ZLRS_PKT_RC, 0U, link->rc_payload, link->rc_len,
				   tag_key_for(link, ZLRS_PKT_RC, true));

	return submit_tx(link, len, slot_freq_hz(link), link->cfg.sync_slot,
			 link->sync_word);
}

static const zlrs_radio_req_t *send_tlm(zlrs_link_t *link)
{
	uint16_t len = build_frame(link, ZLRS_PKT_TLM, 0U, link->tlm_payload, link->tlm_len,
				   tag_key_for(link, ZLRS_PKT_TLM, true));

	return submit_tx(link, len, slot_freq_hz(link), link->cfg.sync_slot,
			 link->sync_word);
}

/* ---- ARQ（数据链路可选档，见 doc/09-protocol.md 4.3） ------------------- */

static uint16_t arq_inflight_bytes(const zlrs_link_t *link)
{
	uint16_t sum = 0U;

	for (uint8_t i = 0U; i < link->arq_used; i++) {
		sum = (uint16_t)(sum + link->arq_len[i]);
	}

	return sum;
}

/*
 * 把队首那帧从窗口里拿掉，同时把它的字节从队列里提交（delivered）或丢掉
 * （重传用尽）。后面几帧的字节在队列里是紧跟着的，所以队首一前移，
 * arq_len[] 跟着左移就对得上了。
 */
static void arq_release_oldest(zlrs_link_t *link, bool delivered)
{
	const uint16_t len = link->arq_len[0];

	link->queue_head = (uint16_t)((link->queue_head + len) % ZLRS_LINK_TX_QUEUE_LEN);
	link->queue_count = (uint16_t)(link->queue_count - len);

	if (!delivered) {
		link->stats.data_drop_bytes =
			(uint32_t)(link->stats.data_drop_bytes + len);
		link->stats.tx_arq_drop++;
	}

	for (uint8_t i = 1U; i < link->arq_used; i++) {
		link->arq_len[i - 1U] = link->arq_len[i];
	}

	link->arq_used--;
	link->arq_seq++;
	link->arq_retries = 0U;
}

/* 发窗口里的第 index 帧：载荷是 2 字节数据序号 + 数据 */
static const zlrs_radio_req_t *arq_send_slot(zlrs_link_t *link, uint8_t index,
					     uint16_t seq, uint16_t len, bool more)
{
	uint8_t payload[ZLRS_PAYLOAD_MAX_LEN];
	const uint8_t type = (link->cfg.role == ZLRS_ROLE_TX) ? ZLRS_PKT_DATA_UP
							     : ZLRS_PKT_DATA_DOWN;
	uint16_t offset = 0U;
	uint16_t frame_len;

	for (uint8_t i = 0U; i < index; i++) {
		offset = (uint16_t)(offset + link->arq_len[i]);
	}

	payload[0] = (uint8_t)(seq & 0xFFU);
	payload[1] = (uint8_t)(seq >> 8);
	for (uint16_t i = 0U; i < len; i++) {
		payload[2U + i] = link->tx_queue[(link->queue_head + offset + i) %
						 ZLRS_LINK_TX_QUEUE_LEN];
	}

	frame_len = build_frame(link, type, more ? ZLRS_FLAG_MORE : 0U, payload,
				(uint16_t)(len + 2U), tag_key_for(link, type, true));
	if (frame_len == 0U) {
		return NULL;
	}

	/* ARQ 的字节要等 ACK 才提交，这里不要让完成回调动队列 */
	link->pending_data_len = 0U;

	return submit_tx(link, frame_len, slot_freq_hz(link),
			 link->cfg.sync_slot, link->sync_word);
}

static const zlrs_radio_req_t *send_data_arq(zlrs_link_t *link, uint32_t now_ms)
{
	const uint16_t inflight = arq_inflight_bytes(link);
	const uint16_t timeout_ms = (link->cfg.arq_timeout_ms > 0U)
					    ? link->cfg.arq_timeout_ms
					    : ZLRS_ARQ_TIMEOUT_MS;
	const bool timeout = (link->arq_used > 0U) &&
			     (uint32_t)(now_ms - link->arq_sent_ms) >= timeout_ms;

	if (timeout) {
		if (link->arq_retries >= ZLRS_ARQ_MAX_RETRIES) {
			/*
			 * 重传用尽：这一段放弃（计数），窗口清空、序号回到 0，
			 * 并发一次重同步，让接收端把半截消息作废、从新的一段开始。
			 * 不这么做的话，后面的数据会被拼到带洞的流上。
			 */
			arq_release_oldest(link, false);
			link->arq_used = 0U;
			link->arq_retries = 0U;
			link->arq_seq = 0U;
			link->arq_resync = true;
			return NULL;
		}

		link->arq_retries++;
		link->stats.tx_retries++;
		link->arq_sent_ms = now_ms;

		/* 重传队首那帧：数据序号与长度都不变，MORE 按队列里后面还有没有数据算 */
		return arq_send_slot(link, 0U, link->arq_seq, link->arq_len[0],
				     link->queue_count > link->arq_len[0]);
	}

	if (link->arq_used >= ZLRS_ARQ_WINDOW) {
		return NULL; /* 窗口满，等 ACK */
	}
	if (link->queue_count <= inflight) {
		return NULL; /* 没有新数据 */
	}

	{
		uint16_t take = (uint16_t)(link->queue_count - inflight);
		const bool more = take > link->cfg.data_mtu;
		const uint8_t index = link->arq_used;
		const uint16_t seq = (uint16_t)(link->arq_seq + index);

		if (more) {
			take = link->cfg.data_mtu;
		}

		link->arq_len[index] = take;
		link->arq_used++;

		if (link->arq_used == 1U) {
			link->arq_retries = 0U;
			link->arq_sent_ms = now_ms;
		}

		return arq_send_slot(link, index, seq, take, more);
	}
}

/* ACK 帧：2 字节累计序号 + 1 字节标志（bit0 = 重同步） */
#define ZLRS_ARQ_ACK_RESYNC 0x01U

static const zlrs_radio_req_t *send_arq_ack(zlrs_link_t *link, uint16_t value,
					    uint8_t flags)
{
	uint8_t payload[3];
	uint16_t frame_len;

	payload[0] = (uint8_t)(value & 0xFFU);
	payload[1] = (uint8_t)(value >> 8);
	payload[2] = flags;

	frame_len = build_frame(link, ZLRS_PKT_ACK, 0U, payload, sizeof(payload),
				tag_key_for(link, ZLRS_PKT_ACK, true));
	link->arq_ack_pending = false;
	link->pending_data_len = 0U;

	if (frame_len == 0U) {
		return NULL;
	}

	return submit_tx(link, frame_len, slot_freq_hz(link),
			 link->cfg.sync_slot, link->sync_word);
}

static const zlrs_radio_req_t *send_ack(zlrs_link_t *link)
{
	/* 还没按序收下任何帧时回 0xFFFF：发送方看到它不会放掉任何一段 */
	const uint16_t value = link->arq_rx_started ? link->arq_rx_seq : 0xFFFFU;

	return send_arq_ack(link, value, 0U);
}

/* 累计 ACK：一次确认可以放掉窗口里连续的好几帧 */
static void handle_ack(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	uint16_t ack;
	uint8_t flags = 0U;

	if (!link->cfg.arq || frame->payload_len < 2U) {
		return;
	}

	ack = (uint16_t)((uint16_t)frame->payload[0] |
			 ((uint16_t)frame->payload[1] << 8));
	if (frame->payload_len >= 3U) {
		flags = frame->payload[2];
	}

	if ((flags & ZLRS_ARQ_ACK_RESYNC) != 0U) {
		/* 对端重传用尽、要重开一段：丢掉半截消息，回一次"还没有进度" */
		zlrs_reasm_init(&link->reasm, link->reasm_buf, sizeof(link->reasm_buf));
		link->arq_rx_started = false;
		link->arq_rx_seq = 0U;
		link->arq_ack_pending = true;
		return;
	}

	if (link->arq_resync) {
		/* 等对端确认它已经复位（0xFFFF = 没有进度）才算同步上 */
		if (ack == 0xFFFFU) {
			link->arq_resync = false;
		}
		return;
	}

	while (link->arq_used > 0U && (int16_t)(ack - link->arq_seq) >= 0) {
		arq_release_oldest(link, true);
	}

	/* 窗口前移，重传计时从这次确认重新起算 */
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

	for (uint16_t i = 0; i < take; i++) {
		payload[i] = link->tx_queue[(link->queue_head + i) % ZLRS_LINK_TX_QUEUE_LEN];
	}

	type = (link->cfg.role == ZLRS_ROLE_TX) ? ZLRS_PKT_DATA_UP : ZLRS_PKT_DATA_DOWN;
	len = build_frame(link, type, flags, payload, take, tag_key_for(link, type, true));
	link->pending_data_len = take;

	return submit_tx(link, len, slot_freq_hz(link), link->cfg.sync_slot,
			 link->sync_word);
}

/*
 * 配对载荷字段固定为 dev_a(发起方) | dev_b(响应方) | nonce_a | nonce_b，
 * 两端按自己的角色填，字段顺序不随角色变化。
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

	/* 发起方把本地失控值一起带过去，响应方存本地 */
	if (initiator && link->failsafe_len == ZLRS_RC_PAYLOAD_LEN) {
		out[PAIR_FLAGS_OFFSET] = PAIR_FLAG_FAILSAFE;
		memcpy(&out[PAIR_FAILSAFE_OFFSET], link->failsafe, ZLRS_RC_PAYLOAD_LEN);
	}
}

static const zlrs_radio_req_t *send_pair(zlrs_link_t *link, uint8_t type,
					 uint32_t nonce_a, uint32_t nonce_b,
					 uint32_t now_ms)
{
	uint8_t payload[PAIR_PAYLOAD_LEN];
	uint16_t len;

	pair_payload_fill(link, payload, nonce_a, nonce_b);
	len = build_frame(link, type, 0U, payload, sizeof(payload),
			  tag_key_for(link, type, true));
	link->last_pair_tx_ms = now_ms;

	return submit_tx(link, len, sync_freq_hz(link), ZLRS_SYNC_SLOT_PAIRING,
			 pairing_sync_word);
}

static const zlrs_radio_req_t *pair_rx(zlrs_link_t *link, uint16_t timeout_ms)
{
	return submit_rx(link, sync_freq_hz(link), ZLRS_SYNC_SLOT_PAIRING, pairing_sync_word,
			 timeout_ms);
}

/* ---- 接收：分发 ---------------------------------------------------------- */

static void handle_data(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	if (link->cfg.arq) {
		/* ARQ：载荷头 2 字节是数据序号，重组器按这个序号对齐 */
		uint16_t seq;
		zlrs_frame_t inner;

		if (frame->payload_len < 3U) {
			link->stats.rx_bad_frame++;
			return;
		}

		seq = (uint16_t)((uint16_t)frame->payload[0] |
				 ((uint16_t)frame->payload[1] << 8));

		/* 不管新旧都回一次累计 ACK，发送方才能收敛 */
		link->arq_ack_pending = true;

		if (!link->arq_rx_started) {
			/*
			 * 会话里的第一帧必须从序号 0 开始：半截流按错位投递比丢一段
			 * 更糟（上层拿到的是错位的数据）。不满足就只回 ACK、不投递，
			 * 让发送方重传。
			 */
			if (seq != 0U) {
				link->stats.rx_out_of_order++;
				return;
			}
			link->arq_rx_started = true;
		} else if ((int16_t)(seq - link->arq_rx_seq) <= 0) {
			link->stats.rx_dup++;
			return; /* 重传导致的重复帧：ACK 已置位，不重复投递 */
		} else if (seq != (uint16_t)(link->arq_rx_seq + 1U)) {
			link->stats.rx_out_of_order++;
			return; /* 缺口：等发送方重传，投递顺序由它保证 */
		}

		link->arq_rx_seq = seq;

		inner = *frame;
		inner.seq = seq;
		inner.payload = &frame->payload[2];
		inner.payload_len = (uint16_t)(frame->payload_len - 2U);
		inner.tag = NULL;

		switch (zlrs_reasm_push(&link->reasm, &inner, now_ms)) {
		case ZLRS_REASM_DONE:
			if (link->ev.on_data != NULL && link->reasm.len > 0U) {
				link->ev.on_data(link->ev.ctx, link->reasm.buf,
						 link->reasm.len);
			}
			break;
		case ZLRS_REASM_DROPPED:
			link->stats.rx_frag_drop++;
			break;
		default:
			break;
		}

		return;
	}

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

static void pairing_complete(zlrs_link_t *link, uint32_t now_ms)
{
	/* 握手交换的两个 nonce 就是这对设备的配对材料：存下来用于重算 pair_key */
	wr32(&link->pair_salt[0], link->pair_nonce);    /* nonce_a：发起方 */
	wr32(&link->pair_salt[4], link->pair_nonce_b); /* nonce_b：响应方 */

	derive_pair(link);
	link->paired = true;
	link->pair_cfm_pending = false;
	link->pair_wait_cfm = false;
	set_state(link, ZLRS_STATE_SEARCHING, now_ms);
	link->last_rx_ms = now_ms;
}

static void handle_beacon(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	uint32_t nonce;
	uint16_t hop_index;

	/* BEACON 只对已配对的两端有意义 */
	if (!link->paired) {
		return;
	}
	if (frame->payload_len < 8U) {
		return;
	}
	if (frame->payload[0] != ZLRS_PROTO_VERSION ||
	    frame->payload[1] != (uint8_t)link->cfg.profile) {
		return;
	}

	hop_index = (uint16_t)(frame->payload[2] | ((uint16_t)frame->payload[3] << 8));
	nonce = rd32(&frame->payload[4]);

	if (link->state == ZLRS_STATE_CONNECTED) {
		/*
		 * 连接态的周期 BEACON（doc/09-protocol.md 5.1）。
		 */
		if (link->cfg.resync_every_n == 0U) {
			/* 没开周期重同步：会话建立后不再听 BEACON（老行为） */
			return;
		}
		link->stats.rx_beacons++;

		if (link->session_valid && nonce == link->sess_nonce_t) {
			/*
			 * 同一段会话：只校正跳频索引，把两端晶振差抹掉。
			 * 序号必须更新——BEACON 用的是同一个序号空间，录下来的
			 * 旧帧不能拿来把索引带偏。
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
			return;
		}

		/*
		 * 换了会话（对端重启或重新配对）。这一条要拿"最近完全听不到对端"
		 * 当前提：BEACON 的 tag 是配对级的，录下来的旧 BEACON 也能验过，
		 * 只要还在正常收帧就不该被它带到 SEARCHING 去。
		 */
		if ((uint32_t)(now_ms - link->last_rx_ms) <= link->cfg.beacon_period_ms) {
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
		/*
		 * 简化握手：这条 BEACON 已经用配对级钥匙验过，采用它报告的跳频索引
		 * 就算会话成立——钥匙是静态的，没有 nonce 要交换。接下来用 SESS_RESP
		 * 反复报到，直到听见对端的第一帧，见 send_announce()。
		 */
		link->sess_nonce_t = nonce;
		link->sess_hop = hop_index;
		session_activate(link, hop_index);
		link->last_rx_seq = frame->seq;
		link->last_rx_ms = now_ms;
		link->announce = true;
		set_state(link, ZLRS_STATE_CONNECTED, now_ms);
		return;
	}

	/*
	 * 同一个 nonce_t 的 BEACON 是重发（TX 还没收到我们的回应）：把上次那条
	 * 回应再发一遍，nonce_r 不能换，换了对端算出的钥匙就跟我们对不上。
	 */
	if (link->session_pending && nonce == link->sess_nonce_t) {
		link->sess_resp_due = true;
		return;
	}

	/*
	 * 谁都可能录下 BEACON 再重放，所以这一步只是"答应谈"：抽一个自己的 nonce_r
	 * 把钥匙预备好，等 TX 用这把钥匙确认（SESS_CFM）才算会话成立。
	 */
	link->sess_nonce_t = nonce;
	link->sess_nonce_r = link_random(link, now_ms);
	link->sess_hop = hop_index;
	derive_session_keys(link, link->sess_nonce_t, link->sess_nonce_r);
	link->session_pending = true;
	link->sess_resp_due = true;
	link->last_rx_ms = now_ms;
	link->stats.last_rssi = link->stats.last_rssi; /* rssi 由移植层写入 */
}

static bool handle_pair_frame(zlrs_link_t *link, const zlrs_frame_t *frame,
			      uint32_t now_ms)
{
	const uint8_t *dev_a = frame->payload;
	const uint8_t *dev_b = &frame->payload[ZLRS_DEV_ID_LEN];
	uint32_t nonce_a;
	uint32_t nonce_b;
	const bool initiator = (link->cfg.role == ZLRS_ROLE_TX);

	if (frame->payload_len != PAIR_PAYLOAD_LEN) {
		return false;
	}

	nonce_a = rd32(&frame->payload[2U * ZLRS_DEV_ID_LEN]);
	nonce_b = rd32(&frame->payload[2U * ZLRS_DEV_ID_LEN + 4U]);

	switch (frame->type) {
	case ZLRS_PKT_PAIR_REQ:
		/* 响应方：记下发起方 id 与 nonce，下一时隙回 PAIR_RESP */
		if (initiator) {
			return false;
		}
		memcpy(link->peer_id, dev_a, ZLRS_DEV_ID_LEN);
		link->pair_nonce = nonce_a;
		link->pair_nonce_b = link_random(link, now_ms);
		link->pair_wait_cfm = true;

		/* 失控值由 TX 下发（它设过才带标志），存本地并交给应用 */
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
		/* 发起方：确认自己的 id 与 nonce 都被回带 */
		if (!initiator) {
			return false;
		}
		if (!zlrs_ct_equal(dev_a, link->dev_id, ZLRS_DEV_ID_LEN) ||
		    nonce_a != link->pair_nonce) {
			return false;
		}
		memcpy(link->peer_id, dev_b, ZLRS_DEV_ID_LEN);
		link->pair_nonce_b = nonce_b;
		link->pair_cfm_pending = true;
		return true;

	case ZLRS_PKT_PAIR_CFM:
		/* 响应方：两个 nonce 都回来了，配对完成 */
		if (initiator) {
			return false;
		}
		if (!zlrs_ct_equal(dev_a, link->peer_id, ZLRS_DEV_ID_LEN) ||
		    !zlrs_ct_equal(dev_b, link->dev_id, ZLRS_DEV_ID_LEN) ||
		    nonce_a != link->pair_nonce || nonce_b != link->pair_nonce_b) {
			return false;
		}
		link->pair_wait_cfm = false;
		pairing_complete(link, now_ms);
		return true;

	default:
		return false;
	}
}

/*
 * 上行链路质量：按序号差算这段窗口里"应该收到多少帧"，够一个窗口就更新一次
 * 百分比。只在会话内、序号严格递增的帧上累计（重复帧与配对帧不算）。
 */
#define ZLRS_LQ_WINDOW_FRAMES 128U

static void lq_account(zlrs_link_t *link, uint16_t seq)
{
	uint16_t delta = (uint16_t)(seq - link->lq_last_seq);

	if (link->stats.lq_rx == 0U && link->stats.lq_expected == 0U) {
		/* 第一个样本：不把上次断链留下的空档算成本次丢包 */
		delta = 1U;
	}

	link->lq_last_seq = seq;
	link->stats.lq_rx++;
	link->stats.lq_expected = (uint16_t)(link->stats.lq_expected + delta);

	if (link->stats.lq_expected >= ZLRS_LQ_WINDOW_FRAMES) {
		link->stats.lq_percent =
			(uint8_t)(((uint32_t)link->stats.lq_rx * 100U) /
				  link->stats.lq_expected);
		link->stats.lq_rx = 0U;
		link->stats.lq_expected = 0U;
	}
}

/*
 * RX 收到会话确认：用候选会话钥匙验 tag（不走"按状态选钥匙"那条路），验过才算
 * 会话成立。重放的确认对不上——它绑的是这次现抽的 nonce_r。
 */
static int handle_sess_cfm(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	uint8_t expect[ZLRS_TAG_LEN];
	const uint8_t tag_len = zlrs_profile_tag_len(link->cfg.profile);

	if (!link->session_pending || link->state != ZLRS_STATE_SEARCHING) {
		return -1;
	}
	if (frame->payload_len < sizeof(uint32_t)) {
		return -1;
	}

	/*
	 * P0（无 tag）下这条确认没有任何东西可以验——P0 本来就是"不做身份校验"的
	 * 调试档，只能靠回带的 nonce_r 对号。生产档没有这个分支。
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

	link->stats.rx_frames++;
	/*
	 * 采用哪一帧的跳频索引：确认帧里带了就用它（连接态重新握手时这就是
	 * 对端此刻的索引），旧固件不带就退回 BEACON 里那个。
	 */
	session_activate(link, (frame->payload_len >= 6U)
				      ? (uint16_t)(frame->payload[4] |
						   ((uint16_t)frame->payload[5] << 8))
				      : link->sess_hop);
	link->last_rx_seq = frame->seq; /* 确认帧本身占掉序号 0 起的第一个位置 */
	link->last_rx_ms = now_ms;
	set_state(link, ZLRS_STATE_CONNECTED, now_ms);

	return 0;
}

/*
 * TX 收到会话响应：回带的 nonce_t 必须是当前这一次请求的（旧响应、重放的响应
 * 一律丢），然后算钥匙、发确认。RX 没收到确认时会重发同一条响应，所以这里允许
 * 同一对 nonce 反复触发，等于给确认帧做重传。
 */
static bool handle_sess_resp(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms)
{
	uint32_t nonce_t;
	uint32_t nonce_r;

	/* SESS_RESP 是响应方回给发起方的，角色不对直接丢 */
	if (link->cfg.role != ZLRS_ROLE_TX ||
	    frame->payload_len < 2U * sizeof(uint32_t)) {
		return false;
	}

	nonce_t = rd32(&frame->payload[0]);
	nonce_r = rd32(&frame->payload[4]);
	if (nonce_t != link->sess_nonce_t) {
		return false;
	}

	/*
	 * 简化握手：对端说的是"我听见你了"。钥匙是静态的，没有确认帧要发——
	 * 本端接下来的数据帧就是确认。CONNECTED 下收到它只当"对端还在"。
	 */
	if (link->cfg.handshake == 0U) {
		link->last_rx_ms = now_ms;
		if (link->state == ZLRS_STATE_SEARCHING) {
			set_state(link, ZLRS_STATE_CONNECTED, now_ms);
		}
		return true;
	}

	/*
	 * CONNECTED 下收到 SESS_RESP：对端重启后听见了本端的周期 BEACON，正在
	 * 要求重新握手。本端就用新的 nonce_r 换一套会话钥匙，再回一次确认。
	 */
	if (link->state != ZLRS_STATE_SEARCHING && link->state != ZLRS_STATE_CONNECTED) {
		return false;
	}

	if (!link->session_valid || nonce_r != link->sess_nonce_r) {
		link->sess_nonce_r = nonce_r;
		derive_session_keys(link, nonce_t, nonce_r);
		session_activate(link, link->fhss.index);
	}
	link->sess_cfm_due = true;
	/* 会话刚建立：别让失联判定拿着 SEARCHING 期间的旧时间戳把这一侧踢回去 */
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

	/* 会话确认得先用候选钥匙验，不能走下面"按会话状态选钥匙"那套 */
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
		if (link->state != ZLRS_STATE_PAIRING) {
			return -1;
		}
		return handle_pair_frame(link, &frame, now_ms) ? 0 : -1;
	}

	if (frame.type == ZLRS_PKT_BEACON) {
		handle_beacon(link, &frame, now_ms);
		return 0;
	}

	if (frame.type == ZLRS_PKT_SESS_RESP) {
		return handle_sess_resp(link, &frame, now_ms) ? 0 : -1;
	}

	/* 发起方收到对端的会话帧即认为链路建立 */
	if (link->state == ZLRS_STATE_SEARCHING && link->cfg.role == ZLRS_ROLE_TX &&
	    (frame.type == ZLRS_PKT_TLM || frame.type == ZLRS_PKT_DATA_UP ||
	     frame.type == ZLRS_PKT_DATA_DOWN)) {
		set_state(link, ZLRS_STATE_CONNECTED, now_ms);
	}

	if (link->state != ZLRS_STATE_CONNECTED) {
		return -1;
	}

	/* 防重放：同一会话内序列号必须严格递增 */
	if ((int16_t)(frame.seq - link->last_rx_seq) <= 0) {
		link->stats.rx_dup++;
		return -1;
	}
	link->last_rx_seq = frame.seq;
	link->last_rx_ms = now_ms;
	/* 对端活着：简化握手的"我在"报到不用再发了 */
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
	case ZLRS_PKT_ACK:
		handle_ack(link, &frame, now_ms);
		break;
	default:
		break;
	}

	return 0;
}

/* ---- 时隙 ---------------------------------------------------------------- */

static void advance_hop(zlrs_link_t *link)
{
	if (link->cfg.hopping == 0U) {
		/* 固定在同步信道（跳频表第 0 个位置就是它） */
		return;
	}

	if (link->state == ZLRS_STATE_CONNECTED) {
		(void)zlrs_fhss_next_hz(&link->fhss);
	}
}

static const zlrs_radio_req_t *control_slot(zlrs_link_t *link, uint8_t slot,
					    uint32_t now_ms)
{
	const bool down = (slot == ZLRS_SLOT_CTL_DOWN);
	const bool tx_side = (link->cfg.role == ZLRS_ROLE_TX);

	/* 每帧的主时隙（下行）过一次计数，两端步调一致 */
	if (down) {
		link->frame_index++;
	}

	if (link->state == ZLRS_STATE_PAIRING) {
		if (down) {
			if (tx_side) {
				if (link->pair_cfm_pending) {
					const zlrs_radio_req_t *req =
						send_pair(link, ZLRS_PKT_PAIR_CFM,
							  link->pair_nonce,
							  link->pair_nonce_b, now_ms);

					pairing_complete(link, now_ms);
					return req;
				}
				if ((uint32_t)(now_ms - link->last_pair_tx_ms) <
				    link->cfg.pair_period_ms) {
					return NULL;
				}
				return send_pair(link, ZLRS_PKT_PAIR_REQ, link->pair_nonce,
						 0U, now_ms);
			}
			return pair_rx(link, link->cfg.rx_timeout_ms);
		}

		if (tx_side) {
			return pair_rx(link, link->cfg.rx_timeout_ms);
		}
		if (link->pair_wait_cfm &&
		    (uint32_t)(now_ms - link->last_pair_tx_ms) >= link->cfg.pair_period_ms) {
			return send_pair(link, ZLRS_PKT_PAIR_RESP, link->pair_nonce,
					 link->pair_nonce_b, now_ms);
		}
		return NULL;
	}

	if (link->state == ZLRS_STATE_SEARCHING) {
		if (tx_side) {
			if (down) {
				if (link->sess_cfm_due) {
					/*
					 * 确认发出去就进 CONNECTED：这样双方从下一
					 * 帧起一起推进跳频，索引不会错开。对端没
					 * 收到确认的话，失联判定会把我们拉回来。
					 */
					const zlrs_radio_req_t *req = send_sess_cfm(link);

					if (req != NULL) {
						set_state(link, ZLRS_STATE_CONNECTED, now_ms);
					}
					return req;
				}
				/*
				 * 时间到了就发；另外在重同步帧的位置多补一发——
				 * 对端如果还在 CONNECTED，它只在这些帧上开着同步
				 * 信道的窗口，不然它听不见"我换了会话"。
				 */
				if ((uint32_t)(now_ms - link->last_beacon_ms) <
					    link->cfg.beacon_period_ms &&
				    !beacon_frame(link)) {
					return NULL;
				}
				return send_beacon(link, now_ms);
			}

			/* 发起方在上行时隙等对端遥测 */
			return submit_rx(link, sync_freq_hz(link),
					 link->cfg.sync_slot, link->sync_word,
					 link->cfg.rx_timeout_ms);
		}
		if (down) {
			return submit_rx(link,
					 sync_freq_hz(link),
					 link->cfg.sync_slot, link->sync_word,
					 link->cfg.rx_timeout_ms);
		}
		if (link->sess_resp_due) {
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
			/* 会话确认还欠着：先补确认，这一帧整帧停在同步信道上 */
			if (link->sess_cfm_due) {
				return send_sess_cfm(link);
			}
			/*
			 * 重同步帧：发一条 BEACON，把自己的跳频索引摆出来。
			 * 对端据此校正漂移；对端若换了会话（重启），也据此重握手。
			 */
			if (beacon_frame(link)) {
				return send_beacon(link, now_ms);
			}
			return send_rc(link);
		}
		return submit_rx(link, slot_freq_hz(link),
				 link->cfg.sync_slot, link->sync_word,
				 link->cfg.rx_timeout_ms);
	}

	/* 会话确认还欠着：对端（SEARCHING）会在同步信道上重发 SESS_RESP */
	if (link->sess_cfm_due) {
		if (tx_side) {
			return submit_rx(link, sync_freq_hz(link), link->cfg.sync_slot,
					 link->sync_word, link->cfg.rx_timeout_ms);
		}
		return NULL;
	}

	/*
	 * 重同步帧的上行时隙也留在同步信道：TX 在这里等对端可能发来的 SESS_RESP
	 * （对端重启时就是这条路），RX 把遥测发在同一个信道上，不然这一帧白过。
	 */
	if (beacon_frame(link)) {
		if (tx_side) {
			return submit_rx(link, sync_freq_hz(link), link->cfg.sync_slot,
					 link->sync_word, link->cfg.rx_timeout_ms);
		}
		if (link->cfg.telemetry == 0U) {
			return NULL;
		}
		return send_tlm(link);
	}

	/*
	 * 简化握手的报到帧优先：对端可能还在 SEARCHING，正守在同步信道上。
	 * 关掉遥测以后这个信道上就只剩报到帧了。
	 */
	if (link->announce) {
		if (tx_side) {
			return submit_rx(link, sync_freq_hz(link), link->cfg.sync_slot,
					 link->sync_word, link->cfg.rx_timeout_ms);
		}
		return send_sess_resp(link);
	}

	if (link->cfg.telemetry == 0U) {
		return NULL;
	}

	/* 上行时隙：遥测按 tlm_every_n 节流（两端同节奏，跳频不受影响） */
	if ((link->frame_index % link->cfg.tlm_every_n) != 0U) {
		return NULL;
	}

	if (tx_side) {
		return submit_rx(link, slot_freq_hz(link), link->cfg.sync_slot,
				 link->sync_word, link->cfg.rx_timeout_ms);
	}

	return send_tlm(link);
}

static const zlrs_radio_req_t *data_slot(zlrs_link_t *link, uint8_t slot,
					 uint32_t now_ms)
{
	const bool a = (slot == ZLRS_SLOT_DATA_A);
	const bool tx_side = (link->cfg.role == ZLRS_ROLE_TX);

	/* 每帧的主时隙（A）过一次计数 */
	if (a) {
		link->frame_index++;
	}

	if (link->state == ZLRS_STATE_SEARCHING) {
		if (tx_side) {
			if (a) {
				if (link->sess_cfm_due) {
					/*
					 * 数据链路没有遥测可等，确认发出去就当
					 * 会话成立；对端没收到的话，失联判定会
					 * 把这一侧拉回 SEARCHING 重来。
					 */
					const zlrs_radio_req_t *req = send_sess_cfm(link);

					if (req != NULL) {
						set_state(link, ZLRS_STATE_CONNECTED, now_ms);
					}
					return req;
				}
				/*
				 * 时间到了就发；另外在重同步帧的位置多补一发——
				 * 对端如果还在 CONNECTED，它只在这些帧上开着同步
				 * 信道的窗口，不然它听不见"我换了会话"。
				 */
				if ((uint32_t)(now_ms - link->last_beacon_ms) <
					    link->cfg.beacon_period_ms &&
				    !beacon_frame(link)) {
					return NULL;
				}
				return send_beacon(link, now_ms);
			}
			/* 发起方在上行时隙等对端的第一帧 */
			return submit_rx(link, sync_freq_hz(link),
					 link->cfg.sync_slot, link->sync_word,
					 link->cfg.rx_timeout_ms);
		}
		if (a) {
			return submit_rx(link,
					 sync_freq_hz(link),
					 link->cfg.sync_slot, link->sync_word,
					 link->cfg.rx_timeout_ms);
		}
		if (link->sess_resp_due) {
			return send_sess_resp(link);
		}
		return NULL;
	}

	if (link->state != ZLRS_STATE_CONNECTED) {
		return NULL;
	}

	/*
	 * 控制帧率拉高以后，数据窗口按 data_every_n 稀疏（两端同节奏）。
	 * 重同步帧与会话确认不参与稀疏：它们的节奏要能整除剩下的每一帧，
	 * 否则（比如 333 帧与 5 帧互质）会几乎永远撞不上。
	 */
	if (!beacon_frame(link) && !link->sess_cfm_due &&
	    (link->frame_index % link->cfg.data_every_n) != 0U) {
		return NULL;
	}

	advance_hop(link);

	/* 简化握手的报到帧：数据链路在自己的那个半窗口里发，走同步信道 */
	if (link->announce && (tx_side == a)) {
		return send_sess_resp(link);
	}

	if (tx_side == a) {
		/* 会话确认优先，其次才是重同步 BEACON（两者都走同步信道） */
		if (link->sess_cfm_due) {
			return send_sess_cfm(link);
		}
		if (beacon_frame(link)) {
			return send_beacon(link, now_ms);
		}
	}

	if (tx_side == a) {
		const zlrs_radio_req_t *req;

		/* ARQ 重同步：先让对端把半截消息作废，再发新的一段 */
		if (link->cfg.arq && link->arq_resync) {
			req = send_arq_ack(link, 0xFFFFU, ZLRS_ARQ_ACK_RESYNC);
			if (req != NULL) {
				return req;
			}
		}

		/* ARQ：待发的累计 ACK 优先于数据，越早确认越省重传 */
		if (link->cfg.arq && link->arq_ack_pending) {
			req = send_ack(link);
			if (req != NULL) {
				return req;
			}
		}

		req = send_data(link, now_ms);

		if (req != NULL) {
			return req;
		}
	}

	return submit_rx(link, slot_freq_hz(link), link->cfg.sync_slot,
			 link->sync_word, link->cfg.rx_timeout_ms);
}

/* ---- 对外接口 ------------------------------------------------------------ */

void zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg,
		    const zlrs_radio_ops_t *radio, const zlrs_link_events_t *events)
{
	memset(link, 0, sizeof(*link));
	link->cfg = *cfg;
	/* 节奏参数缺省当 1（每帧都跑），0 是非法值 */
	if (link->cfg.tlm_every_n == 0U) {
		link->cfg.tlm_every_n = 1U;
	}
	if (link->cfg.data_every_n == 0U) {
		link->cfg.data_every_n = 1U;
	}
	if (radio != NULL) {
		link->radio = *radio;
	}
	if (events != NULL) {
		link->ev = *events;
	}

	zlrs_reasm_init(&link->reasm, link->reasm_buf, sizeof(link->reasm_buf));
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

void zlrs_link_set_mod(zlrs_link_t *link, const zlrs_mod_cfg_t *mod)
{
	if (mod == NULL) {
		return;
	}

	link->cfg.mod = *mod;
	/* LoRa 的同步字是派生出来的，外面给 0 时不覆盖现用的那个 */
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
	link->pair_wait_cfm = false;
	set_state(link, ZLRS_STATE_PAIRING, now_ms);
}

const zlrs_radio_req_t *zlrs_link_slot(zlrs_link_t *link, uint8_t slot, uint32_t now_ms)
{
	if (link->cfg.kind == ZLRS_LINK_CONTROL) {
		return control_slot(link, slot, now_ms);
	}

	return data_slot(link, slot, now_ms);
}

void zlrs_link_complete(zlrs_link_t *link, int status, uint16_t rx_len, int16_t rssi,
			int8_t snr, uint32_t now_ms)
{
	const bool was_tx = link->req_is_tx;

	link->req_pending = false;
	link->stats.last_rssi = rssi;
	link->stats.last_snr = snr;

	if (status != ZLRS_RADIO_OK) {
		link->stats.radio_fail++;
		if (was_tx) {
			/* 发送失败时把已经摘掉的字节放回队列 */
			link->queue_count =
				(uint16_t)(link->queue_count + link->pending_data_len);
			link->pending_data_len = 0U;
		}
		return;
	}

	if (was_tx) {
		link->stats.tx_frames++;
		link->stats.tx_bytes = (uint32_t)(link->stats.tx_bytes + link->req.size);
		link->queue_head = (uint16_t)((link->queue_head + link->pending_data_len) %
					      ZLRS_LINK_TX_QUEUE_LEN);
		link->queue_count = (uint16_t)(link->queue_count - link->pending_data_len);
		link->pending_data_len = 0U;
		return;
	}

	if (rx_len > 0U) {
		(void)handle_rx(link, rx_len, now_ms);
	}
}

void zlrs_link_rx(zlrs_link_t *link, const uint8_t *buf, uint16_t len, int16_t rssi,
		  int8_t snr, uint32_t now_ms)
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
	switch (link->state) {
	case ZLRS_STATE_UNCONFIGURED:
		if (link->configured) {
			set_state(link, ZLRS_STATE_UNPAIRED, now_ms);
		}
		break;

	case ZLRS_STATE_PAIRING:
		if ((uint32_t)(now_ms - link->state_ms) > link->cfg.pair_window_ms) {
			set_state(link, link->paired ? ZLRS_STATE_SEARCHING
						     : ZLRS_STATE_UNPAIRED,
				  now_ms);
		}
		break;

	case ZLRS_STATE_CONNECTED:
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
	uint16_t written = 0U;

	if (data == NULL) {
		return 0U;
	}

	while (written < len && link->queue_count < ZLRS_LINK_TX_QUEUE_LEN) {
		const uint16_t tail =
			(uint16_t)((link->queue_head + link->queue_count) %
				   ZLRS_LINK_TX_QUEUE_LEN);

		link->tx_queue[tail] = data[written];
		link->queue_count++;
		written++;
	}

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

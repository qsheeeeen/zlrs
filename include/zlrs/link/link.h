/*
 * 链路层：一条链路一份实例，跑文档定义的状态机与超帧时隙。
 * 见 doc/03-protocol.md 第 3、4 节。
 *
 *   控制链路（sub-GHz）：RC 下行 + TLM 上行，另负责 BEACON 与配对
 *   数据链路（2.4G）：DATA_UP / DATA_DOWN，best-effort 透传
 *
 * 调用顺序（一个超帧内）：
 *
 *   for (slot = ...) {
 *           req = zlrs_link_slot(link, slot, now_ms);
 *           if (req) radio.submit(&req);   // 完成后回调 zlrs_link_complete()
 *   }
 *   zlrs_link_tick(link, now_ms);          // 超时与状态迁移
 *
 * 本层不依赖 Zephyr，也不直接碰射频芯片。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_LINK_LINK_H_
#define ZLRS_LINK_LINK_H_

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/link/radio.h"
#include "zlrs/proto/config.h"
#include "zlrs/proto/crypto.h"
#include "zlrs/proto/fhss.h"
#include "zlrs/proto/frag.h"
#include "zlrs/proto/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 链路种类 */
#define ZLRS_LINK_CONTROL 0U
#define ZLRS_LINK_DATA 1U

/* 超帧时隙，见 doc/03-protocol.md 4.1 */
#define ZLRS_SLOT_CTL_DOWN 0U /* TX -> RX：RC 帧（RX 侧是接收窗口） */
#define ZLRS_SLOT_CTL_UP 1U   /* RX -> TX：TLM 帧（TX 侧是接收窗口） */
#define ZLRS_SLOT_DATA_A 2U   /* 数据窗口前半：TX -> RX */
#define ZLRS_SLOT_DATA_B 3U   /* 数据窗口后半：RX -> TX */

/* 链路状态，见 doc/03-protocol.md 3.2 */
typedef enum {
	ZLRS_STATE_UNCONFIGURED = 0,
	ZLRS_STATE_UNPAIRED,
	ZLRS_STATE_PAIRING,
	ZLRS_STATE_SEARCHING,
	ZLRS_STATE_CONNECTED,
} zlrs_link_state_t;

/* 完成状态，移植层把射频结果折算成这两个值 */
#define ZLRS_RADIO_OK 0
#define ZLRS_RADIO_FAIL 1

typedef struct {
	uint8_t kind;      /* ZLRS_LINK_CONTROL / ZLRS_LINK_DATA */
	uint8_t role;      /* ZLRS_ROLE_TX / ZLRS_ROLE_RX */
	uint8_t domain;    /* ZLRS_DOMAIN_* */
	uint8_t sync_slot; /* ZLRS_SYNC_SLOT_* */
	zlrs_profile_t profile;
	uint32_t rate_bps;
	int8_t tx_power_dbm;
	uint16_t rx_timeout_ms;     /* 单次接收窗口 */
	uint16_t beacon_period_ms;  /* SEARCHING 下 BEACON 周期 */
	uint16_t peer_timeout_ms;   /* 失联判定 */
	uint16_t pair_period_ms;    /* PAIR_REQ 重发周期 */
	uint16_t pair_window_ms;    /* 配对窗口 */
	uint16_t data_mtu;          /* 数据帧载荷上限 */
	uint8_t arq;                /* 1 = 透传走 ARQ profile，0 = best-effort */
} zlrs_link_cfg_t;

typedef struct {
	void (*on_rc)(void *ctx, const uint8_t *payload, uint16_t len);
	void (*on_tlm)(void *ctx, const uint8_t *payload, uint16_t len);
	void (*on_data)(void *ctx, const uint8_t *data, uint16_t len);
	void (*on_state)(void *ctx, uint8_t state);
	void *ctx;
} zlrs_link_events_t;

typedef struct {
	uint32_t tx_frames;
	uint32_t tx_bytes;
	uint32_t rx_frames;
	uint32_t rx_bytes;
	uint32_t rx_bad_tag;
	uint32_t rx_bad_frame;
	uint32_t rx_dup;
	uint32_t rx_out_of_order; /* ARQ：序号跳变导致的丢弃 */
	uint32_t tx_retries;      /* ARQ：重传次数 */
	uint32_t tx_arq_drop;     /* ARQ：重传用尽后放弃的帧数 */
	uint32_t rx_frag_drop;
	uint32_t radio_fail;
	int16_t last_rssi;
	int8_t last_snr; /* FLRC 下射频栈给不出，见 doc/04-lr2021.md */
	/* 上行链路质量：最近一个窗口里实收帧数与按序号算出的应有帧数 */
	uint8_t lq_percent;
	uint16_t lq_rx;
	uint16_t lq_expected;
	/* 透传背压：因为空口队列满而丢掉的字节 */
	uint32_t data_drop_bytes;
} zlrs_link_stats_t;

#define ZLRS_LINK_TX_QUEUE_LEN 512U
#define ZLRS_LINK_TLM_MAX_LEN 32U

/*
 * ARQ profile（数据链路可选档，见 doc/03-protocol.md 4.3）：累计 ACK + 固定窗口
 * + 超时重传。窗口是"从队首开始连续的若干帧"，只有收到 ACK 才把对应字节从
 * 队列里提交出去，所以重传不需要额外缓存。
 */
#define ZLRS_ARQ_WINDOW 4U
#define ZLRS_ARQ_TIMEOUT_MS 30U
#define ZLRS_ARQ_MAX_RETRIES 6U

typedef struct {
	zlrs_link_cfg_t cfg;
	zlrs_radio_ops_t radio;
	zlrs_link_events_t ev;
	zlrs_link_stats_t stats;

	/* 配置与密钥 */
	bool configured;
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t root_key[ZLRS_KEY_LEN];
	uint8_t auth_key[ZLRS_KEY_LEN];
	bool paired;
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	uint8_t pair_key[ZLRS_KEY_LEN];
	uint8_t link_auth[ZLRS_KEY_LEN];
	uint8_t sync_word[ZLRS_SYNC_WORD_LEN];
	uint8_t session_key[ZLRS_KEY_LEN];
	bool session_valid;
	uint32_t session_nonce;
	uint32_t pair_nonce;

	/* 状态与计时 */
	uint8_t state;
	uint32_t state_ms;
	uint32_t last_rx_ms;
	uint32_t last_beacon_ms;
	uint32_t last_pair_tx_ms;
	bool pair_cfm_pending;  /* TX 收到 PAIR_RESP，下一个下行时隙发 PAIR_CFM */
	bool pair_wait_cfm;     /* RX 已回 PAIR_RESP，等 PAIR_CFM */

	/* 序号与跳频 */
	uint16_t tx_seq;
	uint16_t last_rx_seq;
	uint16_t lq_last_seq;
	zlrs_fhss_t fhss;

	/* 载荷 */
	uint8_t rc_payload[ZLRS_RC_PAYLOAD_LEN];
	uint16_t rc_len;
	uint8_t tlm_payload[ZLRS_LINK_TLM_MAX_LEN];
	uint16_t tlm_len;

	/* 透传队列 */
	uint8_t tx_queue[ZLRS_LINK_TX_QUEUE_LEN];
	uint16_t queue_head;
	uint16_t queue_count;
	uint16_t pending_data_len;

	/* ARQ 发送侧：窗口里在飞的帧长度，队首那帧是 [0] */
	uint16_t arq_seq;
	uint16_t arq_len[ZLRS_ARQ_WINDOW];
	uint8_t arq_used;
	uint8_t arq_retries;
	uint32_t arq_sent_ms;
	/* ARQ 接收侧：已按序收下的最大数据序号 + 待发累计 ACK */
	uint16_t arq_rx_seq;
	bool arq_rx_started;
	bool arq_ack_pending;
	/* ARQ 重同步：重传用尽后重开一段，等对端确认它已经复位 */
	bool arq_resync;

	/* 收发缓冲与重组 */
	uint8_t tx_buf[ZLRS_FRAME_MAX_LEN];
	uint8_t rx_buf[ZLRS_FRAME_MAX_LEN];
	zlrs_reasm_t reasm;
	uint8_t reasm_buf[ZLRS_REASM_BUF_LEN];

	/* 当前待提交的事务 */
	zlrs_radio_req_t req;
	bool req_is_tx;
	bool req_pending;
} zlrs_link_t;

void zlrs_link_init(zlrs_link_t *link, const zlrs_link_cfg_t *cfg,
		    const zlrs_radio_ops_t *radio, const zlrs_link_events_t *events);

/* 写入根密钥后链路才参与收发 */
void zlrs_link_set_root_key(zlrs_link_t *link, const uint8_t root_key[ZLRS_KEY_LEN],
			    uint32_t now_ms);
void zlrs_link_set_dev_id(zlrs_link_t *link, const uint8_t dev_id[ZLRS_DEV_ID_LEN]);

/* 运行中换速率档（速率自适应用），下一个事务生效 */
void zlrs_link_set_rate(zlrs_link_t *link, uint32_t rate_bps);

/* 从持久化记录恢复配对（两端重启后不必重新配对） */
void zlrs_link_set_peer(zlrs_link_t *link, const uint8_t peer_id[ZLRS_DEV_ID_LEN],
			uint32_t now_ms);

/* 按键：长按触发配对（换绑也走这里，新记录顶掉旧的） */
void zlrs_link_press(zlrs_link_t *link, uint32_t now_ms);

/* 超帧时隙：返回待提交的事务，没有则返回 NULL */
const zlrs_radio_req_t *zlrs_link_slot(zlrs_link_t *link, uint8_t slot, uint32_t now_ms);

/* 射频事务完成 */
void zlrs_link_complete(zlrs_link_t *link, int status, uint16_t rx_len, int16_t rssi,
			int8_t snr, uint32_t now_ms);

/*
 * 直接投递一帧的入口：等价于"射频收到了 len 字节"。
 * zlrs_link_complete() 正常路径上也走这里；旁路收包与主机测试用它。
 */
void zlrs_link_rx(zlrs_link_t *link, const uint8_t *buf, uint16_t len, int16_t rssi,
		  int8_t snr, uint32_t now_ms);

/* 超时与状态迁移，每个超帧调一次 */
void zlrs_link_tick(zlrs_link_t *link, uint32_t now_ms);

/* 透传数据入队（RX 与 TX 都可用），返回写入字节数 */
uint16_t zlrs_link_write(zlrs_link_t *link, const uint8_t *data, uint16_t len);

/* 控制链路载荷：TX 侧设置 RC，RX 侧设置 TLM */
void zlrs_link_set_rc(zlrs_link_t *link, const uint8_t *channels, uint16_t len);
void zlrs_link_set_tlm(zlrs_link_t *link, const uint8_t *payload, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_LINK_LINK_H_ */

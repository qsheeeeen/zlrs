/*
 * 链路层：一条链路一份实例，跑状态机与超帧时隙，见 doc/04-protocol.md 第 3、4 节。
 *
 * 每个超帧：zlrs_link_slot() 取事务 → radio.submit() → 完成后 zlrs_link_complete()，
 * 帧尾再 zlrs_link_tick() 走超时与状态迁移。本层不依赖 Zephyr，也不碰射频芯片。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

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

/* 超帧时隙，见 doc/04-protocol.md 4.1 */
#define ZLRS_SLOT_CTL_DOWN 0U /* TX -> RX：RC 帧（RX 侧是接收窗口） */
#define ZLRS_SLOT_CTL_UP 1U   /* RX -> TX：TLM 帧（TX 侧是接收窗口） */
#define ZLRS_SLOT_DATA_A 2U   /* 数据窗口前半：TX -> RX */
#define ZLRS_SLOT_DATA_B 3U   /* 数据窗口后半：RX -> TX */

/* 链路状态，见 doc/04-protocol.md 3.2 */
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
	/*
	 * 调制档：FLRC 的速率档或 LoRa 的 SF/带宽/编码率/前导码，见
	 * doc/04-protocol.md 4.1.1。LoRa 的同步字由配对钥匙派生，链路层自己填
	 * mod.sync_byte（外面给 0 就行）。
	 */
	zlrs_mod_cfg_t mod;
	int8_t tx_power_dbm;
	uint16_t rx_timeout_ms;     /* 单次接收窗口 */
	uint16_t beacon_period_ms;  /* SEARCHING 下 BEACON 周期 */
	uint16_t peer_timeout_ms;   /* 失联判定 */
	uint16_t pair_period_ms;    /* PAIR_REQ 重发周期 */
	uint16_t pair_window_ms;    /* 配对窗口 */
	uint16_t data_mtu;          /* 数据帧载荷上限 */
	uint8_t arq;                /* 1 = 透传走 ARQ profile，0 = best-effort */
	/* ARQ 重传超时（ms）。0 = 用 ZLRS_ARQ_TIMEOUT_MS（FLRC 的亚毫秒帧） */
	uint16_t arq_timeout_ms;
	/*
	 * 逐包跳频。0 = 固定在同步信道（跳频表的第 0 个位置就是同步信道），
	 * 上板调试时先把"收发时序"与"跳频表"分开看，见 doc/02-platform.md 第 4 节。
	 */
	uint8_t hopping;
	/*
	 * 会话三次握手。0 = 直接用配对级的 link_auth 当会话钥匙：BEACON 之后的
	 * 应答只需要 SESS_RESP，对端用第一条数据帧当确认。少一段钥匙派生、少一条
	 * 消息，代价是没有逐会话的钥匙轮换——只用于上板调试，见 doc/05-security.md
	 * 第 3 节。
	 */
	uint8_t handshake;
	/* 控制链路的上行遥测时隙。0 = 不发、也不开这个接收窗口 */
	uint8_t telemetry;
	/*
	 * 节奏稀疏化（1 = 每帧都跑）：控制帧率拉高以后，遥测与数据窗口不必跟着
	 * 每帧来一次。计数在链路层内部做，两端同固件所以节奏一致，跳频不会失步。
	 */
	uint8_t tlm_every_n;
	uint8_t data_every_n;
	/*
	 * 连接态周期 BEACON 的间隔（帧）。0 = 不周期重同步（只在建链时对齐跳频
	 * 索引）；>0 = 每这么多帧用同步信道发一次 BEACON，对端据此校正跳频索引，
	 * 也据此发现"对端换了会话"（重启/重新配对）并自动重新握手。
	 */
	uint8_t resync_every_n;
} zlrs_link_cfg_t;

typedef struct {
	void (*on_rc)(void *ctx, const uint8_t *payload, uint16_t len);
	void (*on_tlm)(void *ctx, const uint8_t *payload, uint16_t len);
	void (*on_data)(void *ctx, const uint8_t *data, uint16_t len);
	void (*on_state)(void *ctx, uint8_t state);
	/* RX 侧：配对时从 TX 收到失控值（RC 载荷），应用存 NVS 并用于输出 */
	void (*on_failsafe)(void *ctx, const uint8_t *payload, uint16_t len);
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
	uint32_t rx_beacons;      /* 连接态收到的 BEACON 数 */
	uint32_t hop_resync;      /* 跳频索引被 BEACON 校正过的次数 */
	uint32_t radio_fail;
	int16_t last_rssi;
	int8_t last_snr; /* FLRC 下射频栈给不出，见 doc/06-radio.md */
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
 * ARQ profile（数据链路可选档，见 doc/04-protocol.md 4.3）：累计 ACK + 固定窗口
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
	/*
	 * 配对时交换的两个 nonce（发起方在前），参与 pair_key 派生，并跟着配对
	 * 记录落盘：每次配对都换一份，旧录制里的会话就再也算不出来。
	 * 见 doc/05-security.md 第 3 节。
	 */
	uint8_t pair_salt[ZLRS_PAIR_SALT_LEN];
	uint8_t pair_key[ZLRS_KEY_LEN];
	/*
	 * 每包 tag 的钥匙按"发送方 → 接收方"分开，上下文是这一对的双方 dev_id：
	 * out 打本机发出去的帧（本机在前），in 校验对端发来的帧（对端在前）。
	 * 两端各算一次就配成一对，不需要协商，也不掺角色。混成一把的话，反射回来
	 * 的自己的帧会通过 tag 校验，见 doc/05-security.md 第 3 节。
	 */
	uint8_t link_auth_out[ZLRS_KEY_LEN];
	uint8_t link_auth_in[ZLRS_KEY_LEN];
	uint8_t sync_word[ZLRS_SYNC_WORD_LEN];
	/* LoRa 的同步字只有 1 字节 + private bit，见 doc/04-protocol.md 5.3 */
	uint8_t lora_sync;
	uint8_t session_key_out[ZLRS_KEY_LEN];
	uint8_t session_key_in[ZLRS_KEY_LEN];
	bool session_valid;
	/*
	 * 会话建立与配对同一个形状（见 doc/04-protocol.md 3.5）：发起方出 nonce_t，
	 * 响应方出 nonce_r，两个 nonce 一起进 KDF，最后一条消息带着 nonce_r 证明
	 * 对端算出了同一把钥匙。RX 只在收到这条证明之后才采用会话，所以重放旧
	 * BEACON 拿不到任何旧会话的钥匙。
	 */
	bool session_pending;    /* RX：已回应 BEACON，等对端用新钥匙确认 */
	bool sess_resp_due;      /* RX：上行时隙要发 SESS_RESP */
	bool sess_cfm_due;       /* TX：下行时隙要发 SESS_CFM */
	uint32_t sess_nonce_t;   /* 发起方（TX）的 nonce */
	uint32_t sess_nonce_r;   /* 响应方（RX）的 nonce */
	uint16_t sess_hop;       /* 这次会话的跳频索引 */
	uint32_t pair_nonce;     /* 配对：发起方 nonce_a */
	uint32_t pair_nonce_b;   /* 配对：响应方 nonce_b */

	/* 状态与计时 */
	uint8_t state;
	uint32_t state_ms;
	uint32_t last_rx_ms;
	uint32_t last_beacon_ms;
	uint32_t last_pair_tx_ms;
	bool pair_cfm_pending;  /* TX 收到 PAIR_RESP，下一个下行时隙发 PAIR_CFM */
	bool pair_wait_cfm;     /* RX 已回 PAIR_RESP，等 PAIR_CFM */
	/*
	 * 简化握手（cfg.handshake = 0）：RX 收到 BEACON 就进 CONNECTED，但要反复
	 * 回 SESS_RESP 直到收到对端的第一帧——否则对端可能还在 SEARCHING。
	 */
	bool announce;

	/* 序号与跳频 */
	uint16_t tx_seq;
	uint16_t last_rx_seq;
	uint16_t lq_last_seq;
	uint16_t frame_index; /* 本链路走了多少个"主时隙"（控制 CTL_DOWN / 数据 DATA_A） */
	zlrs_fhss_t fhss;

	/* 载荷 */
	uint8_t rc_payload[ZLRS_RC_PAYLOAD_LEN];
	uint16_t rc_len;
	uint8_t tlm_payload[ZLRS_LINK_TLM_MAX_LEN];
	uint16_t tlm_len;
	/* 失控值：TX 侧设的要下发给 RX，RX 侧存的是最近一次收到的 */
	uint8_t failsafe[ZLRS_RC_PAYLOAD_LEN];
	uint16_t failsafe_len;

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

/* 运行中换调制档（速率自适应用），下一个事务生效 */
void zlrs_link_set_mod(zlrs_link_t *link, const zlrs_mod_cfg_t *mod);

/* 运行中换发射功率（动态功率用），下一个事务生效 */
void zlrs_link_set_power(zlrs_link_t *link, int8_t tx_power_dbm);

/*
 * 从持久化记录恢复配对（两端重启后不必重新配对）。pair_salt 必须与对端存下来的
 * 那一份一致，它是配对时交换的两个 nonce；key 从两者重算，不落盘。
 */
void zlrs_link_set_peer(zlrs_link_t *link, const uint8_t peer_id[ZLRS_DEV_ID_LEN],
			const uint8_t pair_salt[ZLRS_PAIR_SALT_LEN], uint32_t now_ms);

/* 按键：长按触发配对（换绑也走这里，新记录顶掉旧的） */
void zlrs_link_press(zlrs_link_t *link, uint32_t now_ms);

/* TX 侧设置配对时下发给 RX 的失控值（RC 载荷，见 doc/04-protocol.md 3.4） */
void zlrs_link_set_failsafe(zlrs_link_t *link, const uint8_t *payload, uint16_t len);

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


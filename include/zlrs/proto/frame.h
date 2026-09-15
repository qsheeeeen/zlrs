/*
 * 空口帧格式，见 doc/03-protocol.md 第 2 节：
 *
 *   header(1) | seq(2) | payload(0..N) | tag(0/4)
 *   header: 高 6 位 = 类型，低 2 位 = 标志
 *
 * 前导码、同步字、CRC 与长度字段由 LR2021 的 FLRC 硬件负责，帧里不重复携带。
 * 多字节字段一律小端。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_PROTO_FRAME_H_
#define ZLRS_PROTO_FRAME_H_

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 包类型（6 位空间，见 doc/03-protocol.md 2.2） */
typedef enum {
	ZLRS_PKT_RC = 0,       /* TX -> RX：遥控通道 */
	ZLRS_PKT_TLM = 1,      /* RX -> TX：遥测 */
	ZLRS_PKT_DATA_UP = 2,  /* TX -> RX：透传数据 */
	ZLRS_PKT_DATA_DOWN = 3,/* RX -> TX：透传数据 */
	ZLRS_PKT_ACK = 4,      /* 双向：透传确认（ARQ profile） */
	ZLRS_PKT_BEACON = 5,   /* TX -> RX：会话建立 */
	ZLRS_PKT_PAIR_REQ = 6, /* TX -> RX：配对 */
	ZLRS_PKT_PAIR_RESP = 7,/* RX -> TX：配对 */
	ZLRS_PKT_PAIR_CFM = 8, /* TX -> RX：配对 */
} zlrs_pkt_type_t;

/* header 低 2 位 */
#define ZLRS_FLAG_MORE 0x01U    /* 分片未结束 */
#define ZLRS_FLAG_ACK_REQ 0x02U /* 请求确认 */

/* 安全 profile，见 doc/02-security.md 第 8 节 */
typedef enum {
	ZLRS_PROFILE_P0 = 0, /* 无 tag、无加密，仅调试 */
	ZLRS_PROFILE_P1 = 1, /* 4 B tag */
	ZLRS_PROFILE_P2 = 2, /* 4 B tag + AES-CTR 载荷加密 */
} zlrs_profile_t;

/* 该 profile 的 tag 长度 */
uint8_t zlrs_profile_tag_len(zlrs_profile_t profile);

/* 一帧的解析视图；tag 为 NULL 表示该帧不带 tag */
typedef struct {
	uint8_t type;
	uint8_t flags;
	uint16_t seq;
	const uint8_t *payload;
	uint16_t payload_len;
	const uint8_t *tag;
} zlrs_frame_t;

/* 该载荷长度 + tag 长度编码成多少字节 */
uint16_t zlrs_frame_len(uint16_t payload_len, uint8_t tag_len);

/*
 * 编码到 buf，返回写入长度；容量不足返回 0。
 * frame->tag 为 NULL 时按 tag_len = 0 编码。
 */
uint16_t zlrs_frame_encode(const zlrs_frame_t *frame, uint8_t *buf, uint16_t cap);

/*
 * 解码。len 是射频收到的字节数，tag_len 由当前 profile 决定。
 * 长度不足以容纳 header/tag 或类型越界时返回 false。
 */
bool zlrs_frame_decode(zlrs_frame_t *frame, const uint8_t *buf, uint16_t len,
		       uint8_t tag_len);

/* tag 覆盖的字节数：header + seq + payload */
uint16_t zlrs_frame_mac_len(const zlrs_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_PROTO_FRAME_H_ */

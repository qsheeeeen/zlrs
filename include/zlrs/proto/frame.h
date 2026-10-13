/* One typed air frame; its codec owns the byte layout.
 * Preamble, sync word, length and physical CRC belong to LR2021.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "zlrs/proto/limits.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	ZLRS_PKT_DATA_UP = 0,
	ZLRS_PKT_DATA_DOWN = 1,
	ZLRS_PKT_SYNC = 2,
	ZLRS_PKT_PAIR_REQ = 3,
	ZLRS_PKT_PAIR_RESP = 4,
	ZLRS_PKT_PAIR_CFM = 5,
	ZLRS_PKT_SESS_RESP = 6,
	ZLRS_PKT_SESS_CFM = 7,
} zlrs_pkt_type_t;

#define ZLRS_FRAME_TYPE_BITS 3U
#define ZLRS_FRAME_TYPE_MASK 0x07U
#define ZLRS_SYNC_PROFILE_SHIFT 3U
#define ZLRS_SYNC_VERSION_SHIFT 4U
#define ZLRS_DATA_ACK_MASK 0x01U
#define ZLRS_DATA_SEQ_SHIFT 1U
#define ZLRS_DATA_SEQ_MASK 0x7FU

typedef enum {
	ZLRS_PROFILE_P0 = 0, /* no authentication, debug only */
	ZLRS_PROFILE_P1 = 1, /* 4-byte authentication tag */
	ZLRS_PROFILE_P2 = 2, /* payload encryption remains unimplemented */
} zlrs_profile_t;

/* Typed bodies contain only packet-specific fields. DATA owns its bytes; its
 * used length comes from the radio payload length, never an on-air size field. */
typedef struct {
	uint8_t ack : 1; /* reverse-direction ACK */
	uint8_t data_seq : 7;
	uint8_t rssi_magnitude;
	uint8_t lq_percent;
	uint8_t bytes[ZLRS_DATA_MAX_LEN];
} zlrs_data_frame_t;

typedef struct {
	uint8_t profile : 1;
	uint8_t version : 4;
	uint8_t hop_index;
	uint32_t nonce_t;
	uint32_t frame_index; /* shared TDD period count, including idle periods */
} zlrs_sync_frame_t;

typedef struct {
	uint8_t dev_a[ZLRS_DEV_ID_LEN];
	uint32_t nonce_a;
} zlrs_pair_request_frame_t;

typedef struct {
	uint8_t dev_a[ZLRS_DEV_ID_LEN];
	uint8_t dev_b[ZLRS_DEV_ID_LEN];
	uint32_t nonce_a;
	uint32_t nonce_b;
} zlrs_pair_frame_t;

typedef struct {
	uint32_t nonce_t;
	uint32_t nonce_r;
} zlrs_session_response_frame_t;

typedef struct {
	uint32_t nonce_r;
	uint8_t hop_index;
	uint32_t frame_index; /* shared TDD period count */
} zlrs_session_confirm_frame_t;

typedef struct {
	uint8_t type : ZLRS_FRAME_TYPE_BITS;
	union {
		zlrs_data_frame_t data; /* DATA_UP / DATA_DOWN share this body layout */
		zlrs_sync_frame_t sync;
		zlrs_pair_request_frame_t pair_req;
		zlrs_pair_frame_t pair; /* PAIR_RESP / PAIR_CFM validate all four fields */
		zlrs_session_response_frame_t sess_resp;
		zlrs_session_confirm_frame_t sess_cfm;
	} body;
	uint32_t seq; /* common per-direction counter; zero is never transmitted */
	uint8_t tag[ZLRS_TAG_LEN];
} zlrs_frame_t;

uint8_t zlrs_profile_tag_len(zlrs_profile_t profile);
uint16_t zlrs_frame_len(uint16_t payload_len, uint8_t tag_len);
/* len is the complete radio payload length, including common fields and tag.
 * Fixed bodies require exact lengths. DATA length is inferred from len.
 * Only the selected body's used bytes are encoded; unused capacity is omitted. */
uint16_t zlrs_frame_encode(const zlrs_frame_t *frame, uint8_t *buf, uint16_t cap,
			   uint16_t len, uint8_t tag_len);
bool zlrs_frame_decode(zlrs_frame_t *frame, const uint8_t *buf, uint16_t len, uint8_t tag_len);
#ifdef __cplusplus
}
#endif

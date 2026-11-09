/* One typed air frame; its codec owns the byte layout.
 * Preamble, sync word, length and physical CRC belong to LR2021.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "proto/limits.h"
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/dt-bindings/zlrs/region.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	ZLRS_PKT_DATA_L2F = 0,
	ZLRS_PKT_DATA_F2L = 1,
	ZLRS_PKT_SYNC = 2,
	ZLRS_PKT_PAIR_REQ = 3,
	ZLRS_PKT_PAIR_RESP = 4,
	ZLRS_PKT_PAIR_CFM = 5,
	ZLRS_PKT_SESS_RESP = 6,
	ZLRS_PKT_SESS_CFM = 7,
} zlrs_pkt_type_t;

#define ZLRS_FRAME_TYPE_BITS    3U
#define ZLRS_FRAME_TYPE_MASK    0x07U
#define ZLRS_SYNC_PROFILE_SHIFT 3U
#define ZLRS_SYNC_VERSION_SHIFT 4U
#define ZLRS_DATA_ACK_MASK      0x01U
#define ZLRS_DATA_SEQ_SHIFT     1U
#define ZLRS_DATA_SEQ_MASK      0x7FU
#define ZLRS_LQ_TOGGLE_MASK     0x80U
#define ZLRS_LQ_VALUE_MASK      0x7FU
#define ZLRS_LQ_UNAVAILABLE     0x7FU
#define ZLRS_LQ_WINDOW_FRAMES   128U

typedef enum {
	ZLRS_PROFILE_P0 = 0, /* no authentication, debug only */
	ZLRS_PROFILE_P1 = 1, /* 4-byte authentication tag */
	ZLRS_PROFILE_P2 = 2, /* payload encryption remains unimplemented */
} zlrs_profile_t;

typedef enum {
	ZLRS_AUTH_ROOT,
	ZLRS_AUTH_LINK,
	ZLRS_AUTH_SESSION,
	ZLRS_AUTH_DATA,
} zlrs_auth_t;

typedef struct {
	uint8_t payload_len; /* DATA prefix minimum; fixed length for control packets. */
	zlrs_auth_t auth;
} zlrs_packet_layout_t;

extern const zlrs_packet_layout_t zlrs_packet_layouts[ZLRS_FRAME_TYPE_MASK + 1U];

/* Committed peer identity and handshake salt, shared by link and persistence. */
typedef struct {
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	uint8_t pair_salt[ZLRS_PAIR_SALT_LEN];
} zlrs_pairing_t;

/* DATA metadata and a borrowed payload view. Authentication stays in packet storage. */
typedef struct {
	uint8_t data_seq;
	bool ack;
	uint8_t rssi_magnitude;
	uint8_t lq_percent;
	bool lq_toggle;
} zlrs_data_header_t;

typedef struct {
	zlrs_data_header_t header;
	const uint8_t *bytes;
	uint16_t len;
} zlrs_data_view_t;

/* Present only in authenticated synchronization/confirmation bodies. */
typedef struct {
	uint8_t region: 3;
	uint8_t automatic: 1;
	uint8_t target_subg: 1;
	uint8_t phy: 3;        /* current 2.4 GHz profile; zero on SubG */
	uint8_t target_phy: 3; /* target 2.4 GHz profile; zero for target SubG */
	uint32_t switch_frame; /* zero = no transition; otherwise a future frame */
} zlrs_band_plan_t;

typedef struct {
	uint8_t profile: 1;
	uint8_t version: 4;
	uint8_t hop_index;
	uint32_t nonce;       /* Leader challenge in SYNC, follower proof in SESS_CFM. */
	uint32_t frame_index; /* shared TDD period count, including idle periods */
	zlrs_band_plan_t band_plan;
} zlrs_schedule_frame_t;

typedef struct {
	uint8_t dev_a[ZLRS_DEV_ID_LEN];
	uint8_t dev_b[ZLRS_DEV_ID_LEN];
	uint32_t nonce_a;
	uint32_t nonce_b;
} zlrs_pair_frame_t;

typedef struct {
	uint32_t nonce_leader;
	uint32_t nonce_follower;
} zlrs_session_response_frame_t;

typedef struct {
	uint8_t type: ZLRS_FRAME_TYPE_BITS;

	union {
		zlrs_data_view_t data; /* Borrowed DATA metadata and payload. */
		zlrs_schedule_frame_t schedule;
		zlrs_pair_frame_t pair; /* PAIR_RESP / PAIR_CFM validate all four fields */
		zlrs_session_response_frame_t sess_resp;
	} body;

	uint32_t seq; /* common per-direction counter; zero is never transmitted */
} zlrs_frame_t;

uint8_t zlrs_profile_tag_len(zlrs_profile_t profile);
uint16_t zlrs_frame_len(uint16_t payload_len, uint8_t tag_len);
/* len is the complete radio payload length, including common fields and tag.
 * Encoding writes headers and control bodies; DATA payload and authentication
 * storage remain untouched. The link computes/verifies tags in packet storage.
 * Fixed bodies require exact lengths.
 * Only the selected body's used bytes are encoded; unused capacity is omitted. */
uint16_t zlrs_frame_encode(const zlrs_frame_t *frame, uint8_t *buf, uint16_t cap, uint16_t len,
			   uint8_t tag_len);
bool zlrs_frame_decode(zlrs_frame_t *frame, const uint8_t *buf, uint16_t len, uint8_t tag_len);
#ifdef __cplusplus
}
#endif

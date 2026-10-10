/*
 * Over-the-air frame format, see doc/08-protocol.md 2. Preamble, sync word, CRC and length
 * belong to the LR2021 hardware and are not repeated in the frame; multi-byte fields are little-endian.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/limits.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Packet types (a 6-bit space, see doc/08-protocol.md 2.2) */
typedef enum {
	ZLRS_PKT_DATA_UP = 0,   /* TX -> RX: passthrough data */
	ZLRS_PKT_DATA_DOWN = 1, /* RX -> TX: passthrough data */
	ZLRS_PKT_SYNC = 2,      /* TX -> RX: session setup */
	ZLRS_PKT_PAIR_REQ = 3,  /* TX -> RX: pairing */
	ZLRS_PKT_PAIR_RESP = 4, /* RX -> TX: pairing */
	ZLRS_PKT_PAIR_CFM = 5,  /* TX -> RX: pairing */
	ZLRS_PKT_SESS_RESP = 6, /* RX -> TX: session response carrying its nonce */
	ZLRS_PKT_SESS_CFM = 7,  /* TX -> RX: session confirm, tagged with the new session key */
} zlrs_pkt_type_t;

/* Header bit 0 is reserved and must be zero. */
#define ZLRS_FLAG_ACK 0x02U /* toggles after each accepted packet in this reliable service */

/* Security profiles, see doc/09-security.md 8 */
typedef enum {
	ZLRS_PROFILE_P0 = 0, /* no tag, no encryption, debug only */
	ZLRS_PROFILE_P1 = 1, /* 4 B tag */
	ZLRS_PROFILE_P2 = 2, /* 4 B tag plus AES-CTR payload encryption */
} zlrs_profile_t;

/* tag length of that profile */
uint8_t zlrs_profile_tag_len(zlrs_profile_t profile);

/* Parsed view of a frame; a NULL tag means the frame carries none */
typedef struct {
	uint8_t type;
	uint8_t flags;
	uint16_t seq;
	const uint8_t *payload;
	uint16_t payload_len;
	const uint8_t *tag;
} zlrs_frame_t;

/* How many bytes this payload plus tag length encode to */
uint16_t zlrs_frame_len(uint16_t payload_len, uint8_t tag_len);

/*
 * Encode into buf and return the length written; 0 when the buffer is too small.
 * A NULL frame->tag encodes as tag_len = 0.
 */
uint16_t zlrs_frame_encode(const zlrs_frame_t *frame, uint8_t *buf, uint16_t cap);

/*
 * Decode. len is what the radio received, tag_len comes from the current profile.
 * Returns false when the length cannot hold header and tag or the type is out of range.
 */
bool zlrs_frame_decode(zlrs_frame_t *frame, const uint8_t *buf, uint16_t len, uint8_t tag_len);

/* Bytes the tag covers: header + seq + payload */
uint16_t zlrs_frame_mac_len(const zlrs_frame_t *frame);

#ifdef __cplusplus
}
#endif

/*
 * Protocol core limits and constants: frame sizes, key lengths, the sync slots, the role
 * values — everything that is fixed at compile time. Layering: core/proto and core/link depend on
 * no Zephyr and run in host tests; the Zephyr glue lives in app/ only.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* Current wire format only; SYNC must declare this exact version. */
#define ZLRS_PROTO_VERSION 12U

/* Frame: type/config(1) + common seq(4) + typed body + tag(0/4). */
#define ZLRS_FRAME_HDR_LEN   5U
#define ZLRS_TAG_LEN         4U
#define ZLRS_FRAME_MAX_LEN   511U
#define ZLRS_PAYLOAD_MAX_LEN (ZLRS_FRAME_MAX_LEN - ZLRS_FRAME_HDR_LEN)

/* DATA-only prefix: packed 7-bit sequence / 1-bit ACK, RSSI and LQ. */
#define ZLRS_DATA_PREFIX_LEN       3U
#define ZLRS_DATA_MAX_LEN          (ZLRS_PAYLOAD_MAX_LEN - ZLRS_DATA_PREFIX_LEN)
#define ZLRS_SYNC_PAYLOAD_LEN      15U
#define ZLRS_SESS_RESP_PAYLOAD_LEN 8U
#define ZLRS_SESS_CFM_PAYLOAD_LEN  15U

/* Sync words: 4 bytes, hardware slot 1 DATA, slot 2 pairing */
#define ZLRS_SYNC_WORD_LEN     4U
#define ZLRS_SYNC_SLOT_DATA    1U
#define ZLRS_SYNC_SLOT_PAIRING 2U

/* Hop table length, must be a multiple of the channel count */
#define ZLRS_FHSS_SEQUENCE_LEN 256U

/* keys and device identity */
#define ZLRS_KEY_LEN    32U
#define ZLRS_DEV_ID_LEN 4U

/* The two pairing nonces (initiator || responder): feed pair_key and are stored */
#define ZLRS_PAIR_SALT_LEN 8U

/* Timing/coordination roles; both endpoints transmit and receive DATA. */
typedef enum {
	ZLRS_ROLE_LEADER = 0,
	ZLRS_ROLE_FOLLOWER = 1
} zlrs_role_t;

/* Request sends only the initiator ID/nonce. Response/confirm bind both ends. */
#define ZLRS_PAIR_REQ_PAYLOAD_LEN 8U
#define ZLRS_PAIR_PAYLOAD_LEN     16U

/*
 * Compile-time configuration of the protocol core. Layering: lib/proto and lib/link depend
 * on no Zephyr and run in host tests; the Zephyr glue lives in app/ only.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Over-the-air protocol version: bump on incompatible changes, carried in SYNC, RX checks it.
 * 1: tags and session keys derive from both dev_ids (order encodes direction), pair_key mixes
 *    in the pairing nonce and session keys mix in the handshake nonces (10-security 3).
 */
#define ZLRS_PROTO_VERSION 1U

/* Frame: header(1) + seq(2) + payload + tag(0/4); the radio hardware produces the CRC */
#define ZLRS_FRAME_HDR_LEN   3U
#define ZLRS_TAG_LEN         4U
#define ZLRS_FRAME_MAX_LEN   255U
#define ZLRS_PAYLOAD_MAX_LEN (ZLRS_FRAME_MAX_LEN - ZLRS_FRAME_HDR_LEN - ZLRS_TAG_LEN)

/*
 * Reassembly timeout. ARQ retransmits every 30 ms, so leave enough headroom or a retransmit
 * lets the reassembler discard a half-finished message.
 */
#define ZLRS_FRAG_TIMEOUT_MS 100U
#define ZLRS_REASM_BUF_LEN   512U

/* Sync words: 4 bytes, slot 1 control link, slot 2 data link, slot 3 pairing */
#define ZLRS_SYNC_WORD_LEN     4U
#define ZLRS_SYNC_SLOT_CONTROL 1U
#define ZLRS_SYNC_SLOT_DATA    2U
#define ZLRS_SYNC_SLOT_PAIRING 3U

/* Hop table length, must be a multiple of the channel count */
#define ZLRS_FHSS_SEQUENCE_LEN 256U

/* keys and device identity */
#define ZLRS_KEY_LEN    32U
#define ZLRS_DEV_ID_LEN 4U

/* The two pairing nonces (initiator || responder): feed pair_key and are stored */
#define ZLRS_PAIR_SALT_LEN 8U

/* device roles */
#define ZLRS_ROLE_TX 0U
#define ZLRS_ROLE_RX 1U

/* default control link frame rate (Hz) */
#define ZLRS_FRAME_RATE_HZ 150U

/* Control link RC payload: CRSF's 22 bytes of channel data carried as-is */
#define ZLRS_RC_PAYLOAD_LEN 22U

/* Failsafe values in the config protocol: that same 22-byte CRSF channel payload */
#define ZLRS_CFG_FAILSAFE_LEN ZLRS_RC_PAYLOAD_LEN

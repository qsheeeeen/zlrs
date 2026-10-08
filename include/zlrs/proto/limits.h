/*
 * Protocol core limits and constants: frame sizes, key lengths, the sync slots, the role
 * values — everything that is fixed at compile time. Layering: core/proto and core/link depend on
 * no Zephyr and run in host tests; the Zephyr glue lives in app/ only.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* Current wire format only; SYNC must declare this exact version. */
#define ZLRS_PROTO_VERSION 5U

/* Frame: common header(1) + seq(2) + payload + tag(0/4). */
#define ZLRS_FRAME_HDR_LEN      3U
#define ZLRS_TAG_LEN            4U
#define ZLRS_LORA_FRAME_MAX_LEN 255U
#define ZLRS_FRAME_MAX_LEN      511U
#define ZLRS_PAYLOAD_MAX_LEN    (ZLRS_FRAME_MAX_LEN - ZLRS_FRAME_HDR_LEN - ZLRS_TAG_LEN)

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

/* Pairing payload: endpoint IDs and nonces. */
#define ZLRS_PAIR_PAYLOAD_LEN 16U

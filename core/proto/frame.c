/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/proto/frame.h"

#include <string.h>

#define FRAME_TYPE_SHIFT 2U
#define FRAME_FLAG_MASK  0x03U
#define FRAME_TYPE_MAX   0x3FU

uint8_t zlrs_profile_tag_len(zlrs_profile_t profile)
{
	return (profile == ZLRS_PROFILE_P0) ? 0U : ZLRS_TAG_LEN;
}

uint16_t zlrs_frame_len(uint16_t payload_len, uint8_t tag_len)
{
	return (uint16_t)(ZLRS_FRAME_HDR_LEN + payload_len + tag_len);
}

uint16_t zlrs_frame_encode(const zlrs_frame_t *frame, uint8_t *buf, uint16_t cap)
{
	const uint8_t tag_len = (frame->tag != NULL) ? ZLRS_TAG_LEN : 0U;
	const bool ack_ext = frame->ack_present && ((frame->type == ZLRS_PKT_DATA_UP) ||
						    (frame->type == ZLRS_PKT_DATA_DOWN));
	/* Validate the wide sum before narrowing: a forged payload length must not wrap. */
	const uint32_t len =
		ZLRS_FRAME_HDR_LEN + (uint32_t)frame->payload_len + tag_len + (ack_ext ? 2U : 0U);

	if (frame->type > FRAME_TYPE_MAX || len > cap || len > ZLRS_FRAME_MAX_LEN) {
		return 0U;
	}
	if (frame->payload_len > 0U && frame->payload == NULL) {
		return 0U;
	}

	buf[0] = (uint8_t)((frame->type << FRAME_TYPE_SHIFT) | (frame->flags & FRAME_FLAG_MASK));
	buf[1] = (uint8_t)(frame->seq & 0xFFU);
	buf[2] = (uint8_t)(frame->seq >> 8);
	if (ack_ext) {
		buf[3] = (uint8_t)(frame->ack_seq & 0xFFU);
		buf[4] = (uint8_t)(frame->ack_seq >> 8);
	}

	if (frame->payload_len > 0U) {
		memcpy(&buf[ZLRS_FRAME_HDR_LEN + (ack_ext ? 2U : 0U)], frame->payload,
		       frame->payload_len);
	}
	if (tag_len > 0U) {
		memcpy(&buf[ZLRS_FRAME_HDR_LEN + (ack_ext ? 2U : 0U) + frame->payload_len],
		       frame->tag, tag_len);
	}

	return len;
}

bool zlrs_frame_decode(zlrs_frame_t *frame, const uint8_t *buf, uint16_t len, uint8_t tag_len)
{
	if (len < ZLRS_FRAME_HDR_LEN || len > ZLRS_FRAME_MAX_LEN || tag_len > ZLRS_TAG_LEN) {
		return false;
	}
	if (len < (uint16_t)(ZLRS_FRAME_HDR_LEN + tag_len)) {
		return false;
	}

	frame->type = (uint8_t)(buf[0] >> FRAME_TYPE_SHIFT);
	frame->flags = (uint8_t)(buf[0] & FRAME_FLAG_MASK);
	frame->seq = (uint16_t)(buf[1] | ((uint16_t)buf[2] << 8));
	frame->ack_present =
		((frame->flags & ZLRS_FLAG_ACK) != 0U) &&
		((frame->type == ZLRS_PKT_DATA_UP) || (frame->type == ZLRS_PKT_DATA_DOWN));
	if (frame->ack_present && len < (uint16_t)(ZLRS_FRAME_HDR_LEN + 2U + tag_len)) {
		return false;
	}
	frame->ack_seq = frame->ack_present ? (uint16_t)(buf[3] | ((uint16_t)buf[4] << 8)) : 0U;
	frame->payload_len =
		(uint16_t)(len - ZLRS_FRAME_HDR_LEN - tag_len - (frame->ack_present ? 2U : 0U));
	frame->payload = (frame->payload_len > 0U)
				 ? &buf[ZLRS_FRAME_HDR_LEN + (frame->ack_present ? 2U : 0U)]
				 : NULL;
	frame->tag = (tag_len > 0U) ? &buf[ZLRS_FRAME_HDR_LEN + (frame->ack_present ? 2U : 0U) +
					   frame->payload_len]
				    : NULL;

	return true;
}

uint16_t zlrs_frame_mac_len(const zlrs_frame_t *frame)
{
	return (uint16_t)(ZLRS_FRAME_HDR_LEN + (frame->ack_present ? 2U : 0U) + frame->payload_len);
}

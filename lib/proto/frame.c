/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/proto/frame.h"

#include <string.h>

#define FRAME_TYPE_SHIFT 2U
#define FRAME_FLAG_MASK 0x03U
#define FRAME_TYPE_MAX 0x3FU

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
	const uint16_t len = zlrs_frame_len(frame->payload_len, tag_len);

	if (frame->type > FRAME_TYPE_MAX || len > cap) {
		return 0U;
	}
	if (frame->payload_len > 0U && frame->payload == NULL) {
		return 0U;
	}

	buf[0] = (uint8_t)((frame->type << FRAME_TYPE_SHIFT) |
			   (frame->flags & FRAME_FLAG_MASK));
	buf[1] = (uint8_t)(frame->seq & 0xFFU);
	buf[2] = (uint8_t)(frame->seq >> 8);

	if (frame->payload_len > 0U) {
		memcpy(&buf[ZLRS_FRAME_HDR_LEN], frame->payload, frame->payload_len);
	}
	if (tag_len > 0U) {
		memcpy(&buf[ZLRS_FRAME_HDR_LEN + frame->payload_len], frame->tag, tag_len);
	}

	return len;
}

bool zlrs_frame_decode(zlrs_frame_t *frame, const uint8_t *buf, uint16_t len,
		       uint8_t tag_len)
{
	if (len < ZLRS_FRAME_HDR_LEN || tag_len > ZLRS_TAG_LEN) {
		return false;
	}
	if (len < (uint16_t)(ZLRS_FRAME_HDR_LEN + tag_len)) {
		return false;
	}

	frame->type = (uint8_t)(buf[0] >> FRAME_TYPE_SHIFT);
	frame->flags = (uint8_t)(buf[0] & FRAME_FLAG_MASK);
	frame->seq = (uint16_t)(buf[1] | ((uint16_t)buf[2] << 8));
	frame->payload_len = (uint16_t)(len - ZLRS_FRAME_HDR_LEN - tag_len);
	frame->payload = (frame->payload_len > 0U) ? &buf[ZLRS_FRAME_HDR_LEN] : NULL;
	frame->tag = (tag_len > 0U) ? &buf[ZLRS_FRAME_HDR_LEN + frame->payload_len] : NULL;

	return true;
}

uint16_t zlrs_frame_mac_len(const zlrs_frame_t *frame)
{
	return (uint16_t)(ZLRS_FRAME_HDR_LEN + frame->payload_len);
}

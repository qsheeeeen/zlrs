/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test.h"
#include <string.h>

#include "zlrs/proto/frame.h"

/* ---- frame format -------------------------------------------------------- */

void test_frame(void)
{
	uint8_t buf[ZLRS_FRAME_MAX_LEN];
	uint8_t payload[48U];
	uint8_t tag[ZLRS_TAG_LEN] = {0xde, 0xad, 0xbe, 0xef};
	zlrs_frame_t out;
	zlrs_frame_t in = {
		.type = ZLRS_PKT_DATA_UP,
		.flags = ZLRS_FLAG_ACK,
		.seq = 0x0102U,
		.payload = payload,
		.payload_len = sizeof(payload),
		.tag = tag,
	};
	uint16_t len;

	printf("frame\n");

	memset(payload, 0x5a, sizeof(payload));
	CHECK(zlrs_profile_tag_len(ZLRS_PROFILE_P0) == 0U);
	CHECK(zlrs_profile_tag_len(ZLRS_PROFILE_P1) == ZLRS_TAG_LEN);
	CHECK(zlrs_profile_tag_len(ZLRS_PROFILE_P2) == ZLRS_TAG_LEN);

	len = zlrs_frame_encode(&in, buf, sizeof(buf));
	CHECK(len == ZLRS_FRAME_HDR_LEN + sizeof(payload) + ZLRS_TAG_LEN);
	CHECK(buf[0] == (uint8_t)((ZLRS_PKT_DATA_UP << 1) | ZLRS_FLAG_ACK));
	CHECK(buf[1] == 0x02U && buf[2] == 0x01U); /* little-endian */
	CHECK(buf[3] == 0x5aU);
	CHECK(memcmp(&buf[ZLRS_FRAME_HDR_LEN + sizeof(payload)], tag, sizeof(tag)) == 0);

	CHECK(zlrs_frame_decode(&out, buf, len, ZLRS_TAG_LEN));
	CHECK(out.type == in.type && out.flags == in.flags && out.seq == in.seq);
	CHECK(out.payload_len == sizeof(payload));
	CHECK(out.payload != NULL && memcmp(out.payload, payload, sizeof(payload)) == 0);
	CHECK(out.tag != NULL && memcmp(out.tag, tag, sizeof(tag)) == 0);
	CHECK(zlrs_frame_mac_len(&out) == ZLRS_FRAME_HDR_LEN + sizeof(payload));
	/* Unknown packet types and flags must be rejected. */
	buf[0] = 8U << 1;
	CHECK(!zlrs_frame_decode(&out, buf, len, ZLRS_TAG_LEN));
	buf[0] = ZLRS_FLAG_ACK;
	in.flags |= 0x02U;
	CHECK(zlrs_frame_encode(&in, buf, sizeof(buf)) == 0U);
	in.flags = ZLRS_FLAG_ACK;

	/* P0: no tag */
	CHECK(zlrs_frame_decode(&out, buf, ZLRS_FRAME_HDR_LEN + sizeof(payload), 0U));
	CHECK(out.tag == NULL && out.payload_len == sizeof(payload));

	/* length and capacity edges */
	CHECK(!zlrs_frame_decode(&out, buf, 2U, 0U));
	CHECK(!zlrs_frame_decode(&out, buf, len, (uint8_t)(ZLRS_TAG_LEN + 1U)));
	CHECK(zlrs_frame_encode(&in, buf, 4U) == 0U);

	/* The ACK toggle stays in the authenticated header and consumes no payload bytes. */
	in.type = ZLRS_PKT_DATA_DOWN;
	in.flags = ZLRS_FLAG_ACK;
	len = zlrs_frame_encode(&in, buf, sizeof(buf));
	CHECK(len == ZLRS_FRAME_HDR_LEN + sizeof(payload) + ZLRS_TAG_LEN);
	CHECK(buf[3] == payload[0]);
	CHECK(zlrs_frame_decode(&out, buf, len, ZLRS_TAG_LEN));
	CHECK((out.flags & ZLRS_FLAG_ACK) != 0U);
	CHECK(out.payload_len == sizeof(payload) &&
	      memcmp(out.payload, payload, sizeof(payload)) == 0);
	CHECK(zlrs_frame_mac_len(&out) == ZLRS_FRAME_HDR_LEN + sizeof(payload));
	in.payload = NULL;
	in.payload_len = 0U;
	len = zlrs_frame_encode(&in, buf, sizeof(buf));
	CHECK(len == ZLRS_FRAME_HDR_LEN + ZLRS_TAG_LEN);
	CHECK(zlrs_frame_decode(&out, buf, len, ZLRS_TAG_LEN));
	CHECK((out.flags & ZLRS_FLAG_ACK) != 0U && out.payload_len == 0U && out.payload == NULL);

	/* Full FLRC capacity with a tag and ACK; invalid lengths cannot wrap the sum. */
	uint8_t large_payload[ZLRS_PAYLOAD_MAX_LEN];

	memset(large_payload, 0xa5, sizeof(large_payload));
	in.payload = large_payload;
	in.payload_len = sizeof(large_payload);
	in.tag = tag;
	len = zlrs_frame_encode(&in, buf, sizeof(buf));
	CHECK(len == 511U);
	CHECK(zlrs_frame_decode(&out, buf, len, ZLRS_TAG_LEN));
	CHECK(out.payload_len == sizeof(large_payload));
	CHECK(memcmp(out.payload, large_payload, sizeof(large_payload)) == 0);
	CHECK(out.flags == in.flags);
	CHECK(zlrs_frame_encode(&in, buf, sizeof(buf) - 1U) == 0U);
	in.payload_len = UINT16_MAX;
	CHECK(zlrs_frame_encode(&in, buf, sizeof(buf)) == 0U);
	CHECK(!zlrs_frame_decode(&out, buf, sizeof(buf) + 1U, ZLRS_TAG_LEN));
	buf[0] = 0xFCU;
	CHECK(!zlrs_frame_decode(&out, buf, len, ZLRS_TAG_LEN));
	in.type = 63U;
	CHECK(zlrs_frame_encode(&in, buf, sizeof(buf)) == 0U);
}

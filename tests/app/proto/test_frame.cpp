/* SPDX-License-Identifier: Apache-2.0 */
#include "../support/peer.h"
#include "proto/frame.h"
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string.h>
#include <zephyr/ztest.h>

static void check_wire(const zlrs_frame_t *view, const uint8_t *expected, uint16_t size)
{
	uint8_t buf[ZLRS_FRAME_MAX_LEN], again[ZLRS_FRAME_MAX_LEN];
	zlrs_frame_t decoded;
	const uint8_t tag[] = {0xde, 0xad, 0xbe, 0xef};
	memcpy(buf + size - sizeof(tag), tag, sizeof(tag));
	zassert_equal(zlrs_test_encode(view, buf, sizeof(buf), size, ZLRS_TAG_LEN), size);
	zexpect_equal(memcmp(buf, expected, size), 0);
	zassert_true(zlrs_frame_decode(&decoded, buf, size, ZLRS_TAG_LEN));
	zexpect_true(decoded.type == view->type && decoded.seq == 0x04030102U);
	memcpy(again + size - sizeof(tag), buf + size - sizeof(tag), sizeof(tag));
	zassert_equal(zlrs_test_encode(&decoded, again, sizeof(again), size, ZLRS_TAG_LEN), size);
	zexpect_equal(memcmp(buf, again, size), 0);
	if (view->type <= ZLRS_PKT_DATA_F2L) {
		zexpect_equal(+decoded.body.data.header.lq_percent,
			      +view->body.data.header.lq_percent);
		zexpect_equal(+decoded.body.data.header.lq_toggle,
			      +view->body.data.header.lq_toggle);
	}
	if (view->type != ZLRS_PKT_SYNC) {
		buf[0] |= 8U;
		zexpect_false(zlrs_frame_decode(&decoded, buf, size, ZLRS_TAG_LEN));
		buf[0] = expected[0];
	} else {
		zexpect_true(decoded.body.schedule.version == 13U &&
			     decoded.body.schedule.profile == 1U);
	}
	if (view->type >= ZLRS_PKT_SYNC) {
		zexpect_false(zlrs_frame_decode(&decoded, buf, size - 1U, ZLRS_TAG_LEN));
		zexpect_false(zlrs_frame_decode(&decoded, buf, size + 1U, ZLRS_TAG_LEN));
		zexpect_equal(zlrs_test_encode(view, buf, sizeof(buf), size - 1U, ZLRS_TAG_LEN),
			      0U);
		zexpect_equal(zlrs_test_encode(view, buf, sizeof(buf), size + 1U, ZLRS_TAG_LEN),
			      0U);
	}
	zlrs_frame_t debug = *view;
	if (debug.type == ZLRS_PKT_SYNC) {
		debug.body.schedule.profile = ZLRS_PROFILE_P0;
	}
	zassert_equal(zlrs_test_encode(&debug, buf, sizeof(buf), size - ZLRS_TAG_LEN, 0U),
		      size - ZLRS_TAG_LEN);
	zexpect_equal(buf[0], (view->type == ZLRS_PKT_SYNC ? 0xd2U : expected[0]));
	zexpect_equal(memcmp(buf + 1U, expected + 1U, size - ZLRS_TAG_LEN - 1U), 0);
	zassert_true(zlrs_frame_decode(&decoded, buf, size - ZLRS_TAG_LEN, 0U));
	zexpect_true(decoded.seq == view->seq);
	memset(buf + 1U, 0, 4U);
	zexpect_false(zlrs_frame_decode(&decoded, buf, size - ZLRS_TAG_LEN, 0U));
}

ZTEST(FrameCodec, test_InPlaceDataLeavesPayloadAndTagUntouchedAndReturnsBorrowedView)
{
	for (uint8_t tag_len : {0U, ZLRS_TAG_LEN}) {
		for (uint16_t payload = 0; payload <= ZLRS_DATA_MAX_LEN - tag_len; payload++) {
			uint8_t buffer[ZLRS_FRAME_MAX_LEN + 2];
			memset(buffer, 0xa5, sizeof(buffer));
			uint8_t *wire = buffer + 1; /* Deliberately unaligned, with canaries. */
			const zlrs_frame_t frame = {
				.type = ZLRS_PKT_DATA_F2L,
				.body = {.data = {.header = {.data_seq = 127,
							     .ack = true,
							     .rssi_magnitude = 200,
							     .lq_percent = 100,
							     .lq_toggle = true}}},
				.seq = UINT32_MAX};
			const uint16_t total =
				ZLRS_FRAME_HDR_LEN + ZLRS_DATA_PREFIX_LEN + payload + tag_len;
			zassert_equal(zlrs_frame_encode(&frame, wire, total, total, tag_len),
				      total);
			zexpect_equal(buffer[0], 0xa5);
			for (unsigned int n = 1 + ZLRS_FRAME_HDR_LEN + ZLRS_DATA_PREFIX_LEN;
			     n < sizeof(buffer); n++) {
				zassert_equal(buffer[n], 0xa5);
			}
			zlrs_frame_t decoded;
			zassert_true(zlrs_frame_decode(&decoded, wire, total, tag_len));
			const zlrs_data_view_t &view = decoded.body.data;
			zexpect_equal(view.bytes, wire + ZLRS_FRAME_HDR_LEN + ZLRS_DATA_PREFIX_LEN);
			zexpect_equal(view.len, payload);
			zexpect_equal(decoded.seq, UINT32_MAX);
			zexpect_equal(decoded.type, ZLRS_PKT_DATA_F2L);
			zexpect_equal(view.header.data_seq, 127);
			zexpect_true(view.header.ack);
			zexpect_equal(view.header.rssi_magnitude, 200);
			zexpect_equal(view.header.lq_percent, 100);
			zexpect_true(view.header.lq_toggle);
			zexpect_equal(zlrs_frame_encode(&frame, wire, total - 1, total, tag_len),
				      0);
		}
	}
}

ZTEST(FrameCodec, test_AllFrameTypesPreserveAuthenticationStorage)
{
	for (uint8_t type = 0; type <= ZLRS_FRAME_TYPE_MASK; type++) {
		for (uint8_t tag_len : {0U, ZLRS_TAG_LEN}) {
			uint8_t storage[ZLRS_FRAME_MAX_LEN + 2U];
			memset(storage, 0xa5, sizeof(storage));
			const uint16_t size =
				zlrs_frame_len(zlrs_packet_layouts[type].payload_len, tag_len);
			const zlrs_frame_t frame = {.type = type, .seq = 1U};
			zassert_equal(zlrs_frame_encode(&frame, storage + 1U, size, size, tag_len),
				      size);
			zexpect_equal(storage[0], 0xa5);
			for (size_t i = 1U + size - tag_len; i < sizeof(storage); i++) {
				zassert_equal(storage[i], 0xa5,
					      "type %u changed tag/canary byte %zu", type, i);
			}
		}
	}
}

ZTEST(FrameCodec, test_IndependentWireVectorsAndLengthBounds)
{
	const uint8_t payload[] = {0x5a, 0xa5};
	zlrs_frame_t frame = {.type = ZLRS_PKT_DATA_L2F,
			      .body = {.data = {.header = {.data_seq = 127,
							   .ack = true,
							   .rssi_magnitude = 200,
							   .lq_percent = 99,
							   .lq_toggle = true},
						.bytes = payload}},
			      .seq = 0x04030102};
	const uint8_t data[] = {0, 2, 1, 3, 4, 0xff, 200, 0xe3, 0x5a, 0xa5, 0xde, 0xad, 0xbe, 0xef};
	check_wire(&frame, data, sizeof(data));
	frame = zlrs_frame_t{.type = ZLRS_PKT_DATA_F2L,
			     .body = {.data = {.header = {.data_seq = 1, .lq_percent = 100}}},
			     .seq = 0x04030102};
	const uint8_t feedback[] = {1, 2, 1, 3, 4, 2, 0, 100, 0xde, 0xad, 0xbe, 0xef};
	check_wire(&frame, feedback, sizeof(feedback));
	frame = zlrs_frame_t{.type = ZLRS_PKT_SYNC,
			     .body = {.schedule = {.profile = 1,
						   .version = 13,
						   .hop_index = 254,
						   .nonce = 0x11223344,
						   .frame_index = 0x55667788,
						   .band_plan = {.region = 4,
								 .automatic = 1,
								 .phy = 1,
								 .target_phy = 4,
								 .switch_frame = 0x66778899}}},
			     .seq = 0x04030102};
	const uint8_t sync[] = {0xda, 2,    1,    3,    4,    254,  0x44, 0x33,
				0x22, 0x11, 0x88, 0x77, 0x66, 0x55, 0x0c, 0x99,
				0x88, 0x77, 0x66, 0x21, 0xde, 0xad, 0xbe, 0xef};
	check_wire(&frame, sync, sizeof(sync));
	frame = zlrs_frame_t{.type = ZLRS_PKT_PAIR_REQ,
			     .body = {.pair = {.dev_a = {1, 2, 3, 4}, .nonce_a = 0x11223344}},
			     .seq = 0x04030102};
	const uint8_t req[] = {3,    2,    1,    3,    4,    1,    2,    3,   4,
			       0x44, 0x33, 0x22, 0x11, 0xde, 0xad, 0xbe, 0xef};
	check_wire(&frame, req, sizeof(req));
	const zlrs_pair_frame_t pair_body = {.dev_a = {1, 2, 3, 4},
					     .dev_b = {5, 6, 7, 8},
					     .nonce_a = 0x11223344,
					     .nonce_b = 0x55667788};
	uint8_t pair[] = {4,    2,    1,    3,    4,    1,    2,    3,    4,    5,    6,    7,   8,
			  0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55, 0xde, 0xad, 0xbe, 0xef};
	for (uint8_t type = ZLRS_PKT_PAIR_RESP; type <= ZLRS_PKT_PAIR_CFM; type++) {
		frame = zlrs_frame_t{.type = type, .seq = 0x04030102};
		pair[0] = type;
		frame.body.pair = pair_body;
		check_wire(&frame, pair, sizeof(pair));
	}
	frame = zlrs_frame_t{
		.type = ZLRS_PKT_SESS_RESP,
		.body = {.sess_resp = {.nonce_leader = 0x11223344, .nonce_follower = 0x55667788}},
		.seq = 0x04030102};
	const uint8_t resp[] = {6,    2,    1,    3,    4,    0x44, 0x33, 0x22, 0x11,
				0x88, 0x77, 0x66, 0x55, 0xde, 0xad, 0xbe, 0xef};
	check_wire(&frame, resp, sizeof(resp));
	frame = zlrs_frame_t{.type = ZLRS_PKT_SESS_CFM,
			     .body = {.schedule = {.hop_index = 0x44,
						   .nonce = 0x55667788,
						   .frame_index = 0x99aabbcc,
						   .band_plan = {.region = 4}}},
			     .seq = 0x04030102};
	const uint8_t cfm[] = {7,    2,    1, 3, 4, 0x88, 0x77, 0x66, 0x55, 0x44, 0xcc, 0xbb,
			       0xaa, 0x99, 4, 0, 0, 0,    0,    0,    0xde, 0xad, 0xbe, 0xef};
	check_wire(&frame, cfm, sizeof(cfm));

	/* DATA borrows exactly the used bytes; P1 reserves four bytes that P0 can use. */
	uint8_t buf[ZLRS_FRAME_MAX_LEN];
	frame = zlrs_frame_t{.type = ZLRS_PKT_DATA_L2F, .seq = 1U};
	uint8_t maximum[ZLRS_DATA_MAX_LEN];
	memset(maximum, 0xa5, sizeof(maximum));
	frame.body.data.bytes = maximum;
	zlrs_frame_t out;
	zassert_equal(zlrs_test_encode(&frame, buf, sizeof(buf), 511U, ZLRS_TAG_LEN), 511U);
	zassert_true(zlrs_frame_decode(&out, buf, sizeof(buf), ZLRS_TAG_LEN));
	zexpect_equal(memcmp(out.body.data.bytes, frame.body.data.bytes, 499U), 0);
	zexpect_equal(out.body.data.len, 499U);
	memset(buf, 0, sizeof(buf));
	zexpect_true(out.body.data.bytes[0] == 0U);
	zassert_equal(zlrs_test_encode(&frame, buf, sizeof(buf), 511U, 0U), 511U);
	zassert_true(zlrs_frame_decode(&out, buf, sizeof(buf), 0U));
	zexpect_equal(memcmp(out.body.data.bytes, frame.body.data.bytes, 503U), 0);
	zexpect_equal(zlrs_test_encode(&frame, buf, sizeof(buf) - 1U, 511U, ZLRS_TAG_LEN), 0U);
	zexpect_equal(zlrs_test_encode(&frame, buf, sizeof(buf), UINT16_MAX, ZLRS_TAG_LEN), 0U);
	zexpect_equal(zlrs_test_encode(&frame, buf, sizeof(buf), 7U, 0U), 0U);
	zexpect_equal(zlrs_test_encode(&frame, buf, sizeof(buf), 8U, 1U), 0U);
	zassert_equal(zlrs_test_encode(&frame, buf, sizeof(buf), 8U, 0U), 8U);
	zassert_true(zlrs_frame_decode(&out, buf, 8U, 0U));
	zexpect_true(out.body.data.len == 0U);
	zexpect_false(zlrs_frame_decode(&out, buf, 7U, 0U));
	zexpect_false(zlrs_frame_decode(&out, buf, 4U, 0U));
	zexpect_false(zlrs_frame_decode(&out, buf, sizeof(buf) + 1U, 0U));
	zexpect_false(zlrs_frame_decode(&out, buf, 8U, 1U));
	frame.seq = 0U;
	zexpect_equal(zlrs_test_encode(&frame, buf, sizeof(buf), 8U, 0U), 0U);
	zexpect_equal(zlrs_profile_tag_len(ZLRS_PROFILE_P0), 0U);
	zexpect_equal(zlrs_profile_tag_len(ZLRS_PROFILE_P1), ZLRS_TAG_LEN);
}

ZTEST_SUITE(FrameCodec, NULL, NULL, NULL, NULL, NULL);

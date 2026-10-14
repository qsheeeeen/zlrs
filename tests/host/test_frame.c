/* SPDX-License-Identifier: Apache-2.0 */
#include "test.h"
#include <string.h>
#include "zlrs/proto/frame.h"

static void check_wire(const zlrs_frame_t *view, const uint8_t *expected, uint16_t size)
{
	uint8_t buf[ZLRS_FRAME_MAX_LEN], again[ZLRS_FRAME_MAX_LEN];
	zlrs_frame_t decoded;
	CHECK(zlrs_frame_encode(view, buf, sizeof(buf), size, ZLRS_TAG_LEN) == size);
	CHECK(memcmp(buf, expected, size) == 0);
	CHECK(zlrs_frame_decode(&decoded, buf, size, ZLRS_TAG_LEN));
	CHECK(decoded.type == view->type && decoded.seq == 0x04030102U);
	CHECK(memcmp(decoded.tag, expected + size - ZLRS_TAG_LEN, ZLRS_TAG_LEN) == 0);
	CHECK(zlrs_frame_encode(&decoded, again, sizeof(again), size, ZLRS_TAG_LEN) == size);
	CHECK(memcmp(buf, again, size) == 0);
	if (view->type != ZLRS_PKT_SYNC) {
		buf[0] |= 8U;
		CHECK(!zlrs_frame_decode(&decoded, buf, size, ZLRS_TAG_LEN));
		buf[0] = expected[0];
	} else {
		CHECK(decoded.body.sync.version == 11U && decoded.body.sync.profile == 1U);
	}
	if (view->type >= ZLRS_PKT_SYNC) {
		CHECK(!zlrs_frame_decode(&decoded, buf, size - 1U, ZLRS_TAG_LEN));
		CHECK(!zlrs_frame_decode(&decoded, buf, size + 1U, ZLRS_TAG_LEN));
		CHECK(zlrs_frame_encode(view, buf, sizeof(buf), size - 1U, ZLRS_TAG_LEN) == 0U);
		CHECK(zlrs_frame_encode(view, buf, sizeof(buf), size + 1U, ZLRS_TAG_LEN) == 0U);
	}
	zlrs_frame_t debug = *view;
	if (debug.type == ZLRS_PKT_SYNC) {
		debug.body.sync.profile = ZLRS_PROFILE_P0;
	}
	CHECK(zlrs_frame_encode(&debug, buf, sizeof(buf), size - ZLRS_TAG_LEN, 0U)
	      == size - ZLRS_TAG_LEN);
	CHECK(buf[0] == (view->type == ZLRS_PKT_SYNC ? 0xb2U : expected[0]));
	CHECK(memcmp(buf + 1U, expected + 1U, size - ZLRS_TAG_LEN - 1U) == 0);
	CHECK(zlrs_frame_decode(&decoded, buf, size - ZLRS_TAG_LEN, 0U));
	CHECK(decoded.seq == view->seq && decoded.tag[0] == 0U);
	memset(buf + 1U, 0, 4U);
	CHECK(!zlrs_frame_decode(&decoded, buf, size - ZLRS_TAG_LEN, 0U));
}

void test_frame(void)
{
	zlrs_frame_t frame = {.type = ZLRS_PKT_DATA_L2F,
			      .seq = 0x04030102,
			      .tag = {0xde, 0xad, 0xbe, 0xef},
			      .body.data = {.data_seq = 127,
					    .ack = 1,
					    .rssi_magnitude = 48,
					    .lq_percent = 99,
					    .bytes = {0x5a, 0xa5}}};
	const uint8_t data[] = {0,2,1,3,4, 0xff,48,99,0x5a,0xa5, 0xde,0xad,0xbe,0xef};
	check_wire(&frame, data, sizeof(data));
	frame = (zlrs_frame_t){.type = ZLRS_PKT_DATA_F2L,
			       .seq = 0x04030102,
			       .tag = {0xde, 0xad, 0xbe, 0xef},
			       .body.data = {.data_seq = 1, .lq_percent = 100}};
	const uint8_t feedback[] = {1,2,1,3,4, 2,0,100, 0xde,0xad,0xbe,0xef};
	check_wire(&frame, feedback, sizeof(feedback));
	frame = (zlrs_frame_t){.type = ZLRS_PKT_SYNC,
			       .seq = 0x04030102,
			       .tag = {0xde, 0xad, 0xbe, 0xef},
			       .body.sync = {.version = 11,
					     .profile = 1,
					     .hop_index = 254,
					     .nonce_leader = 0x11223344,
					     .frame_index = 0x55667788,
					     .band_plan = {.domain = 4,
							   .automatic = 1,
							   .target_subg = 1,
							   .switch_frame = 0x66778899}}};
	const uint8_t sync[] = {0xba,2,1,3,4, 254,0x44,0x33,0x22,0x11,
			       0x88,0x77,0x66,0x55, 0x1c,0x99,0x88,0x77,0x66, 0xde,0xad,0xbe,0xef};
	check_wire(&frame, sync, sizeof(sync));
	frame = (zlrs_frame_t){.type = ZLRS_PKT_PAIR_REQ, .seq = 0x04030102,
		.tag = {0xde, 0xad, 0xbe, 0xef},
		.body.pair_req = {.dev_a = {1,2,3,4}, .nonce_a = 0x11223344}};
	const uint8_t req[] = {3,2,1,3,4, 1,2,3,4,0x44,0x33,0x22,0x11, 0xde,0xad,0xbe,0xef};
	check_wire(&frame, req, sizeof(req));
	const zlrs_pair_frame_t pair_body = {.dev_a = {1,2,3,4}, .dev_b = {5,6,7,8},
		.nonce_a = 0x11223344, .nonce_b = 0x55667788};
	uint8_t pair[] = {4,2,1,3,4, 1,2,3,4,5,6,7,8,0x44,0x33,0x22,0x11,
			 0x88,0x77,0x66,0x55, 0xde,0xad,0xbe,0xef};
	for (uint8_t type = ZLRS_PKT_PAIR_RESP; type <= ZLRS_PKT_PAIR_CFM; type++) {
		frame = (zlrs_frame_t){.type = type, .seq = 0x04030102,
			.tag = {0xde, 0xad, 0xbe, 0xef}};
		pair[0] = type;
		frame.body.pair = pair_body;
		check_wire(&frame, pair, sizeof(pair));
	}
	frame = (zlrs_frame_t){
		.type = ZLRS_PKT_SESS_RESP,
		.seq = 0x04030102,
		.tag = {0xde, 0xad, 0xbe, 0xef},
		.body.sess_resp = {.nonce_leader = 0x11223344, .nonce_follower = 0x55667788}};
	const uint8_t resp[] = {6,2,1,3,4, 0x44,0x33,0x22,0x11,0x88,0x77,0x66,0x55,
			       0xde,0xad,0xbe,0xef};
	check_wire(&frame, resp, sizeof(resp));
	frame = (zlrs_frame_t){.type = ZLRS_PKT_SESS_CFM,
			       .seq = 0x04030102,
			       .tag = {0xde, 0xad, 0xbe, 0xef},
			       .body.sess_cfm = {.nonce_follower = 0x55667788,
						 .hop_index = 0x44,
						 .frame_index = 0x99aabbcc,
						 .band_plan = {.domain = 4}}};
	const uint8_t cfm[] = {7,2,1,3,4, 0x88,0x77,0x66,0x55,0x44,
			      0xcc,0xbb,0xaa,0x99, 4,0,0,0,0, 0xde,0xad,0xbe,0xef};
	check_wire(&frame, cfm, sizeof(cfm));

	/* DATA owns the used bytes; P1 reserves four bytes that P0 can use. */
	uint8_t buf[ZLRS_FRAME_MAX_LEN];
	frame = (zlrs_frame_t){
		.type = ZLRS_PKT_DATA_L2F, .seq = 1U, .tag = {0xde, 0xad, 0xbe, 0xef}};
	memset(frame.body.data.bytes, 0xa5, sizeof(frame.body.data.bytes));
	zlrs_frame_t out;
	CHECK(zlrs_frame_encode(&frame, buf, sizeof(buf), 511U, ZLRS_TAG_LEN) == 511U);
	CHECK(zlrs_frame_decode(&out, buf, sizeof(buf), ZLRS_TAG_LEN));
	CHECK(memcmp(out.body.data.bytes, frame.body.data.bytes, 499U) == 0);
	CHECK(out.body.data.bytes[499] == 0U);
	memset(buf, 0, sizeof(buf));
	CHECK(out.body.data.bytes[0] == 0xa5 && out.tag[0] == 0xde);
	CHECK(zlrs_frame_encode(&frame, buf, sizeof(buf), 511U, 0U) == 511U);
	CHECK(zlrs_frame_decode(&out, buf, sizeof(buf), 0U));
	CHECK(memcmp(out.body.data.bytes, frame.body.data.bytes, 503U) == 0);
	CHECK(zlrs_frame_encode(&frame, buf, sizeof(buf) - 1U, 511U, ZLRS_TAG_LEN) == 0U);
	CHECK(zlrs_frame_encode(&frame, buf, sizeof(buf), UINT16_MAX, ZLRS_TAG_LEN) == 0U);
	CHECK(zlrs_frame_encode(&frame, buf, sizeof(buf), 7U, 0U) == 0U);
	CHECK(zlrs_frame_encode(&frame, buf, sizeof(buf), 8U, 1U) == 0U);
	CHECK(zlrs_frame_encode(&frame, buf, sizeof(buf), 8U, 0U) == 8U);
	CHECK(zlrs_frame_decode(&out, buf, 8U, 0U));
	CHECK(out.body.data.bytes[0] == 0U && out.tag[0] == 0U);
	CHECK(!zlrs_frame_decode(&out, buf, 7U, 0U));
	CHECK(!zlrs_frame_decode(&out, buf, 4U, 0U));
	CHECK(!zlrs_frame_decode(&out, buf, sizeof(buf) + 1U, 0U));
	CHECK(!zlrs_frame_decode(&out, buf, 8U, 1U));
	frame.seq = 0U;
	CHECK(zlrs_frame_encode(&frame, buf, sizeof(buf), 8U, 0U) == 0U);
	CHECK(zlrs_profile_tag_len(ZLRS_PROFILE_P0) == 0U);
	CHECK(zlrs_profile_tag_len(ZLRS_PROFILE_P1) == ZLRS_TAG_LEN);
}

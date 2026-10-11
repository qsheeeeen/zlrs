/* SPDX-License-Identifier: Apache-2.0 */
#include "test.h"
#include <string.h>
#include "zlrs/mgmt/mgmt.h"

/* Feed a response frame back into the parser and pull out cmd and payload */
static bool parse_response(const uint8_t *frame, uint8_t len, zlrs_mgmt_parser_t *parser)
{
	int result = 0;

	zlrs_mgmt_parser_init(parser);
	for (uint8_t i = 0U; i < len; i++) {
		result = zlrs_mgmt_feed(parser, frame[i]);
	}

	return result == 1;
}

static void test_frame_codec(void)
{
	uint8_t frame[ZLRS_MGMT_FRAME_MAX_LEN];
	uint8_t payload[4] = {1U, 2U, 3U, 4U};
	zlrs_mgmt_parser_t parser;
	uint8_t len;

	printf("mgmt: frame codec\n");

	len = zlrs_mgmt_encode(frame, sizeof(frame), ZLRS_MGMT_CMD_SET_BAUD, payload,
			       sizeof(payload));
	CHECK(len == 10U);
	CHECK(frame[0] == ZLRS_MGMT_SOF0 && frame[1] == ZLRS_MGMT_SOF1);
	CHECK(frame[2] == ZLRS_MGMT_VERSION);
	CHECK(frame[3] == ZLRS_MGMT_CMD_SET_BAUD);
	CHECK(frame[4] == sizeof(payload));

	CHECK(parse_response(frame, len, &parser));
	CHECK(parser.cmd == ZLRS_MGMT_CMD_SET_BAUD);
	CHECK(parser.payload_len == sizeof(payload));
	CHECK(memcmp(ZLRS_MGMT_PAYLOAD(&parser), payload, sizeof(payload)) == 0);
	CHECK(parser.frames_ok == 1U);

	/* bad CRC */
	zlrs_mgmt_parser_init(&parser);
	for (uint8_t i = 0U; i < len; i++) {
		const uint8_t byte = (i + 1U == len) ? (uint8_t)(frame[i] ^ 0xFFU) : frame[i];
		const int r = zlrs_mgmt_feed(&parser, byte);

		if (i + 1U == len) {
			CHECK(r == -1);
		}
	}
	CHECK(parser.frames_ok == 0U);

	/* Wrong version, oversized payload and leading garbage all have to resynchronise */
	zlrs_mgmt_parser_init(&parser);
	CHECK(zlrs_mgmt_feed(&parser, 0x5AU) == -1);
	CHECK(zlrs_mgmt_feed(&parser, ZLRS_MGMT_SOF0) == 0);
	CHECK(zlrs_mgmt_feed(&parser, ZLRS_MGMT_SOF1) == 0);
	CHECK(zlrs_mgmt_feed(&parser, 0x7FU) == -1); /* unsupported version */
	CHECK(parse_response(frame, len, &parser));

	CHECK(zlrs_mgmt_encode(frame, sizeof(frame), ZLRS_MGMT_CMD_STATUS, payload,
			       ZLRS_MGMT_PAYLOAD_MAX_LEN + 1U) == 0U);
	CHECK(zlrs_mgmt_encode(frame, 4U, ZLRS_MGMT_CMD_STATUS, NULL, 0U) == 0U);
}

void test_mgmt(void)
{
	test_frame_codec();
}

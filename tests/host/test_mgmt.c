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

	const zlrs_mgmt_status_t sent = {
		.version = ZLRS_MGMT_VERSION,
		.rssi_dbm = -48,
		.peer_rssi_dbm = -51,
		.peer_lq_percent = 97,
		.active_power_dbm = -10,
		.power_floor_dbm = -10,
		.temperature_centi_c = -125,
		.tx_frames = 1024,
		.rx_frames = 0xFEDCBA98U,
		.rx_crc_errors = 24,
		.rx_bad_tag = 1,
		.tx_retries = 3,
		.missed_slots = 2,
		.radio_fail = 4,
		.frequency_hz = 2478000000U,
		.hop_index = 236,
		.frame_period_us = 9080,
		.radio_temperature_centi_c = -125,
		.radio_xosc_temperature_centi_c = 3125,
		.radio_vbat_mv = 3301,
		.radio_version_major = 1,
		.radio_version_minor = 0x18,
		.radio_chip_mode = 2,
		.radio_command_status = 2,
		.radio_reset_status = 1,
		.radio_valid = 0x3F,
		.radio_errors = 0x8401,
		.radio_irq = 0xFEDCBA98U,
	};
	uint8_t bytes[ZLRS_MGMT_STATUS_LEN];
	zlrs_mgmt_status_t received;
	zlrs_mgmt_status_encode(bytes, &sent);
	const uint8_t signed_fields[] = {0xD0, 0xFF, 0xCD, 0xFF, 97, 0xF6, 0xF6, 0x83, 0xFF};
	CHECK(memcmp(bytes + 21, signed_fields, sizeof(signed_fields)) == 0);
	CHECK(zlrs_mgmt_status_decode(bytes, sizeof(bytes), &received));
	CHECK(received.rssi_dbm == -48 && received.peer_rssi_dbm == -51);
	CHECK(received.temperature_centi_c == -125);
	CHECK(received.rx_frames == 0xFEDCBA98U && received.tx_frames == 1024);
	CHECK(received.rx_crc_errors == 24 && received.tx_retries == 3);
	CHECK(received.frequency_hz == 2478000000U && received.hop_index == 236);
	CHECK(received.frame_period_us == 9080);
	const uint8_t radio_fields[] = {0x83, 0xFF, 0x35, 0x0C, 0xE5, 0x0C, 1, 0x18,
					2, 2, 1, 0x3F, 1, 0x84, 0x98, 0xBA, 0xDC, 0xFE};
	CHECK(memcmp(bytes + 72, radio_fields, sizeof(radio_fields)) == 0);
	CHECK(received.radio_temperature_centi_c == -125);
	CHECK(received.radio_xosc_temperature_centi_c == 3125);
	CHECK(received.radio_vbat_mv == 3301);
	CHECK(received.radio_version_major == 1 && received.radio_version_minor == 0x18);
	CHECK(received.radio_chip_mode == 2 && received.radio_command_status == 2);
	CHECK(received.radio_reset_status == 1 && received.radio_valid == 0x3F);
	CHECK(received.radio_errors == 0x8401 && received.radio_irq == 0xFEDCBA98U);
	/* The enlarged status exactly fills the maximum management payload. */
	uint8_t frame[ZLRS_MGMT_FRAME_MAX_LEN];
	zlrs_mgmt_parser_t parser;
	const uint8_t len = zlrs_mgmt_encode(frame, sizeof(frame), ZLRS_MGMT_CMD_STATUS_RESP,
					     bytes, sizeof(bytes));
	CHECK(len == ZLRS_MGMT_FRAME_MAX_LEN && parse_response(frame, len, &parser));
	CHECK(parser.payload_len == ZLRS_MGMT_STATUS_LEN);
	CHECK(memcmp(ZLRS_MGMT_PAYLOAD(&parser), bytes, sizeof(bytes)) == 0);
	CHECK(!zlrs_mgmt_status_decode(bytes, 72, &received));
	CHECK(!zlrs_mgmt_status_decode(bytes, sizeof(bytes) - 1, &received));
	CHECK(!zlrs_mgmt_status_decode(bytes, sizeof(bytes) + 1, &received));
}

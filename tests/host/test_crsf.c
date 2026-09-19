/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * CRSF codec tests. Where the expected values come from:
 *   - CRC8 from the standard check value ("123456789" -> 0xBC)
 *   - channel packing from hand-computed edges (all zero, all ones, a single channel)
 *   - the parser gets built frames fed byte by byte, including truncation, garbage and bad CRC
 */

#include "test.h"

#include <string.h>

#include "zlrs/crsf/crsf.h"

static void test_crc8(void)
{
	static const uint8_t check[] = "123456789";

	printf("crsf crc8\n");

	CHECK(zlrs_crsf_crc8(check, 9U) == 0xBCU);
	CHECK(zlrs_crsf_crc8((const uint8_t *)"", 0U) == 0x00U);
	/* Single bytes 0x00 and 0x01 match the hand computation */
	CHECK(zlrs_crsf_crc8((const uint8_t[]){ 0x00U }, 1U) == 0x00U);
	CHECK(zlrs_crsf_crc8((const uint8_t[]){ 0x01U }, 1U) == 0xD5U);
}

static void test_channels(void)
{
	uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT];
	uint16_t back[ZLRS_CRSF_CHANNEL_COUNT];
	uint8_t packed[ZLRS_CRSF_RC_PAYLOAD_LEN];

	printf("crsf channels\n");

	/* All zero -> all zero bytes */
	memset(channels, 0, sizeof(channels));
	zlrs_crsf_channels_pack(packed, channels);
	CHECK(zlrs_test_hex_eq(packed, sizeof(packed),
			       "00000000000000000000000000000000000000000000"));

	/* All ones (every 11-bit field set) -> 22 bytes of 0xFF */
	for (uint8_t i = 0U; i < ZLRS_CRSF_CHANNEL_COUNT; i++) {
		channels[i] = ZLRS_CRSF_CHANNEL_MASK;
	}
	zlrs_crsf_channels_pack(packed, channels);
	CHECK(zlrs_test_hex_eq(packed, sizeof(packed),
			       "ffffffffffffffffffffffffffffffffffffffffffff"));

	/* Only channel 0 = 0x0AC (172): byte 0 is 0xAC and the low 3 bits of byte 1 hold channel 1's
	 * top 3 bits (zero), everything else zero: the hand-computed result */
	memset(channels, 0, sizeof(channels));
	channels[0] = ZLRS_CRSF_CHANNEL_MIN;
	zlrs_crsf_channels_pack(packed, channels);
	CHECK(packed[0] == 0xACU);
	CHECK(packed[1] == 0x00U);
	for (size_t i = 2U; i < sizeof(packed); i++) {
		CHECK(packed[i] == 0x00U);
	}

	/* Channel edge values survive a round trip; anything above 11 bits is masked off */
	for (uint8_t i = 0U; i < ZLRS_CRSF_CHANNEL_COUNT; i++) {
		channels[i] = (uint16_t)(ZLRS_CRSF_CHANNEL_MIN + i * 97U);
	}
	channels[3] = 0xFFFFU; /* must be truncated to 0x7FF */
	zlrs_crsf_channels_pack(packed, channels);
	zlrs_crsf_channels_unpack(packed, back);
	for (uint8_t i = 0U; i < ZLRS_CRSF_CHANNEL_COUNT; i++) {
		const uint16_t want =
			(uint16_t)(channels[i] & ZLRS_CRSF_CHANNEL_MASK);

		CHECK(back[i] == want);
	}

	/* Default failsafe: throttle low, the rest centred */
	zlrs_crsf_channels_default(back);
	CHECK(back[0] == ZLRS_CRSF_CHANNEL_MIN);
	for (uint8_t i = 1U; i < ZLRS_CRSF_CHANNEL_COUNT; i++) {
		CHECK(back[i] == ZLRS_CRSF_CHANNEL_MID);
	}
}

static void test_encode_parse(void)
{
	uint8_t frame[ZLRS_CRSF_FRAME_MAX_LEN];
	uint8_t payload[ZLRS_CRSF_RC_PAYLOAD_LEN];
	uint16_t channels[ZLRS_CRSF_CHANNEL_COUNT];
	zlrs_crsf_parser_t parser;
	uint8_t len;

	printf("crsf encode/parse\n");

	zlrs_crsf_channels_default(channels);
	zlrs_crsf_channels_pack(payload, channels);

	len = zlrs_crsf_encode(frame, ZLRS_CRSF_ADDR_FC, ZLRS_CRSF_TYPE_RC_CHANNELS, payload,
			       sizeof(payload));
	CHECK(len == 26U); /* 4 bytes of overhead plus a 22-byte payload */
	CHECK(frame[0] == ZLRS_CRSF_ADDR_FC);
	CHECK(frame[1] == 24U); /* len = payload + type + crc */
	CHECK(frame[2] == ZLRS_CRSF_TYPE_RC_CHANNELS);

	/* Whole-frame CRC matches the hand computation (it covers type + payload) */
	CHECK(frame[len - 1U] == zlrs_crsf_crc8(&frame[2], len - 3U));

	/* Fed byte by byte: the frame only appears on the last one */
	zlrs_crsf_parser_init(&parser);
	for (uint8_t i = 0U; i + 1U < len; i++) {
		CHECK(zlrs_crsf_feed(&parser, frame[i]) == ZLRS_CRSF_NONE);
	}
	CHECK(zlrs_crsf_feed(&parser, frame[len - 1U]) == ZLRS_CRSF_FRAME);
	CHECK(parser.type == ZLRS_CRSF_TYPE_RC_CHANNELS);
	CHECK(parser.payload_len == sizeof(payload));
	CHECK(memcmp(ZLRS_CRSF_PAYLOAD(&parser), payload, sizeof(payload)) == 0);
	CHECK(parser.frames_ok == 1U);
	CHECK(parser.frames_bad == 0U);

	/* An illegal payload length (> 60) is refused when building the frame */
	CHECK(zlrs_crsf_encode(frame, ZLRS_CRSF_ADDR_FC, ZLRS_CRSF_TYPE_RC_CHANNELS, payload,
			       61U) == 0U);
}

static void test_parser_recovery(void)
{
	uint8_t frame[ZLRS_CRSF_FRAME_MAX_LEN];
	uint8_t stats_payload[ZLRS_CRSF_LINK_STATS_LEN] = { 60U, 60U, 99U, 5U, 0U, 1U, 3U, 61U, 98U, 4U };
	zlrs_crsf_parser_t parser;
	uint8_t len;

	printf("crsf parser recovery\n");

	len = zlrs_crsf_encode(frame, ZLRS_CRSF_ADDR_TX_MODULE,
			       ZLRS_CRSF_TYPE_LINK_STATISTICS, stats_payload,
			       sizeof(stats_payload));
	CHECK(len == 14U);

	/* Resynchronises after leading garbage, including a fake address */
	zlrs_crsf_parser_init(&parser);
	CHECK(zlrs_crsf_feed(&parser, 0x5AU) == ZLRS_CRSF_BAD_FRAME);
	CHECK(zlrs_crsf_feed(&parser, 0xC8U) == ZLRS_CRSF_NONE); /* fake header */
	CHECK(zlrs_crsf_feed(&parser, 0xFFU) == ZLRS_CRSF_BAD_FRAME); /* illegal length */
	for (uint8_t i = 0U; i < len; i++) {
		const enum zlrs_crsf_result r = zlrs_crsf_feed(&parser, frame[i]);

		if (i + 1U < len) {
			CHECK(r == ZLRS_CRSF_NONE);
		} else {
			CHECK(r == ZLRS_CRSF_FRAME);
		}
	}
	CHECK(parser.type == ZLRS_CRSF_TYPE_LINK_STATISTICS);
	CHECK(parser.frames_ok == 1U);

	/* Bad CRC: flip the last byte */
	zlrs_crsf_parser_init(&parser);
	for (uint8_t i = 0U; i < len; i++) {
		const uint8_t byte = (i + 1U == len) ? (uint8_t)(frame[i] ^ 0x01U) : frame[i];

		if (i + 1U < len) {
			(void)zlrs_crsf_feed(&parser, byte);
		} else {
			CHECK(zlrs_crsf_feed(&parser, byte) == ZLRS_CRSF_BAD_CRC);
		}
	}
	CHECK(parser.frames_ok == 0U);
	CHECK(parser.frames_bad == 1U);

	/* Recovers straight after a truncated frame: feed a new one */
	enum zlrs_crsf_result seen = ZLRS_CRSF_NONE;

	zlrs_crsf_parser_init(&parser);
	for (uint8_t i = 0U; i < 5U; i++) {
		(void)zlrs_crsf_feed(&parser, frame[i]);
	}
	for (uint8_t i = 0U; i < len; i++) {
		(void)zlrs_crsf_feed(&parser, frame[i]);
	}
	/* Truncated leftovers eat part of the next frame (CRC must fail), then resync on the third */
	for (uint8_t i = 0U; i < len; i++) {
		const enum zlrs_crsf_result r = zlrs_crsf_feed(&parser, frame[i]);

		if (r == ZLRS_CRSF_FRAME) {
			seen = r;
		}
	}
	CHECK(seen == ZLRS_CRSF_FRAME);
	CHECK(parser.frames_ok >= 1U);
}

static void test_link_statistics(void)
{
	zlrs_crsf_link_stats_t in;
	zlrs_crsf_link_stats_t out;
	uint8_t buf[ZLRS_CRSF_LINK_STATS_LEN];

	printf("crsf link statistics\n");

	memset(&in, 0, sizeof(in));
	in.uplink_rssi_ant1 = zlrs_crsf_rssi_from_dbm(-72);
	in.uplink_rssi_ant2 = in.uplink_rssi_ant1;
	in.uplink_lq = 97U;
	in.uplink_snr_db = -3;
	in.active_antenna = 0U;
	in.rf_mode = zlrs_crsf_rf_mode(1300000U);
	in.uplink_tx_power = zlrs_crsf_tx_power_index(20);
	in.downlink_rssi = zlrs_crsf_rssi_from_dbm(-65);
	in.downlink_lq = 100U;
	in.downlink_snr_db = 6;

	CHECK(in.uplink_rssi_ant1 == 72U);
	CHECK(in.rf_mode == 1U);
	CHECK(in.uplink_tx_power == 3U); /* 20 dBm ≈ 100 mW */

	zlrs_crsf_link_stats_encode(buf, &in);
	CHECK(zlrs_test_hex_eq(buf, sizeof(buf), "484861fd000103416406"));

	zlrs_crsf_link_stats_decode(buf, &out);
	CHECK(memcmp(&in, &out, sizeof(in)) == 0);

	/* power tier edges */
	CHECK(zlrs_crsf_tx_power_index(0) == 0U);
	CHECK(zlrs_crsf_tx_power_index(10) == 1U);
	CHECK(zlrs_crsf_tx_power_index(-5) == 0U);
	CHECK(zlrs_crsf_tx_power_index(30) == 5U); /* 1000 mW */
	CHECK(zlrs_crsf_tx_power_index(33) == 5U); /* 33 dBm ~ 2 W, still reports the floor tier */
	CHECK(zlrs_crsf_tx_power_index(34) == 6U); /* 34 dBm ~ 2.5 W -> the 2000 mW tier */

	/* Unknown rate tiers get no id */
	CHECK(zlrs_crsf_rf_mode(12345U) == 0xFFU);

	/* Modulation aware: LoRa tiers encode the SF (0x10 | SF), FLRC matches the old entry point */
	{
		const zlrs_mod_cfg_t lora = zlrs_mod_lora(7U, 125000U, 1U, 8U);
		const zlrs_mod_cfg_t flrc = zlrs_mod_flrc(1300000U);

		CHECK(zlrs_crsf_rf_mode_mod(&lora) == 0x17U);
		CHECK(zlrs_crsf_rf_mode_mod(&flrc) == 1U);
		CHECK(zlrs_crsf_rf_mode_mod(NULL) == 0xFFU);
	}
}

void test_crsf(void)
{
	test_crc8();
	test_channels();
	test_encode_parse();
	test_parser_recovery();
	test_link_statistics();
}

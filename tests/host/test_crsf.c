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
#include "zlrs/crsf/device.h"

static void test_crc8(void)
{
	static const uint8_t check[] = "123456789";

	printf("crsf crc8\n");

	CHECK(zlrs_crsf_crc8(check, 9U) == 0xBCU);
	CHECK(zlrs_crsf_crc8((const uint8_t *)"", 0U) == 0x00U);
	/* Single bytes 0x00 and 0x01 match the hand computation */
	CHECK(zlrs_crsf_crc8((const uint8_t[]){0x00U}, 1U) == 0x00U);
	CHECK(zlrs_crsf_crc8((const uint8_t[]){0x01U}, 1U) == 0xD5U);
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
		const uint16_t want = (uint16_t)(channels[i] & ZLRS_CRSF_CHANNEL_MASK);

		CHECK(back[i] == want);
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

	for (uint8_t i = 0U; i < ZLRS_CRSF_CHANNEL_COUNT; i++) {
		channels[i] = ZLRS_CRSF_CHANNEL_MID;
	}
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
	uint8_t stats_payload[ZLRS_CRSF_LINK_STATS_LEN] = {60U, 60U, 99U, 5U,  0U,
							   1U,  3U,  61U, 98U, 4U};
	zlrs_crsf_parser_t parser;
	uint8_t len;

	printf("crsf parser recovery\n");

	len = zlrs_crsf_encode(frame, ZLRS_CRSF_ADDR_TX_MODULE, ZLRS_CRSF_TYPE_LINK_STATISTICS,
			       stats_payload, sizeof(stats_payload));
	CHECK(len == 14U);

	/* Resynchronises after leading garbage, including a fake address */
	zlrs_crsf_parser_init(&parser);
	CHECK(zlrs_crsf_feed(&parser, 0x01U) == ZLRS_CRSF_BAD_FRAME);
	CHECK(zlrs_crsf_feed(&parser, 0xC8U) == ZLRS_CRSF_NONE);      /* fake header */
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
	in.rf_mode = zlrs_crsf_rf_mode(50U);
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

	/* The basic report uses the closest nominal profile; 0x1D carries actual fps. */
	CHECK(zlrs_crsf_rf_mode(12345U) == 2U);

	CHECK(zlrs_crsf_rf_mode(4U) == 0U);
	CHECK(zlrs_crsf_rf_mode(150U) == 2U);
}

typedef struct {
	uint8_t type;
	uint8_t data[60];
	uint8_t len;
	uint32_t sends;
	uint32_t writes;
	uint32_t commands;
	bool blocked;
	bool reject;
} device_capture_t;

static bool device_send(void *ctx, uint8_t type, const uint8_t *data, uint8_t len)
{
	device_capture_t *c = ctx;
	if (c->blocked) {
		return false;
	}
	c->type = type;
	c->len = len;
	memcpy(c->data, data, len);
	c->sends++;
	return true;
}

static bool device_write(void *ctx, uint8_t id, int32_t value)
{
	device_capture_t *c = ctx;
	(void)id;
	(void)value;
	c->writes++;
	return !c->reject;
}

static bool device_command(void *ctx, uint8_t group, uint8_t command, const uint8_t *data,
			   uint8_t len)
{
	device_capture_t *c = ctx;
	(void)group;
	(void)command;
	(void)data;
	(void)len;
	c->commands++;
	return !c->reject;
}

static bool device_speed(void *ctx, uint8_t port, uint32_t baud)
{
	(void)ctx;
	return port == 0U && baud == 1000000U;
}

static void test_device_protocol(void)
{
	device_capture_t c = {0};
	const zlrs_crsf_device_ops_t ops = {
		.send = device_send,
		.write = device_write,
		.command = device_command,
		.speed = device_speed,
		.ctx = &c,
	};
	zlrs_crsf_device_t d;
	uint8_t p[60] = {0xEE, 0xEA};

	printf("crsf device discovery, chunks, write confirmation and commands\n");
	zlrs_crsf_device_init(&d, 0xEE, &ops);
	d.serial = 0x12345678U;
	CHECK(zlrs_crsf_device_handle(&d, 0x28, p, 2) == ZLRS_CRSF_HANDLED);
	CHECK(c.type == 0x29 && c.data[0] == 0xEA && c.data[1] == 0xEE);
	CHECK(memcmp(c.data + 2, "ZLRS TX", 8) == 0);
	CHECK(zlrs_test_hex_eq(c.data + 10, 4, "12345678"));
	CHECK(c.data[c.len - 2] == ZLRS_CRSF_PARAM_COUNT - 1);
	p[0] = 0xEC;
	CHECK(zlrs_crsf_device_handle(&d, 0x28, p, 2) == ZLRS_CRSF_ROUTE);
	p[0] = 0;
	CHECK(zlrs_crsf_device_handle(&d, 0x28, p, 2) == ZLRS_CRSF_HANDLED);
	c.blocked = true;
	CHECK(zlrs_crsf_device_handle(&d, 0x28, p, 2) == ZLRS_CRSF_RETRY);
	c.blocked = false;
	p[0] = 0xEE;
	p[2] = 0;
	p[3] = 0;
	CHECK(zlrs_crsf_device_handle(&d, 0x2C, p, 4) == ZLRS_CRSF_HANDLED);
	CHECK(c.type == 0x2B && c.data[5] == 11);
	CHECK(c.data[c.len - 1] == 0xFF);
	/* A long firmware info exercises independently requested and repeatable chunks. */
	d.version =
		"01234567890123456789012345678901234567890123456789012345678901234567890123456789";
	p[2] = ZLRS_CRSF_PARAM_VERSION;
	CHECK(zlrs_crsf_device_handle(&d, 0x2C, p, 4) == ZLRS_CRSF_HANDLED);
	CHECK(c.len == 60 && c.data[3] == 1);
	uint8_t first[60];
	memcpy(first, c.data, sizeof(first));
	CHECK(zlrs_crsf_device_handle(&d, 0x2C, p, 4) == ZLRS_CRSF_HANDLED);
	CHECK(memcmp(c.data, first, sizeof(first)) == 0);
	p[3] = 1;
	CHECK(zlrs_crsf_device_handle(&d, 0x2C, p, 4) == ZLRS_CRSF_HANDLED);
	CHECK(c.data[3] == 0 && c.len < 60 && c.data[c.len - 1] == 0);
	p[2] = 0xFE;
	p[3] = 0;
	CHECK(zlrs_crsf_device_handle(&d, 0x2C, p, 4) == ZLRS_CRSF_HANDLED);
	CHECK(c.data[5] == 0x7F);
	/* Signed big-endian FLOAT value, durable-write rejection and range rejection. */
	p[2] = ZLRS_CRSF_PARAM_POWER;
	p[3] = 0xFF;
	p[4] = 0xFF;
	p[5] = 0xFF;
	p[6] = 0xF6;
	CHECK(zlrs_crsf_device_handle(&d, 0x2D, p, 7) == ZLRS_CRSF_HANDLED);
	CHECK(d.values[ZLRS_CRSF_PARAM_POWER] == -10 && c.type == 0x2D);
	CHECK(c.data[0] == 0xEA && c.data[1] == 0xEE && memcmp(c.data + 2, p + 2, 5) == 0);
	c.reject = true;
	p[6] = 0xF7;
	CHECK(zlrs_crsf_device_handle(&d, 0x2D, p, 7) == ZLRS_CRSF_HANDLED);
	CHECK(d.values[ZLRS_CRSF_PARAM_POWER] == -10 && c.type == 0x2B);
	c.reject = false;
	const uint32_t writes = c.writes;
	p[3] = p[4] = p[5] = 0;
	p[6] = 34;
	CHECK(zlrs_crsf_device_handle(&d, 0x2D, p, 7) == ZLRS_CRSF_HANDLED);
	CHECK(c.writes == writes);
	/* Device response must not be interpreted as another write or command. */
	p[0] = 0xEA;
	CHECK(zlrs_crsf_device_handle(&d, 0x2D, p, 7) == ZLRS_CRSF_ROUTE);
	p[0] = 0xEE;
	p[2] = 0x10;
	p[3] = 0x05;
	p[4] = 9;
	p[5] = zlrs_crsf_command_crc(p, 5);
	CHECK(zlrs_crsf_device_handle(&d, 0x32, p, 6) == ZLRS_CRSF_HANDLED);
	CHECK(d.values[ZLRS_CRSF_PARAM_MODEL] == 9 && c.commands == 1);
	CHECK(zlrs_crsf_command_valid(c.data, c.len));
	CHECK(c.data[2] == 0xFF && c.data[3] == 0x10 && c.data[4] == 0x05 && c.data[5] == 1);
	p[5] ^= 1;
	CHECK(zlrs_crsf_device_handle(&d, 0x32, p, 6) == ZLRS_CRSF_INVALID);
	CHECK(c.commands == 1);
	p[3] = 0x06;
	p[4] = zlrs_crsf_command_crc(p, 4);
	CHECK(zlrs_crsf_device_handle(&d, 0x32, p, 5) == ZLRS_CRSF_HANDLED);
	CHECK(c.data[3] == 7 && c.data[4] == 9 && zlrs_crsf_command_valid(c.data, c.len));
	p[2] = 0x0A;
	p[3] = 0x70;
	p[4] = 0;
	p[5] = 0;
	p[6] = 0x0F;
	p[7] = 0x42;
	p[8] = 0x40;
	p[9] = zlrs_crsf_command_crc(p, 9);
	CHECK(zlrs_crsf_device_handle(&d, 0x32, p, 10) == ZLRS_CRSF_HANDLED);
	CHECK(c.data[3] == 0x71 && c.data[4] == 0 && c.data[5] == 1);
	p[8] = 0;
	p[9] = zlrs_crsf_command_crc(p, 9);
	CHECK(zlrs_crsf_device_handle(&d, 0x32, p, 10) == ZLRS_CRSF_HANDLED);
	CHECK(c.data[5] == 0);
	CHECK(zlrs_crsf_device_timing(&d, 2000, -123));
	CHECK(c.type == 0x3A && c.len == 11 && c.data[2] == 0x10);
	CHECK(zlrs_test_hex_eq(c.data + 3, 8, "00004e20fffffb32"));
	CHECK(!zlrs_crsf_device_timing(&d, 0, 0));
	/* Malformed and short extended frames never access missing addresses/fields. */
	for (uint8_t len = 0; len < 4; len++) {
		CHECK(zlrs_crsf_device_handle(&d, 0x2C, p, len) != ZLRS_CRSF_RETRY);
	}
}

static void test_serial_extensions(void)
{
	zlrs_crsf_parser_t p;
	uint8_t frame[64];
	uint8_t channels[23] = {0};

	printf("crsf extended RC, device sync addresses and UART gap recovery\n");
	channels[22] = 2;
	const uint8_t len = zlrs_crsf_encode(frame, 0xEE, 0x16, channels, 23);
	CHECK(len == 27 && zlrs_crsf_frame_valid(frame, len));
	zlrs_crsf_parser_init(&p);
	for (uint8_t i = 0; i < len; i++) {
		const enum zlrs_crsf_result result = zlrs_crsf_feed_at(&p, frame[i], 100);
		CHECK(result == (i + 1 == len ? ZLRS_CRSF_FRAME : ZLRS_CRSF_NONE));
	}
	CHECK(p.payload_len == 23 && ZLRS_CRSF_PAYLOAD(&p)[22] == 2);
	zlrs_crsf_parser_init(&p);
	for (uint8_t i = 0; i < 5; i++) {
		(void)zlrs_crsf_feed_at(&p, frame[i], 0xFFFFFFFEU);
	}
	for (uint8_t i = 0; i < len; i++) {
		const enum zlrs_crsf_result result = zlrs_crsf_feed_at(&p, frame[i], 4);
		CHECK(result == (i + 1 == len ? ZLRS_CRSF_FRAME : ZLRS_CRSF_NONE));
	}
	CHECK(p.frames_bad == 1);
	frame[0] = 0xCE;
	CHECK(zlrs_crsf_frame_valid(frame, len));
	frame[0] = 0x5A;
	CHECK(zlrs_crsf_frame_valid(frame, len));
	frame[0] = 0xEF;
	CHECK(!zlrs_crsf_frame_valid(frame, len));
	CHECK(zlrs_crsf_encode(NULL, 0xC8, 0x16, channels, 23) == 0);
	CHECK(zlrs_crsf_encode(frame, 0xC8, 0x16, NULL, 23) == 0);
	CHECK(zlrs_crsf_rssi_percent(-100, -100) == 0);
	CHECK(zlrs_crsf_rssi_percent(-75, -100) == 50);
	CHECK(zlrs_crsf_rssi_percent(-45, -100) == 100);
}

static void test_device_malformed(void)
{
	device_capture_t capture = {0};
	const zlrs_crsf_device_ops_t ops = {.send = device_send,
					    .write = device_write,
					    .command = device_command,
					    .ctx = &capture};
	zlrs_crsf_device_t device;
	uint8_t payload[ZLRS_CRSF_PAYLOAD_MAX_LEN];
	uint32_t random = 0x19780905U;
	const uint8_t types[] = {0x28, 0x2B, 0x2C, 0x2D, 0x32, 0x3A, 0x7A, 0x88};

	printf("crsf bounded malformed device payloads and command CRCs\n");
	zlrs_crsf_device_init(&device, ZLRS_CRSF_ADDR_TX_MODULE, &ops);
	for (unsigned int sample = 0U; sample < 4096U; sample++) {
		for (size_t i = 0U; i < sizeof(payload); i++) {
			random ^= random << 13;
			random ^= random >> 17;
			random ^= random << 5;
			payload[i] = (uint8_t)random;
		}
		payload[0] = sample & 1U ? 0U : device.address;
		payload[1] = ZLRS_CRSF_ADDR_HANDSET;
		const uint8_t len = (uint8_t)(sample % (sizeof(payload) + 1U));
		const uint8_t type = types[sample % sizeof(types)];

		if (type == ZLRS_CRSF_TYPE_COMMAND && len >= 5U && (sample & 2U) != 0U) {
			payload[len - 1U] = zlrs_crsf_command_crc(payload, len - 1U);
		}
		(void)zlrs_crsf_device_handle(&device, type, payload, len);
		CHECK(capture.len <= ZLRS_CRSF_PAYLOAD_MAX_LEN);
	}
}

void test_crsf(void)
{
	test_crc8();
	test_channels();
	test_encode_parse();
	test_parser_recovery();
	test_link_statistics();
	test_device_protocol();
	test_serial_extensions();
	test_device_malformed();
}

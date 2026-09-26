/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Management protocol tests: framing with built frames and byte-at-a-time parsing, and command
 * dispatch run against a fake backend, error paths included. Fixed PBKDF2 vectors from Python.
 */

#include "test.h"

#include <string.h>

#include "zlrs/mgmt/mgmt.h"
#include "zlrs/proto/crypto.h"

typedef struct {
	uint8_t saved_key[ZLRS_KEY_LEN];
	bool key_saved;
	uint8_t failsafe[22];
	bool failsafe_saved;
	uint32_t baud;
	bool baud_saved;
	uint32_t pair_count;
	uint8_t role;
	bool role_saved;
	uint8_t profile;
	bool profile_saved;
	uint32_t new_dev_id_count;
	int store_rc; /* error code save_root_key should return */
	bool image_confirmed;
	uint32_t log_read_offset; /* what the last log read was asked for */
	uint8_t log_read_len;
	uint8_t log_read_count;
} fake_dev_t;

static fake_dev_t fake;

static int fake_save_key(void *ctx, const uint8_t key[ZLRS_KEY_LEN])
{
	fake_dev_t *dev = ctx;

	if (dev->store_rc != ZLRS_MGMT_OK) {
		return dev->store_rc;
	}
	memcpy(dev->saved_key, key, ZLRS_KEY_LEN);
	dev->key_saved = true;

	return ZLRS_MGMT_OK;
}

static int fake_read_status(void *ctx, zlrs_mgmt_status_t *out)
{
	(void)ctx;

	out->version = ZLRS_MGMT_VERSION;
	out->role = 1U;
	out->state = 2U;
	out->flags = ZLRS_MGMT_FLAG_KEY | ZLRS_MGMT_FLAG_PAIRED;
	out->dev_id[0] = 0xAAU;
	out->dev_id[3] = 0xBBU;
	out->peer_id[0] = 0x11U;
	out->lq_percent = 97U;
	out->profile = 1U;

	return ZLRS_MGMT_OK;
}

static int fake_confirm_image(void *ctx)
{
	fake_dev_t *dev = ctx;

	dev->image_confirmed = true;

	return ZLRS_MGMT_OK;
}

static int fake_read_build(void *ctx, char *out, size_t out_len)
{
	(void)ctx;

	if (out == NULL || out_len < 8U) {
		return ZLRS_MGMT_E_PARAM;
	}
	memcpy(out, "0.1.0-t", 8U);

	return ZLRS_MGMT_OK;
}

static int fake_read_image_hash(void *ctx, uint8_t out[ZLRS_MGMT_IMAGE_HASH_LEN])
{
	(void)ctx;

	for (uint8_t i = 0U; i < ZLRS_MGMT_IMAGE_HASH_LEN; i++) {
		out[i] = i;
	}

	return ZLRS_MGMT_OK;
}

/*
 * The simple settings all arrive through one entry point, exactly as the application's table
 * sees them: the command says what the payload means, this fake decodes and records it.
 */
static int fake_save_setting(void *ctx, uint8_t cmd, const uint8_t *value, uint8_t len)
{
	fake_dev_t *dev = ctx;

	switch (cmd) {
	case ZLRS_MGMT_CMD_SET_FAILSAFE:
		if (len != 22U) {
			return ZLRS_MGMT_E_PARAM;
		}
		memcpy(dev->failsafe, value, sizeof(dev->failsafe));
		dev->failsafe_saved = true;
		return ZLRS_MGMT_OK;
	case ZLRS_MGMT_CMD_SET_BAUD:
		if (len != 4U) {
			return ZLRS_MGMT_E_PARAM;
		}
		dev->baud = (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
			    ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
		dev->baud_saved = true;
		return ZLRS_MGMT_OK;
	case ZLRS_MGMT_CMD_SET_ROLE:
		dev->role = value[0];
		dev->role_saved = true;
		return ZLRS_MGMT_OK;
	case ZLRS_MGMT_CMD_SET_PROFILE:
		/* Pretend this build is production-shaped: P0 is refused, like the application does */
		if (value[0] == 0U) {
			return ZLRS_MGMT_E_PARAM;
		}
		dev->profile = value[0];
		dev->profile_saved = true;
		return ZLRS_MGMT_OK;
	default:
		return ZLRS_MGMT_E_STATE;
	}
}

static int fake_pair(void *ctx)
{
	fake_dev_t *dev = ctx;

	dev->pair_count++;

	return ZLRS_MGMT_OK;
}

static int fake_new_dev_id(void *ctx)
{
	fake_dev_t *dev = ctx;

	dev->new_dev_id_count++;

	return ZLRS_MGMT_OK;
}

/* Two "files": the export walks them oldest first and asks for chunks */
static int fake_log_info(void *ctx, zlrs_mgmt_log_info_t *out)
{
	(void)ctx;

	out->count = 2U;
	out->newest = 1U;
	out->num[0] = 6U;
	out->num[1] = 7U;
	out->size[0] = 0x11223344U;
	out->size[1] = 5U;

	return ZLRS_MGMT_OK;
}

static int fake_log_read(void *ctx, uint8_t index, uint32_t offset, uint8_t len, uint8_t *out,
			 uint8_t *out_len)
{
	fake_dev_t *dev = ctx;

	dev->log_read_offset = offset;
	dev->log_read_len = len;
	dev->log_read_count++;

	if (index != 1U) {
		return ZLRS_MGMT_E_PARAM;
	}
	/* Like a file: "hello" from offset 1 is "ello" */
	const char text[] = "hello";
	uint8_t avail = (uint8_t)(sizeof(text) - 1U);

	if (offset > avail) {
		offset = avail;
	}
	avail = (uint8_t)(avail - offset);
	if (len > avail) {
		len = avail;
	}
	for (uint8_t i = 0U; i < len; i++) {
		out[i] = (uint8_t)text[offset + i];
	}
	*out_len = len;

	return ZLRS_MGMT_OK;
}

static const zlrs_mgmt_backend_t backend = {
	.save_root_key = fake_save_key,
	.read_status = fake_read_status,
	.save_setting = fake_save_setting,
	.new_dev_id = fake_new_dev_id,
	.pair = fake_pair,
	.confirm_image = fake_confirm_image,
	.read_build = fake_read_build,
	.read_image_hash = fake_read_image_hash,
	.log_info = fake_log_info,
	.log_read = fake_log_read,
	.ctx = &fake,
};

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

static void test_phrase_vector(void)
{
	uint8_t key[ZLRS_KEY_LEN];

	printf("mgmt: phrase derivation\n");

	/* Vector from Python hashlib.pbkdf2_hmac('sha256', phrase, b'zlrs/kdf/v1', 1e5, 32) */
	zlrs_root_key_from_phrase("correct horse battery staple", key);
	CHECK(zlrs_test_hex_eq(key, sizeof(key),
			       "039690ebe7c3c9f237d80555d083e7a48f781b7a537156a6d0e501f49d60f726"));

	zlrs_root_key_from_phrase("hello", key);
	CHECK(zlrs_test_hex_eq(key, sizeof(key),
			       "7e7363b218016d1f319a6389fa20aaf53d2fe21056ffe30566cc3b874e6073f8"));

	/* The same phrase must derive the same key every time (tool and device each do it once) */
	zlrs_root_key_from_phrase("hello", key);
	CHECK(zlrs_test_hex_eq(key, sizeof(key),
			       "7e7363b218016d1f319a6389fa20aaf53d2fe21056ffe30566cc3b874e6073f8"));
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

static void test_dispatch(void)
{
	uint8_t resp[ZLRS_MGMT_FRAME_MAX_LEN];
	uint8_t payload[ZLRS_MGMT_PAYLOAD_MAX_LEN];
	uint8_t len;
	zlrs_mgmt_parser_t parser;

	printf("mgmt: commands\n");

	memset(&fake, 0, sizeof(fake));

	/* STATUS */
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_STATUS, NULL, 0U, resp, sizeof(resp));
	CHECK(len > 0U);
	CHECK(parse_response(resp, len, &parser));
	CHECK(parser.cmd == ZLRS_MGMT_CMD_STATUS_RESP);
	{
		zlrs_mgmt_status_t status;

		CHECK(zlrs_mgmt_status_decode(ZLRS_MGMT_PAYLOAD(&parser), parser.payload_len,
					     &status));
		CHECK(status.role == 1U);
		CHECK(status.state == 2U);
		CHECK((status.flags & ZLRS_MGMT_FLAG_PAIRED) != 0U);
		CHECK(zlrs_test_hex_eq(status.dev_id, ZLRS_DEV_ID_LEN, "aa0000bb"));
		CHECK(status.lq_percent == 97U);
		CHECK(status.profile == 1U);
	}

	/* BUILD: the build string comes back unchanged */
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_BUILD, NULL, 0U, resp, sizeof(resp));
	CHECK(len > 0U);
	CHECK(parse_response(resp, len, &parser));
	CHECK(parser.cmd == ZLRS_MGMT_CMD_BUILD_RESP);
	CHECK(parser.payload_len == 7U);
	CHECK(memcmp(ZLRS_MGMT_PAYLOAD(&parser), "0.1.0-t", 7U) == 0);

	/* IMAGE_HASH: 32 bytes come back unchanged */
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_IMAGE_HASH, NULL, 0U, resp, sizeof(resp));
	CHECK(len > 0U);
	CHECK(parse_response(resp, len, &parser));
	CHECK(parser.cmd == ZLRS_MGMT_CMD_IMAGE_HASH_RESP);
	CHECK(parser.payload_len == ZLRS_MGMT_IMAGE_HASH_LEN);
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[0] == 0U);
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[ZLRS_MGMT_IMAGE_HASH_LEN - 1U] ==
	      ZLRS_MGMT_IMAGE_HASH_LEN - 1U);

	/* CONFIRM: the backend callback must really fire (the upgrade hangs on it) */
	fake.image_confirmed = false;
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_CONFIRM, NULL, 0U, resp, sizeof(resp));
	CHECK(len > 0U);
	CHECK(parse_response(resp, len, &parser));
	CHECK(parser.cmd == ZLRS_MGMT_CMD_ACK);
	CHECK(parser.payload_len >= 2U);
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_OK);
	CHECK(fake.image_confirmed);

	/* SET_KEY_HEX: a valid 64-character key */
	for (uint8_t i = 0U; i < ZLRS_KEY_LEN; i++) {
		static const char hex[] = "0123456789abcdef";

		payload[i * 2U] = (uint8_t)hex[i % 16U];
		payload[i * 2U + 1U] = (uint8_t)hex[(i / 16U) % 16U];
	}
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_KEY_HEX, payload, 64U, resp,
				sizeof(resp));
	CHECK(len > 0U);
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[0] == ZLRS_MGMT_CMD_SET_KEY_HEX);
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_OK);
	CHECK(fake.key_saved);
	CHECK(fake.saved_key[0] == 0x00U && fake.saved_key[1] == 0x10U);

	/* SET_KEY_HEX: wrong length, illegal characters */
	fake.key_saved = false;
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_KEY_HEX, payload, 63U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_E_PARAM);
	CHECK(!fake.key_saved);

	payload[3] = (uint8_t)'z'; /* not a hex digit */
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_KEY_HEX, payload, 64U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_E_PARAM);
	CHECK(!fake.key_saved);

	/* A failing store has to be reported honestly */
	fake.store_rc = ZLRS_MGMT_E_STORE;
	payload[3] = (uint8_t)'1';
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_KEY_HEX, payload, 64U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_E_STORE);
	fake.store_rc = ZLRS_MGMT_OK;

	/* SET_PHRASE: the device derives the vector above */
	{
		static const char phrase[] = "hello";

		len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_PHRASE, (const uint8_t *)phrase,
					sizeof(phrase) - 1U, resp, sizeof(resp));
		CHECK(parse_response(resp, len, &parser));
		CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_OK);
		CHECK(zlrs_test_hex_eq(
			fake.saved_key, ZLRS_KEY_LEN,
			"7e7363b218016d1f319a6389fa20aaf53d2fe21056ffe30566cc3b874e6073f8"));
	}

	/* An empty phrase is rejected */
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_PHRASE, NULL, 0U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_E_PARAM);

	/* PAIR */
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_PAIR, NULL, 0U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_OK);
	CHECK(fake.pair_count == 1U);

	/* SET_FAILSAFE: a 22-byte channel payload */
	for (uint8_t i = 0U; i < 22U; i++) {
		payload[i] = (uint8_t)(0x40U + i);
	}
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_FAILSAFE, payload, 22U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_OK);
	CHECK(fake.failsafe_saved);
	CHECK(memcmp(fake.failsafe, payload, 22U) == 0);

	/* A wrong length is rejected */
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_FAILSAFE, payload, 21U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_E_PARAM);

	/* SET_BAUD: 4 bytes little-endian */
	payload[0] = 0x00U;
	payload[1] = 0x25U;
	payload[2] = 0x06U; /* 0x00062500 = 400000? see below: 0x25 0x06 -> 0x062500 */
	payload[3] = 0x00U;
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_BAUD, payload, 4U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_OK);
	CHECK(fake.baud_saved);
	CHECK(fake.baud == 0x00062500U);

	/* SET_ROLE: 0 = TX, 1 = RX; anything else is a parameter error */
	payload[0] = 1U;
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_ROLE, payload, 1U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_OK);
	CHECK(fake.role_saved && fake.role == 1U);

	payload[0] = 2U;
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_ROLE, payload, 1U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_E_PARAM);

	/* SET_PROFILE: P1 goes through, P0 is refused by the backend (production-shaped build) */
	payload[0] = 1U;
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_PROFILE, payload, 1U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_OK);
	CHECK(fake.profile_saved && fake.profile == 1U);

	payload[0] = 0U;
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_PROFILE, payload, 1U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_E_PARAM);

	/* Out-of-range profile ids never reach the backend */
	payload[0] = 3U;
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_SET_PROFILE, payload, 1U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_E_PARAM);

	/* NEW_DEV_ID takes no payload and has to reach the backend */
	fake.new_dev_id_count = 0U;
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_NEW_DEV_ID, NULL, 0U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_OK);
	CHECK(fake.new_dev_id_count == 1U);

	/* A payload is a parameter error, and the backend is not called */
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_NEW_DEV_ID, payload, 1U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_E_PARAM);
	CHECK(fake.new_dev_id_count == 1U);

	/* EXIT and an unknown command */
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_EXIT, NULL, 0U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_OK);

	len = zlrs_mgmt_dispatch(&backend, 0x7EU, NULL, 0U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[0] == 0x7EU);
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_E_UNKNOWN);

	/* Log export: the info frame carries count, newest and per-file number plus size */
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_LOG_INFO, NULL, 0U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(parser.cmd == ZLRS_MGMT_CMD_LOG_INFO_RESP);
	const uint8_t *body = ZLRS_MGMT_PAYLOAD(&parser);

	CHECK(parser.payload_len == 2U + 6U * 2U);
	CHECK(body[0] == 2U);
	CHECK(body[1] == 1U);
	CHECK(body[2] == 6U);
	CHECK(body[3] == 0U);
	CHECK(body[4] == 0x44U && body[5] == 0x33U && body[6] == 0x22U && body[7] == 0x11U);
	CHECK(body[8] == 7U);
	CHECK(body[9] == 0U);
	CHECK(body[10] == 5U);

	/* A read: index, offset, length in; index, offset, bytes out */
	payload[0] = 1U;
	payload[1] = 1U;
	payload[2] = 0U;
	payload[3] = 0U;
	payload[4] = 0U;
	payload[5] = 4U;
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_LOG_READ, payload, 6U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(parser.cmd == ZLRS_MGMT_CMD_LOG_DATA_RESP);
	CHECK(parser.payload_len == 5U + 4U);
	body = ZLRS_MGMT_PAYLOAD(&parser);
	CHECK(body[0] == 1U);
	CHECK(body[1] == 1U && body[2] == 0U && body[3] == 0U && body[4] == 0U);
	CHECK(memcmp(&body[5], "ello", 4) == 0);
	CHECK(fake.log_read_offset == 1U);
	CHECK(fake.log_read_len == 4U);

	/* A read at the end of the file returns an empty chunk, not an error */
	const uint8_t before = fake.log_read_count;

	payload[1] = 5U;
	payload[5] = 8U;
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_LOG_READ, payload, 6U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(parser.cmd == ZLRS_MGMT_CMD_LOG_DATA_RESP);
	CHECK(parser.payload_len == 5U);
	CHECK(fake.log_read_count == (uint8_t)(before + 1U));

	/* A chunk longer than a frame can carry never reaches the backend */
	payload[5] = ZLRS_MGMT_LOG_CHUNK_MAX + 1U;
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_LOG_READ, payload, 6U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_E_PARAM);
	CHECK(fake.log_read_count == (uint8_t)(before + 1U));

	/* A short payload is a parameter error too */
	len = zlrs_mgmt_dispatch(&backend, ZLRS_MGMT_CMD_LOG_READ, payload, 5U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_MGMT_PAYLOAD(&parser)[1] == ZLRS_MGMT_E_PARAM);
}

void test_mgmt(void)
{
	test_phrase_vector();
	test_frame_codec();
	test_dispatch();
}

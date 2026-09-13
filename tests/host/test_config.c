/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 配置协议测试：帧编解码用构造帧 + 逐字节解析，命令分发用假 backend 把每条
 * 命令跑一遍（含错误路径）。短语派生用 Python hashlib 算出的固定向量。
 */

#include "test.h"

#include <string.h>

#include "zlrs/config/config.h"
#include "zlrs/proto/crypto.h"

typedef struct {
	uint8_t saved_key[ZLRS_KEY_LEN];
	bool key_saved;
	uint8_t failsafe[22];
	bool failsafe_saved;
	uint32_t baud;
	bool baud_saved;
	uint32_t pair_count;
	int store_rc; /* save_root_key 想返回的错误码 */
} fake_dev_t;

static fake_dev_t fake;

static int fake_save_key(void *ctx, const uint8_t key[ZLRS_KEY_LEN])
{
	fake_dev_t *dev = ctx;

	if (dev->store_rc != ZLRS_CFG_OK) {
		return dev->store_rc;
	}
	memcpy(dev->saved_key, key, ZLRS_KEY_LEN);
	dev->key_saved = true;

	return ZLRS_CFG_OK;
}

static int fake_read_status(void *ctx, zlrs_cfg_status_t *out)
{
	(void)ctx;

	out->version = ZLRS_CFG_VERSION;
	out->role = 1U;
	out->state = 2U;
	out->flags = ZLRS_CFG_FLAG_KEY | ZLRS_CFG_FLAG_PAIRED;
	out->dev_id[0] = 0xAAU;
	out->dev_id[3] = 0xBBU;
	out->peer_id[0] = 0x11U;
	out->lq_percent = 97U;

	return ZLRS_CFG_OK;
}

static int fake_save_failsafe(void *ctx, const uint8_t channels[22])
{
	fake_dev_t *dev = ctx;

	memcpy(dev->failsafe, channels, sizeof(dev->failsafe));
	dev->failsafe_saved = true;

	return ZLRS_CFG_OK;
}

static int fake_save_baud(void *ctx, uint32_t baud)
{
	fake_dev_t *dev = ctx;

	dev->baud = baud;
	dev->baud_saved = true;

	return ZLRS_CFG_OK;
}

static int fake_pair(void *ctx)
{
	fake_dev_t *dev = ctx;

	dev->pair_count++;

	return ZLRS_CFG_OK;
}

static const zlrs_cfg_backend_t backend = {
	.save_root_key = fake_save_key,
	.read_status = fake_read_status,
	.save_failsafe = fake_save_failsafe,
	.save_baud = fake_save_baud,
	.pair = fake_pair,
	.ctx = &fake,
};

/* 把响应帧喂回解析器，取出 cmd 与 payload */
static bool parse_response(const uint8_t *frame, uint8_t len, zlrs_cfg_parser_t *parser)
{
	int result = 0;

	zlrs_cfg_parser_init(parser);
	for (uint8_t i = 0U; i < len; i++) {
		result = zlrs_cfg_feed(parser, frame[i]);
	}

	return result == 1;
}

static void test_phrase_vector(void)
{
	uint8_t key[ZLRS_KEY_LEN];

	printf("config: phrase derivation\n");

	/* 向量由 Python 的 hashlib.pbkdf2_hmac('sha256', phrase, b'zlrs/kdf/v1', 1e5, 32) 算出 */
	zlrs_root_key_from_phrase("correct horse battery staple", key);
	CHECK(zlrs_test_hex_eq(key, sizeof(key),
			       "039690ebe7c3c9f237d80555d083e7a48f781b7a537156a6d0e501f49d60f726"));

	zlrs_root_key_from_phrase("hello", key);
	CHECK(zlrs_test_hex_eq(key, sizeof(key),
			       "7e7363b218016d1f319a6389fa20aaf53d2fe21056ffe30566cc3b874e6073f8"));

	/* 同一个短语必须每次派生一样的结果（配置工具与设备各算一次） */
	zlrs_root_key_from_phrase("hello", key);
	CHECK(zlrs_test_hex_eq(key, sizeof(key),
			       "7e7363b218016d1f319a6389fa20aaf53d2fe21056ffe30566cc3b874e6073f8"));
}

static void test_frame_codec(void)
{
	uint8_t frame[ZLRS_CFG_FRAME_MAX_LEN];
	uint8_t payload[4] = { 1U, 2U, 3U, 4U };
	zlrs_cfg_parser_t parser;
	uint8_t len;

	printf("config: frame codec\n");

	len = zlrs_cfg_encode(frame, sizeof(frame), ZLRS_CFG_CMD_SET_BAUD, payload,
			      sizeof(payload));
	CHECK(len == 10U);
	CHECK(frame[0] == ZLRS_CFG_SOF0 && frame[1] == ZLRS_CFG_SOF1);
	CHECK(frame[2] == ZLRS_CFG_VERSION);
	CHECK(frame[3] == ZLRS_CFG_CMD_SET_BAUD);
	CHECK(frame[4] == sizeof(payload));

	CHECK(parse_response(frame, len, &parser));
	CHECK(parser.cmd == ZLRS_CFG_CMD_SET_BAUD);
	CHECK(parser.payload_len == sizeof(payload));
	CHECK(memcmp(ZLRS_CFG_PAYLOAD(&parser), payload, sizeof(payload)) == 0);
	CHECK(parser.frames_ok == 1U);

	/* 坏 CRC */
	zlrs_cfg_parser_init(&parser);
	for (uint8_t i = 0U; i < len; i++) {
		const uint8_t byte = (i + 1U == len) ? (uint8_t)(frame[i] ^ 0xFFU) : frame[i];
		const int r = zlrs_cfg_feed(&parser, byte);

		if (i + 1U == len) {
			CHECK(r == -1);
		}
	}
	CHECK(parser.frames_ok == 0U);

	/* 版本不对、载荷过长、垃圾前置都要能拒绝并重新同步 */
	zlrs_cfg_parser_init(&parser);
	CHECK(zlrs_cfg_feed(&parser, 0x5AU) == -1);
	CHECK(zlrs_cfg_feed(&parser, ZLRS_CFG_SOF0) == 0);
	CHECK(zlrs_cfg_feed(&parser, ZLRS_CFG_SOF1) == 0);
	CHECK(zlrs_cfg_feed(&parser, 0x7FU) == -1); /* 版本不支持 */
	CHECK(parse_response(frame, len, &parser));

	CHECK(zlrs_cfg_encode(frame, sizeof(frame), ZLRS_CFG_CMD_STATUS, payload,
			      ZLRS_CFG_PAYLOAD_MAX_LEN + 1U) == 0U);
	CHECK(zlrs_cfg_encode(frame, 4U, ZLRS_CFG_CMD_STATUS, NULL, 0U) == 0U);
}

static void test_dispatch(void)
{
	uint8_t resp[ZLRS_CFG_FRAME_MAX_LEN];
	uint8_t payload[ZLRS_CFG_PAYLOAD_MAX_LEN];
	uint8_t len;
	zlrs_cfg_parser_t parser;

	printf("config: commands\n");

	memset(&fake, 0, sizeof(fake));

	/* STATUS */
	len = zlrs_cfg_dispatch(&backend, ZLRS_CFG_CMD_STATUS, NULL, 0U, resp, sizeof(resp));
	CHECK(len > 0U);
	CHECK(parse_response(resp, len, &parser));
	CHECK(parser.cmd == ZLRS_CFG_CMD_STATUS_RESP);
	{
		zlrs_cfg_status_t status;

		CHECK(zlrs_cfg_status_decode(ZLRS_CFG_PAYLOAD(&parser), parser.payload_len,
					     &status));
		CHECK(status.role == 1U);
		CHECK(status.state == 2U);
		CHECK((status.flags & ZLRS_CFG_FLAG_PAIRED) != 0U);
		CHECK(zlrs_test_hex_eq(status.dev_id, ZLRS_DEV_ID_LEN, "aa0000bb"));
		CHECK(status.lq_percent == 97U);
	}

	/* SET_KEY_HEX：合法 64 字符 */
	for (uint8_t i = 0U; i < ZLRS_KEY_LEN; i++) {
		static const char hex[] = "0123456789abcdef";

		payload[i * 2U] = (uint8_t)hex[i % 16U];
		payload[i * 2U + 1U] = (uint8_t)hex[(i / 16U) % 16U];
	}
	len = zlrs_cfg_dispatch(&backend, ZLRS_CFG_CMD_SET_KEY_HEX, payload, 64U, resp,
				sizeof(resp));
	CHECK(len > 0U);
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_CFG_PAYLOAD(&parser)[0] == ZLRS_CFG_CMD_SET_KEY_HEX);
	CHECK(ZLRS_CFG_PAYLOAD(&parser)[1] == ZLRS_CFG_OK);
	CHECK(fake.key_saved);
	CHECK(fake.saved_key[0] == 0x00U && fake.saved_key[1] == 0x10U);

	/* SET_KEY_HEX：长度不对 / 非法字符 */
	fake.key_saved = false;
	len = zlrs_cfg_dispatch(&backend, ZLRS_CFG_CMD_SET_KEY_HEX, payload, 63U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_CFG_PAYLOAD(&parser)[1] == ZLRS_CFG_E_PARAM);
	CHECK(!fake.key_saved);

	payload[3] = (uint8_t)'z'; /* 非法十六进制字符 */
	len = zlrs_cfg_dispatch(&backend, ZLRS_CFG_CMD_SET_KEY_HEX, payload, 64U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_CFG_PAYLOAD(&parser)[1] == ZLRS_CFG_E_PARAM);
	CHECK(!fake.key_saved);

	/* 落盘失败要如实回报 */
	fake.store_rc = ZLRS_CFG_E_STORE;
	payload[3] = (uint8_t)'1';
	len = zlrs_cfg_dispatch(&backend, ZLRS_CFG_CMD_SET_KEY_HEX, payload, 64U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_CFG_PAYLOAD(&parser)[1] == ZLRS_CFG_E_STORE);
	fake.store_rc = ZLRS_CFG_OK;

	/* SET_PHRASE：设备侧派生出上面那个向量 */
	{
		static const char phrase[] = "hello";

		len = zlrs_cfg_dispatch(&backend, ZLRS_CFG_CMD_SET_PHRASE,
					(const uint8_t *)phrase, sizeof(phrase) - 1U, resp,
					sizeof(resp));
		CHECK(parse_response(resp, len, &parser));
		CHECK(ZLRS_CFG_PAYLOAD(&parser)[1] == ZLRS_CFG_OK);
		CHECK(zlrs_test_hex_eq(fake.saved_key, ZLRS_KEY_LEN,
				       "7e7363b218016d1f319a6389fa20aaf53d2fe21056ffe30566cc3b874e6073f8"));
	}

	/* 空短语拒绝 */
	len = zlrs_cfg_dispatch(&backend, ZLRS_CFG_CMD_SET_PHRASE, NULL, 0U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_CFG_PAYLOAD(&parser)[1] == ZLRS_CFG_E_PARAM);

	/* PAIR */
	len = zlrs_cfg_dispatch(&backend, ZLRS_CFG_CMD_PAIR, NULL, 0U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_CFG_PAYLOAD(&parser)[1] == ZLRS_CFG_OK);
	CHECK(fake.pair_count == 1U);

	/* SET_FAILSAFE：22 字节通道载荷 */
	for (uint8_t i = 0U; i < 22U; i++) {
		payload[i] = (uint8_t)(0x40U + i);
	}
	len = zlrs_cfg_dispatch(&backend, ZLRS_CFG_CMD_SET_FAILSAFE, payload, 22U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_CFG_PAYLOAD(&parser)[1] == ZLRS_CFG_OK);
	CHECK(fake.failsafe_saved);
	CHECK(memcmp(fake.failsafe, payload, 22U) == 0);

	/* 长度不对要拒 */
	len = zlrs_cfg_dispatch(&backend, ZLRS_CFG_CMD_SET_FAILSAFE, payload, 21U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_CFG_PAYLOAD(&parser)[1] == ZLRS_CFG_E_PARAM);

	/* SET_BAUD：4 字节小端 */
	payload[0] = 0x00U;
	payload[1] = 0x25U;
	payload[2] = 0x06U; /* 0x00062500 = 400000？见下：0x25 0x06 → 0x062500 */
	payload[3] = 0x00U;
	len = zlrs_cfg_dispatch(&backend, ZLRS_CFG_CMD_SET_BAUD, payload, 4U, resp,
				sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_CFG_PAYLOAD(&parser)[1] == ZLRS_CFG_OK);
	CHECK(fake.baud_saved);
	CHECK(fake.baud == 0x00062500U);

	/* EXIT 与未知命令 */
	len = zlrs_cfg_dispatch(&backend, ZLRS_CFG_CMD_EXIT, NULL, 0U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_CFG_PAYLOAD(&parser)[1] == ZLRS_CFG_OK);

	len = zlrs_cfg_dispatch(&backend, 0x7EU, NULL, 0U, resp, sizeof(resp));
	CHECK(parse_response(resp, len, &parser));
	CHECK(ZLRS_CFG_PAYLOAD(&parser)[0] == 0x7EU);
	CHECK(ZLRS_CFG_PAYLOAD(&parser)[1] == ZLRS_CFG_E_UNKNOWN);
}

void test_config(void)
{
	test_phrase_vector();
	test_frame_codec();
	test_dispatch();
}

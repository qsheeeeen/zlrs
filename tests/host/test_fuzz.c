/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 畸形输入：给三个有状态的解析器（空口帧、配置帧、CRSF）喂随机字节与"接近合法"
 * 的变体，断言它们只会拒绝、不会越界或跑飞。种子写死，失败可复现。
 *
 * 单独跑能验证逻辑不变量；主机测试默认就带 ASan + UBSan（`make -C tests/host run`），
 * 越界读写与未定义行为会被 sanitizer 直接抓出来。想多跑几轮：
 * `make -C tests/host run FUZZ_ITERS=2000000`。
 */

#include "test.h"

#include <string.h>

#include "zlrs/config/config.h"
#include "zlrs/crsf/crsf.h"
#include "zlrs/link/link.h"
#include "zlrs/proto/crypto.h"
#include "zlrs/proto/frame.h"

#ifndef FUZZ_ITERS
#define FUZZ_ITERS 20000U /* 可以用 make FUZZ_ITERS=... 覆盖 */
#endif
#define FUZZ_BUF_LEN 128U

typedef struct {
	uint32_t state;
} rng_t;

static uint32_t rng_next(rng_t *rng)
{
	uint32_t x = rng->state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng->state = (x != 0U) ? x : 0x12345678U;

	return rng->state;
}

static void rng_fill(rng_t *rng, uint8_t *out, uint16_t len)
{
	for (uint16_t i = 0; i < len; i++) {
		out[i] = (uint8_t)rng_next(rng);
	}
}

/* ---- 空口帧：解出来的视图必须都落在输入缓冲区内 ---- */

static void fuzz_frame_decode(rng_t *rng)
{
	uint8_t buf[FUZZ_BUF_LEN];
	uint8_t pack[FUZZ_BUF_LEN];
	uint32_t violations = 0U;
	uint32_t decoded = 0U;

	for (uint32_t i = 0; i < FUZZ_ITERS; i++) {
		const uint16_t len = (uint16_t)(rng_next(rng) % (FUZZ_BUF_LEN + 1U));
		const uint8_t tag_len = ((rng_next(rng) & 1U) != 0U) ? ZLRS_TAG_LEN : 0U;
		zlrs_frame_t frame;

		rng_fill(rng, buf, len);
		if (len >= ZLRS_FRAME_HDR_LEN && (rng_next(rng) % 4U) == 0U) {
			/* 类型字段取合法值，让解析器往下走 */
			buf[0] = (uint8_t)(((rng_next(rng) % 11U) << 2) | (buf[0] & 0x03U));
		}

		if (!zlrs_frame_decode(&frame, buf, len, tag_len)) {
			continue;
		}
		decoded++;

		if (frame.payload_len > 0U &&
		    (frame.payload < buf || frame.payload + frame.payload_len > buf + len)) {
			violations++;
		}
		if (tag_len > 0U &&
		    (frame.tag < buf || frame.tag + ZLRS_TAG_LEN > buf + len)) {
			violations++;
		}
		if (zlrs_frame_mac_len(&frame) > len) {
			violations++;
		}

		/* 解出来再编回去，应当逐字节一致（含 tag） */
		if (zlrs_frame_encode(&frame, pack, sizeof(pack)) != len) {
			violations++;
		} else if (memcmp(pack, buf, len) != 0) {
			violations++;
		}
	}

	CHECK(violations == 0U);
	CHECK(decoded > 0U); /* 别把用例跑成空转 */
}

/* ---- 链路入口：随机字节 + 接近合法的帧（含 tag）都喂进去 ---- */

static void fx_on_rc(void *ctx, const uint8_t *payload, uint16_t len)
{
	(void)ctx;
	(void)payload;
	(void)len;
}

static void fx_on_tlm(void *ctx, const uint8_t *payload, uint16_t len)
{
	(void)ctx;
	(void)payload;
	(void)len;
}

static void fx_on_data(void *ctx, const uint8_t *data, uint16_t len)
{
	(void)ctx;
	(void)data;
	(void)len;
}

static void fx_on_state(void *ctx, uint8_t state)
{
	(void)ctx;
	(void)state;
}

static void fx_on_failsafe(void *ctx, const uint8_t *payload, uint16_t len)
{
	(void)ctx;
	(void)payload;
	(void)len;
}

static int fx_submit(void *ctx, const zlrs_radio_req_t *req)
{
	(void)ctx;
	(void)req;

	return 0;
}

static uint32_t fx_random(void *ctx)
{
	(void)ctx;

	return 0x9E3779B9U;
}

static void fx_link_init(zlrs_link_t *link, uint8_t arq)
{
	static const uint8_t root_key[ZLRS_KEY_LEN] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
		0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
		0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
	};
	static const uint8_t dev_id[ZLRS_DEV_ID_LEN] = { 0x11, 0x22, 0x33, 0x44 };
	static const uint8_t peer_id[ZLRS_DEV_ID_LEN] = { 0xaa, 0xbb, 0xcc, 0xdd };
	static const uint8_t salt[ZLRS_PAIR_SALT_LEN] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U };
	const zlrs_link_cfg_t cfg = {
		.kind = ZLRS_LINK_DATA,
		.role = ZLRS_ROLE_TX,
		.domain = ZLRS_DOMAIN_ISM2400,
		.sync_slot = ZLRS_SYNC_SLOT_DATA,
		.profile = ZLRS_PROFILE_P1,
		.mod = { .modulation = ZLRS_MOD_FLRC, .rate_bps = 2600000U },
		.tx_power_dbm = 10,
		.rx_timeout_ms = 2U,
		.sync_period_ms = 2000U,
		.peer_timeout_ms = 0U,
		.pair_period_ms = 100U,
		.pair_window_ms = 30000U,
		.data_mtu = 128U,
		.arq = arq,
		.hopping = 1U,
		.handshake = 1U,
		.telemetry = 1U,
		.tlm_every_n = 1U,
		.data_every_n = 1U,
	};
	const zlrs_radio_ops_t ops = {
		.submit = fx_submit,
		.random = fx_random,
		.ctx = NULL,
	};
	const zlrs_link_events_t ev = {
		.on_rc = fx_on_rc,
		.on_tlm = fx_on_tlm,
		.on_data = fx_on_data,
		.on_state = fx_on_state,
		.on_failsafe = fx_on_failsafe,
		.ctx = NULL,
	};

	zlrs_link_init(link, &cfg, &ops, &ev);
	zlrs_link_set_dev_id(link, dev_id);
	zlrs_link_set_root_key(link, root_key, 0U);
	zlrs_link_set_peer(link, peer_id, salt, 0U);
}

/* 造一帧形状与 tag 都合法的空口帧；调用方再随机破坏它 */
static uint16_t fx_make_frame(rng_t *rng, uint8_t *out, uint16_t cap, const uint8_t *key)
{
	uint8_t payload[ZLRS_PAYLOAD_MAX_LEN];
	zlrs_frame_t frame = {
		.type = (uint8_t)(rng_next(rng) % 11U),
		.flags = (uint8_t)(rng_next(rng) & 0x03U),
		.seq = (uint16_t)rng_next(rng),
		.payload = payload,
		.payload_len = (uint16_t)(rng_next(rng) % 32U),
	};
	uint16_t total;

	rng_fill(rng, payload, frame.payload_len);
	total = zlrs_frame_len(frame.payload_len, ZLRS_TAG_LEN);
	if (total > cap) {
		return 0U;
	}
	if (zlrs_frame_encode(&frame, out, cap) !=
	    (uint16_t)(ZLRS_FRAME_HDR_LEN + frame.payload_len)) {
		return 0U;
	}
	zlrs_tag(key, ZLRS_KEY_LEN, out, ZLRS_FRAME_HDR_LEN + frame.payload_len,
		 &out[ZLRS_FRAME_HDR_LEN + frame.payload_len]);

	return total;
}

static void fuzz_link_rx(rng_t *rng)
{
	uint8_t buf[FUZZ_BUF_LEN];
	zlrs_link_t best_effort;
	zlrs_link_t arq;
	uint32_t now_ms = 0U;
	uint32_t violations = 0U;

	fx_link_init(&best_effort, 0U);
	fx_link_init(&arq, 1U);

	for (uint32_t i = 0; i < FUZZ_ITERS; i++) {
		zlrs_link_t *link = ((rng_next(rng) & 1U) != 0U) ? &best_effort : &arq;
		uint16_t len = fx_make_frame(rng, buf, sizeof(buf), link->link_auth_in);

		if (len == 0U || (rng_next(rng) % 3U) == 0U) {
			/* 纯随机字节 */
			len = (uint16_t)(rng_next(rng) % 70U);
			rng_fill(rng, buf, len);
		} else {
			/* 合法帧上翻几位，或在随机位置截断 */
			uint8_t flips = (uint8_t)(1U + (rng_next(rng) % 4U));

			for (uint8_t f = 0U; f < flips; f++) {
				buf[rng_next(rng) % len] ^= (uint8_t)(1U << (rng_next(rng) % 8U));
			}
			if ((rng_next(rng) % 4U) == 0U) {
				len = (uint16_t)(rng_next(rng) % (len + 1U));
			}
		}

		now_ms += 7U;
		zlrs_link_rx(link, buf, len, -60, 0, now_ms);

		/* 状态机必须停在合法状态上 */
		if (link->state > ZLRS_STATE_CONNECTED) {
			violations++;
		}
	}

	CHECK(violations == 0U);
	/* 拒绝过东西，说明真的走到解析逻辑里了 */
	CHECK(best_effort.stats.rx_bad_frame + best_effort.stats.rx_bad_tag +
		      arq.stats.rx_bad_frame + arq.stats.rx_bad_tag >
	      0U);
	CHECK(best_effort.stats.rx_frames + arq.stats.rx_frames > 0U);
}

/* ---- 配置帧与 CRSF：逐字节喂随机流 ---- */

static void fuzz_byte_parsers(rng_t *rng)
{
	zlrs_cfg_parser_t cfg;
	zlrs_crsf_parser_t crsf;
	uint32_t cfg_frames = 0U;
	uint32_t crsf_frames = 0U;
	uint32_t violations = 0U;

	zlrs_cfg_parser_init(&cfg);
	zlrs_crsf_parser_init(&crsf);

	for (uint32_t i = 0; i < FUZZ_ITERS; i++) {
		if ((rng_next(rng) % 64U) == 0U) {
			zlrs_cfg_parser_init(&cfg); /* 时不时重同步 */
		}
		if ((rng_next(rng) % 64U) == 0U) {
			zlrs_crsf_parser_init(&crsf);
		}

		switch (zlrs_cfg_feed(&cfg, (uint8_t)rng_next(rng))) {
		case 1: {
			cfg_frames++;
			if (cfg.payload_len > ZLRS_CFG_PAYLOAD_MAX_LEN ||
			    ZLRS_CFG_PAYLOAD(&cfg) + cfg.payload_len >
				    cfg.buf + sizeof(cfg.buf)) {
				violations++;
			}
			break;
		}
		case 0:
		case -1:
			break;
		default:
			violations++;
			break;
		}

		if (zlrs_crsf_feed(&crsf, (uint8_t)rng_next(rng)) == ZLRS_CRSF_FRAME) {
			crsf_frames++;
			if (crsf.payload_len > sizeof(crsf.buf) ||
			    ZLRS_CRSF_PAYLOAD(&crsf) + crsf.payload_len >
				    crsf.buf + sizeof(crsf.buf)) {
				violations++;
			}
		}
	}

	CHECK(violations == 0U);
	CHECK(cfg_frames < FUZZ_ITERS); /* 随机流不该凑出成片的配置帧 */
	CHECK(crsf_frames < FUZZ_ITERS);
}

void test_fuzz(void)
{
	rng_t rng = { .state = 0x2545F491U };

	printf("fuzz: frame decode\n");
	fuzz_frame_decode(&rng);

	printf("fuzz: link rx\n");
	fuzz_link_rx(&rng);

	printf("fuzz: config / crsf parsers\n");
	fuzz_byte_parsers(&rng);
}

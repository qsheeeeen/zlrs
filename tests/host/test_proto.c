/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test.h"

#include <string.h>

#include "zlrs/proto/crypto.h"
#include "zlrs/proto/fhss.h"
#include "zlrs/proto/frag.h"
#include "zlrs/proto/frame.h"

/* ---- SHA-256 / HMAC / HKDF / PBKDF2 -------------------------------------- */

static void test_sha256(void)
{
	static const struct {
		const char *input;
		const char *digest;
	} vectors[] = {
		{"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
		{"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
		{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
	};

	printf("sha256\n");

	for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		uint8_t digest[ZLRS_SHA256_LEN];

		zlrs_sha256(vectors[i].input, strlen(vectors[i].input), digest);
		CHECK(zlrs_test_hex_eq(digest, sizeof(digest), vectors[i].digest));
	}
}

static void test_hmac_sha256(void)
{
	uint8_t key[20];
	uint8_t out[ZLRS_SHA256_LEN];

	printf("hmac-sha256\n");

	/* RFC 4231 用例 1 与 2 */
	memset(key, 0x0b, sizeof(key));
	zlrs_hmac_sha256(key, sizeof(key), (const uint8_t *)"Hi There", 8U, out);
	CHECK(zlrs_test_hex_eq(out, sizeof(out),
			       "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));

	zlrs_hmac_sha256((const uint8_t *)"Jefe", 4U,
			 (const uint8_t *)"what do ya want for nothing?", 28U, out);
	CHECK(zlrs_test_hex_eq(out, sizeof(out),
			       "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
}

static void test_hkdf_sha256(void)
{
	uint8_t ikm[22];
	uint8_t salt[13];
	uint8_t info[10];
	uint8_t prk[ZLRS_SHA256_LEN];
	uint8_t okm[42];

	printf("hkdf-sha256\n");

	/* RFC 5869 用例 1 */
	memset(ikm, 0x0b, sizeof(ikm));
	for (uint8_t i = 0; i < sizeof(salt); i++) {
		salt[i] = i;
	}
	for (uint8_t i = 0; i < sizeof(info); i++) {
		info[i] = (uint8_t)(0xf0U + i);
	}

	zlrs_hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk);
	CHECK(zlrs_test_hex_eq(prk, sizeof(prk),
			       "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"));

	zlrs_hkdf_expand(prk, info, sizeof(info), okm, sizeof(okm));
	CHECK(zlrs_test_hex_eq(okm, sizeof(okm),
			       "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
			       "34007208d5b887185865"));
}

static void test_pbkdf2_sha256(void)
{
	uint8_t out[ZLRS_KEY_LEN];

	printf("pbkdf2-sha256\n");

	/* RFC 7914 第 11 节的 PBKDF2-HMAC-SHA256 向量 */
	zlrs_pbkdf2_sha256("password", (const uint8_t *)"salt", 4U, 1U, out);
	CHECK(zlrs_test_hex_eq(out, sizeof(out),
			       "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));

	zlrs_pbkdf2_sha256("password", (const uint8_t *)"salt", 4U, 4096U, out);
	CHECK(zlrs_test_hex_eq(out, sizeof(out),
			       "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
}

static void test_kdf(void)
{
	uint8_t key[ZLRS_KEY_LEN];
	uint8_t a[ZLRS_SYNC_WORD_LEN];
	uint8_t b[ZLRS_SYNC_WORD_LEN];
	uint8_t c[ZLRS_SYNC_WORD_LEN];
	uint8_t mac[ZLRS_SHA256_LEN];
	uint8_t tag[ZLRS_TAG_LEN];
	uint8_t ctx[4] = { 1U, 2U, 3U, 4U };

	printf("kdf\n");

	memset(key, 0x42, sizeof(key));
	zlrs_kdf(key, sizeof(key), "zlrs/sync/control", NULL, 0U, a, sizeof(a));
	zlrs_kdf(key, sizeof(key), "zlrs/sync/control", NULL, 0U, b, sizeof(b));
	zlrs_kdf(key, sizeof(key), "zlrs/sync/data", NULL, 0U, c, sizeof(c));

	CHECK(memcmp(a, b, sizeof(a)) == 0); /* 同输入可重复 */
	CHECK(memcmp(a, c, sizeof(a)) != 0); /* 标签不同 -> 结果不同（域分离） */

	zlrs_kdf(key, sizeof(key), "zlrs/session", ctx, sizeof(ctx), c, sizeof(c));
	CHECK(memcmp(a, c, sizeof(a)) != 0); /* 带上下文 */
	zlrs_kdf(key, sizeof(key), "zlrs/session", ctx, sizeof(ctx), b, sizeof(b));
	CHECK(memcmp(b, c, sizeof(b)) == 0);

	zlrs_hmac_sha256(key, sizeof(key), ctx, sizeof(ctx), mac);
	zlrs_tag(key, sizeof(key), ctx, sizeof(ctx), tag);
	CHECK(memcmp(tag, mac, sizeof(tag)) == 0);
}

/* ---- 帧格式 -------------------------------------------------------------- */

static void test_frame(void)
{
	uint8_t buf[ZLRS_FRAME_MAX_LEN];
	uint8_t payload[ZLRS_RC_PAYLOAD_LEN];
	uint8_t tag[ZLRS_TAG_LEN] = { 0xde, 0xad, 0xbe, 0xef };
	zlrs_frame_t out;
	zlrs_frame_t in = {
		.type = ZLRS_PKT_RC,
		.flags = ZLRS_FLAG_MORE | ZLRS_FLAG_ACK_REQ,
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
	CHECK(buf[0] == (uint8_t)((ZLRS_PKT_RC << 2) | ZLRS_FLAG_MORE | ZLRS_FLAG_ACK_REQ));
	CHECK(buf[1] == 0x02U && buf[2] == 0x01U); /* 小端 */
	CHECK(buf[3] == 0x5aU);
	CHECK(memcmp(&buf[ZLRS_FRAME_HDR_LEN + sizeof(payload)], tag, sizeof(tag)) == 0);

	CHECK(zlrs_frame_decode(&out, buf, len, ZLRS_TAG_LEN));
	CHECK(out.type == in.type && out.flags == in.flags && out.seq == in.seq);
	CHECK(out.payload_len == sizeof(payload));
	CHECK(out.payload != NULL && memcmp(out.payload, payload, sizeof(payload)) == 0);
	CHECK(out.tag != NULL && memcmp(out.tag, tag, sizeof(tag)) == 0);
	CHECK(zlrs_frame_mac_len(&out) == ZLRS_FRAME_HDR_LEN + sizeof(payload));

	/* P0：不带 tag */
	CHECK(zlrs_frame_decode(&out, buf, ZLRS_FRAME_HDR_LEN + sizeof(payload), 0U));
	CHECK(out.tag == NULL && out.payload_len == sizeof(payload));

	/* 长度与容量边界 */
	CHECK(!zlrs_frame_decode(&out, buf, 2U, 0U));
	CHECK(!zlrs_frame_decode(&out, buf, len, (uint8_t)(ZLRS_TAG_LEN + 1U)));
	CHECK(zlrs_frame_encode(&in, buf, 4U) == 0U);
}

/* ---- 分片重组 ------------------------------------------------------------ */

static void push_chunk(zlrs_reasm_t *reasm, uint16_t seq, uint8_t flags,
		       const char *text, uint32_t now_ms, int *result)
{
	zlrs_frame_t frame = {
		.type = ZLRS_PKT_DATA_UP,
		.flags = flags,
		.seq = seq,
		.payload = (const uint8_t *)text,
		.payload_len = (uint16_t)strlen(text),
	};

	*result = zlrs_reasm_push(reasm, &frame, now_ms);
}

static void test_frag(void)
{
	uint8_t buf[ZLRS_REASM_BUF_LEN];
	zlrs_reasm_t reasm;
	int r = 0;

	printf("frag\n");

	/* 单帧消息 */
	zlrs_reasm_init(&reasm, buf, sizeof(buf));
	push_chunk(&reasm, 10U, 0U, "solo", 0U, &r);
	CHECK(r == ZLRS_REASM_DONE);
	CHECK(reasm.len == 4U && memcmp(buf, "solo", 4U) == 0);

	/* 三段拼接 */
	zlrs_reasm_init(&reasm, buf, sizeof(buf));
	push_chunk(&reasm, 20U, ZLRS_FLAG_MORE, "abc", 0U, &r);
	CHECK(r == ZLRS_REASM_NONE);
	push_chunk(&reasm, 21U, ZLRS_FLAG_MORE, "def", 1U, &r);
	CHECK(r == ZLRS_REASM_NONE);
	push_chunk(&reasm, 22U, 0U, "ghi", 2U, &r);
	CHECK(r == ZLRS_REASM_DONE);
	CHECK(reasm.len == 9U && memcmp(buf, "abcdefghi", 9U) == 0);

	/* 序列缺口：整段丢弃 */
	zlrs_reasm_init(&reasm, buf, sizeof(buf));
	push_chunk(&reasm, 30U, ZLRS_FLAG_MORE, "abc", 0U, &r);
	CHECK(r == ZLRS_REASM_NONE);
	push_chunk(&reasm, 32U, ZLRS_FLAG_MORE, "def", 1U, &r);
	CHECK(r == ZLRS_REASM_DROPPED);
	CHECK(!reasm.active && reasm.len == 0U);

	/* 超时：残留丢弃，新的分片从头开始 */
	zlrs_reasm_init(&reasm, buf, sizeof(buf));
	push_chunk(&reasm, 40U, ZLRS_FLAG_MORE, "abc", 0U, &r);
	CHECK(r == ZLRS_REASM_NONE);
	push_chunk(&reasm, 41U, ZLRS_FLAG_MORE, "xyz", ZLRS_FRAG_TIMEOUT_MS + 5U, &r);
	CHECK(r == ZLRS_REASM_NONE);
	CHECK(reasm.len == 3U && memcmp(buf, "xyz", 3U) == 0);

	/* 超过缓冲：丢弃 */
	zlrs_reasm_init(&reasm, buf, 8U);
	push_chunk(&reasm, 50U, ZLRS_FLAG_MORE, "123456", 0U, &r);
	CHECK(r == ZLRS_REASM_NONE);
	push_chunk(&reasm, 51U, ZLRS_FLAG_MORE, "789", 1U, &r);
	CHECK(r == ZLRS_REASM_DROPPED);
}

/* ---- 跳频表 -------------------------------------------------------------- */

static void test_fhss(void)
{
	uint16_t seen[16];
	zlrs_fhss_t fhss;
	zlrs_fhss_t again;
	uint32_t first;
	uint32_t next;

	printf("fhss\n");

	/* EU868：13 个信道，256 / 13 = 19 块 */
	zlrs_fhss_init(&fhss, ZLRS_DOMAIN_EU868, 0x1234abcdU);
	CHECK(fhss.domain->channel_count == 13U);
	CHECK(fhss.sequence_count == 247U);
	CHECK(fhss.sync_channel == 6U);

	memset(seen, 0, sizeof(seen));
	for (uint16_t i = 0; i < fhss.sequence_count; i++) {
		CHECK(fhss.sequence[i] < 13U);
		seen[fhss.sequence[i]]++;
		if ((i % 13U) == 0U) {
			CHECK(fhss.sequence[i] == fhss.sync_channel); /* 每块首位是同步信道 */
		}
	}
	for (uint8_t ch = 0; ch < 13U; ch++) {
		CHECK(seen[ch] == 19U); /* 等概率使用 */
	}

	/* 同种子两端算出同一张表；换种子换表 */
	zlrs_fhss_init(&again, ZLRS_DOMAIN_EU868, 0x1234abcdU);
	CHECK(memcmp(fhss.sequence, again.sequence, fhss.sequence_count) == 0);
	zlrs_fhss_init(&again, ZLRS_DOMAIN_EU868, 0x1234abceU);
	CHECK(memcmp(fhss.sequence, again.sequence, fhss.sequence_count) != 0);

	zlrs_fhss_init(&fhss, ZLRS_DOMAIN_ISM2400, 0x1234abcdU);
	CHECK(fhss.sequence_count == 237U); /* 256 / 79 = 3 块 */
	CHECK(fhss.domain->channel_count == 79U);
	CHECK(fhss.sequence[fhss.index] == fhss.sync_channel);

	first = zlrs_fhss_current_hz(&fhss);
	CHECK(first == fhss.domain->start_hz +
				(uint32_t)fhss.sync_channel * fhss.domain->step_hz);
	next = zlrs_fhss_next_hz(&fhss);
	CHECK(next != first);
	CHECK(fhss.index == 1U);
	zlrs_fhss_set_index(&fhss, 0U);
	CHECK(zlrs_fhss_current_hz(&fhss) == first);
}

void test_proto(void)
{
	test_sha256();
	test_hmac_sha256();
	test_hkdf_sha256();
	test_pbkdf2_sha256();
	test_kdf();
	test_frame();
	test_frag();
	test_fhss();
}

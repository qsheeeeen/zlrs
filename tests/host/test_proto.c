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
#include "zlrs/proto/tlm.h"

/* ---- KDF and per-frame tag ----------------------------------------------- */

static void test_kdf(void)
{
	uint8_t key[ZLRS_KEY_LEN];
	uint8_t a[ZLRS_SYNC_WORD_LEN];
	uint8_t b[ZLRS_SYNC_WORD_LEN];
	uint8_t tag[ZLRS_TAG_LEN];
	uint8_t ctx[4] = {1U, 2U, 3U, 4U};

	printf("kdf\n");

	memset(key, 0x42, sizeof(key));

	/* Fixed vectors computed independently with Python hashlib/hmac (empty HKDF salt) */
	zlrs_kdf(key, sizeof(key), "zlrs/sync/control", NULL, 0U, a, sizeof(a));
	CHECK(zlrs_test_hex_eq(a, sizeof(a), "c7f3da8b"));
	zlrs_kdf(key, sizeof(key), "zlrs/session", ctx, sizeof(ctx), b, sizeof(b));
	CHECK(zlrs_test_hex_eq(b, sizeof(b), "89e0e3e0"));

	/* Repeatable for the same input; a new label or context must change the result (domain separation) */
	zlrs_kdf(key, sizeof(key), "zlrs/sync/control", NULL, 0U, b, sizeof(b));
	CHECK(memcmp(a, b, sizeof(a)) == 0);
	zlrs_kdf(key, sizeof(key), "zlrs/sync/data", NULL, 0U, b, sizeof(b));
	CHECK(memcmp(a, b, sizeof(a)) != 0);
	zlrs_kdf(key, sizeof(key), "zlrs/session", ctx, sizeof(ctx), a, sizeof(a));
	zlrs_kdf(key, sizeof(key), "zlrs/session", NULL, 0U, b, sizeof(b));
	CHECK(memcmp(a, b, sizeof(a)) != 0);

	/* Per-frame tag: HMAC-SHA256 truncated to 4 bytes */
	zlrs_tag(key, sizeof(key), ctx, sizeof(ctx), tag);
	CHECK(zlrs_test_hex_eq(tag, sizeof(tag), "e1c2dd24"));
}

/* ---- frame format -------------------------------------------------------- */

static void test_frame(void)
{
	uint8_t buf[ZLRS_FRAME_MAX_LEN];
	uint8_t payload[ZLRS_RC_PAYLOAD_LEN];
	uint8_t tag[ZLRS_TAG_LEN] = {0xde, 0xad, 0xbe, 0xef};
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
	CHECK(buf[1] == 0x02U && buf[2] == 0x01U); /* little-endian */
	CHECK(buf[3] == 0x5aU);
	CHECK(memcmp(&buf[ZLRS_FRAME_HDR_LEN + sizeof(payload)], tag, sizeof(tag)) == 0);

	CHECK(zlrs_frame_decode(&out, buf, len, ZLRS_TAG_LEN));
	CHECK(out.type == in.type && out.flags == in.flags && out.seq == in.seq);
	CHECK(out.payload_len == sizeof(payload));
	CHECK(out.payload != NULL && memcmp(out.payload, payload, sizeof(payload)) == 0);
	CHECK(out.tag != NULL && memcmp(out.tag, tag, sizeof(tag)) == 0);
	CHECK(zlrs_frame_mac_len(&out) == ZLRS_FRAME_HDR_LEN + sizeof(payload));

	/* P0: no tag */
	CHECK(zlrs_frame_decode(&out, buf, ZLRS_FRAME_HDR_LEN + sizeof(payload), 0U));
	CHECK(out.tag == NULL && out.payload_len == sizeof(payload));

	/* length and capacity edges */
	CHECK(!zlrs_frame_decode(&out, buf, 2U, 0U));
	CHECK(!zlrs_frame_decode(&out, buf, len, (uint8_t)(ZLRS_TAG_LEN + 1U)));
	CHECK(zlrs_frame_encode(&in, buf, 4U) == 0U);
}

/* ---- reassembly ---------------------------------------------------------- */

static void push_chunk(zlrs_reasm_t *reasm, uint16_t seq, uint8_t flags, const char *text,
		       uint32_t now_ms, int *result)
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

	/* single-frame message */
	zlrs_reasm_init(&reasm, buf, sizeof(buf));
	push_chunk(&reasm, 10U, 0U, "solo", 0U, &r);
	CHECK(r == ZLRS_REASM_DONE);
	CHECK(reasm.len == 4U && memcmp(buf, "solo", 4U) == 0);

	/* three fragments */
	zlrs_reasm_init(&reasm, buf, sizeof(buf));
	push_chunk(&reasm, 20U, ZLRS_FLAG_MORE, "abc", 0U, &r);
	CHECK(r == ZLRS_REASM_NONE);
	push_chunk(&reasm, 21U, ZLRS_FLAG_MORE, "def", 1U, &r);
	CHECK(r == ZLRS_REASM_NONE);
	push_chunk(&reasm, 22U, 0U, "ghi", 2U, &r);
	CHECK(r == ZLRS_REASM_DONE);
	CHECK(reasm.len == 9U && memcmp(buf, "abcdefghi", 9U) == 0);

	/* sequence gap: drop the whole run */
	zlrs_reasm_init(&reasm, buf, sizeof(buf));
	push_chunk(&reasm, 30U, ZLRS_FLAG_MORE, "abc", 0U, &r);
	CHECK(r == ZLRS_REASM_NONE);
	push_chunk(&reasm, 32U, ZLRS_FLAG_MORE, "def", 1U, &r);
	CHECK(r == ZLRS_REASM_DROPPED);
	CHECK(!reasm.active && reasm.len == 0U);

	/* timeout: the leftovers go, a new run starts from scratch */
	zlrs_reasm_init(&reasm, buf, sizeof(buf));
	push_chunk(&reasm, 40U, ZLRS_FLAG_MORE, "abc", 0U, &r);
	CHECK(r == ZLRS_REASM_NONE);
	push_chunk(&reasm, 41U, ZLRS_FLAG_MORE, "xyz", ZLRS_FRAG_TIMEOUT_MS + 5U, &r);
	CHECK(r == ZLRS_REASM_NONE);
	CHECK(reasm.len == 3U && memcmp(buf, "xyz", 3U) == 0);

	/* over the buffer: drop */
	zlrs_reasm_init(&reasm, buf, 8U);
	push_chunk(&reasm, 50U, ZLRS_FLAG_MORE, "123456", 0U, &r);
	CHECK(r == ZLRS_REASM_NONE);
	push_chunk(&reasm, 51U, ZLRS_FLAG_MORE, "789", 1U, &r);
	CHECK(r == ZLRS_REASM_DROPPED);

	/*
	 * ARQ: the sender retransmits frame 61 until it lands, so a run may take arbitrarily long.
	 * Silently dropping what already arrived loses bytes the peer was told were delivered (the
	 * ACK covers the frames before the gap), which is exactly how a passthrough loses a chunk
	 * of every message whenever the air is lossy.
	 */
	zlrs_reasm_init(&reasm, buf, sizeof(buf));
	zlrs_reasm_set_expiry(&reasm, false);
	push_chunk(&reasm, 60U, ZLRS_FLAG_MORE, "abc", 0U, &r);
	CHECK(r == ZLRS_REASM_NONE);
	push_chunk(&reasm, 61U, 0U, "def", ZLRS_FRAG_TIMEOUT_MS * 5U, &r);
	CHECK(r == ZLRS_REASM_DONE);
	CHECK(reasm.len == 6U && memcmp(buf, "abcdef", 6U) == 0);
}

/* ---- hop tables ---------------------------------------------------------- */

static void test_fhss(void)
{
	uint16_t seen[16];
	zlrs_fhss_t fhss;
	zlrs_fhss_t again;
	uint32_t first;
	uint32_t next;

	printf("fhss\n");

	/* Two tables (main 2.4G plus sub-G) from one seed, identical on both ends */
	{
		zlrs_fhss_t dual;
		zlrs_fhss_t dual2;

		zlrs_fhss_init_dual(&dual, ZLRS_DOMAIN_ISM2400, ZLRS_DOMAIN_CN433, 0x5a5a1234U);
		zlrs_fhss_init_dual(&dual2, ZLRS_DOMAIN_ISM2400, ZLRS_DOMAIN_CN433, 0x5a5a1234U);
		CHECK(zlrs_fhss_sub_ready(&dual));
		CHECK(dual.sub_count == (ZLRS_FHSS_SEQUENCE_LEN / 5U) * 5U);
		CHECK(zlrs_fhss_sub_current_hz(&dual) == zlrs_fhss_sub_current_hz(&dual2));
		CHECK(zlrs_fhss_sub_current_hz(&dual) >= 433050000U);
		CHECK(zlrs_fhss_sub_current_hz(&dual) <= 434790000U);
		CHECK(zlrs_fhss_sub_next_hz(&dual) == zlrs_fhss_sub_current_hz(&dual));
		zlrs_fhss_set_sub_index(&dual, 3U);
		CHECK(zlrs_fhss_sub_current_hz(&dual) == zlrs_fhss_sub_current_hz(&dual2) ||
		      dual.sub_index == 3U);

		/* Four mix levels map to how many of every 4 slots land on sub-G */
		CHECK(zlrs_fhss_mix_slots(0U) == 0U);
		CHECK(zlrs_fhss_mix_slots(1U) == 1U);
		CHECK(zlrs_fhss_mix_slots(2U) == 2U);
		CHECK(zlrs_fhss_mix_slots(3U) == 4U);

		/* Without a sub-band the second table is unusable */
		zlrs_fhss_init(&dual, ZLRS_DOMAIN_ISM2400, 0x1U);
		CHECK(!zlrs_fhss_sub_ready(&dual));
		CHECK(zlrs_fhss_sub_current_hz(&dual) == 0U);
	}

	/* EU868: 13 channels, 256 / 13 = 19 blocks */
	zlrs_fhss_init(&fhss, ZLRS_DOMAIN_EU868, 0x1234abcdU);
	CHECK(fhss.domain->channel_count == 13U);
	CHECK(fhss.sequence_count == 247U);
	CHECK(fhss.sync_channel == 6U);

	memset(seen, 0, sizeof(seen));
	for (uint16_t i = 0; i < fhss.sequence_count; i++) {
		CHECK(fhss.sequence[i] < 13U);
		seen[fhss.sequence[i]]++;
		if ((i % 13U) == 0U) {
			CHECK(fhss.sequence[i] ==
			      fhss.sync_channel); /* slot 0 of every block is the sync channel */
		}
	}
	for (uint8_t ch = 0; ch < 13U; ch++) {
		CHECK(seen[ch] == 19U); /* every channel used equally often */
	}

	/* One seed gives one table on both ends; a new seed gives a new table */
	zlrs_fhss_init(&again, ZLRS_DOMAIN_EU868, 0x1234abcdU);
	CHECK(memcmp(fhss.sequence, again.sequence, fhss.sequence_count) == 0);
	zlrs_fhss_init(&again, ZLRS_DOMAIN_EU868, 0x1234abceU);
	CHECK(memcmp(fhss.sequence, again.sequence, fhss.sequence_count) != 0);

	zlrs_fhss_init(&fhss, ZLRS_DOMAIN_ISM2400, 0x1234abcdU);
	CHECK(fhss.sequence_count == 237U); /* 256 / 79 = 3 blocks */
	CHECK(fhss.domain->channel_count == 79U);
	CHECK(fhss.sequence[fhss.index] == fhss.sync_channel);

	first = zlrs_fhss_current_hz(&fhss);
	CHECK(first == fhss.domain->start_hz + (uint32_t)fhss.sync_channel * fhss.domain->step_hz);
	next = zlrs_fhss_next_hz(&fhss);
	CHECK(next != first);
	CHECK(fhss.index == 1U);
	zlrs_fhss_set_index(&fhss, 0U);
	CHECK(zlrs_fhss_current_hz(&fhss) == first);
}

/* Telemetry payload: fixed length, fixed field order, wrong length is rejected */
static void test_tlm(void)
{
	zlrs_tlm_t in;
	zlrs_tlm_t out;
	uint8_t buf[ZLRS_TLM_LEN];
	uint8_t short_buf[ZLRS_TLM_LEN - 1U] = {0};

	printf("tlm payload\n");

	in.lq_percent = 96U;
	in.rssi_neg_dbm = 71U;
	in.snr_db = -4;
	in.rf_mode = 1U;
	in.tx_power = 3U;
	in.state = 4U;
	in.flags = ZLRS_TLM_FLAG_PAIRED | ZLRS_TLM_FLAG_SESSION;

	zlrs_tlm_encode(buf, &in);
	CHECK(zlrs_test_hex_eq(buf, sizeof(buf), "6047fc0103040300"));

	CHECK(zlrs_tlm_decode(buf, sizeof(buf), &out));
	CHECK(memcmp(&in, &out, sizeof(in)) == 0);

	/* A short payload is rejected and the outputs stay untouched */
	memset(&out, 0, sizeof(out));
	CHECK(!zlrs_tlm_decode(short_buf, sizeof(short_buf), &out));
	CHECK(out.lq_percent == 0U);
	CHECK(!zlrs_tlm_decode(buf, 0U, &out));
}

void test_proto(void)
{
	test_kdf();
	test_frame();
	test_frag();
	test_fhss();
	test_tlm();
}

/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test.h"
#include <string.h>

#include "zlrs/proto/frag.h"

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

void test_frag(void)
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

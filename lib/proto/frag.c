/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/proto/frag.h"

#include <string.h>

static void reasm_reset(zlrs_reasm_t *reasm)
{
	reasm->active = false;
	reasm->len = 0;
	reasm->expected_seq = 0;
	reasm->last_ms = 0;
}

static bool reasm_expired(const zlrs_reasm_t *reasm, uint32_t now_ms)
{
	return reasm->active && (uint32_t)(now_ms - reasm->last_ms) > ZLRS_FRAG_TIMEOUT_MS;
}

void zlrs_reasm_init(zlrs_reasm_t *reasm, uint8_t *buf, uint16_t cap)
{
	memset(reasm, 0, sizeof(*reasm));
	reasm->buf = buf;
	reasm->cap = cap;
}

int zlrs_reasm_push(zlrs_reasm_t *reasm, const zlrs_frame_t *frame, uint32_t now_ms)
{
	const bool more = (frame->flags & ZLRS_FLAG_MORE) != 0U;

	if (reasm->buf == NULL || reasm->cap == 0U) {
		return ZLRS_REASM_DROPPED;
	}

	if (reasm_expired(reasm, now_ms)) {
		reasm_reset(reasm);
	}

	if (!reasm->active) {
		if (!more && frame->payload_len <= reasm->cap) {
			/* single-frame message */
			if (frame->payload_len > 0U) {
				memcpy(reasm->buf, frame->payload, frame->payload_len);
			}
			reasm->len = frame->payload_len;
			return ZLRS_REASM_DONE;
		}
		reasm->active = true;
		reasm->len = 0;
		reasm->expected_seq = frame->seq;
	} else if (frame->seq != reasm->expected_seq) {
		/* gap: drop the whole run */
		reasm_reset(reasm);
		return ZLRS_REASM_DROPPED;
	}

	if ((uint32_t)reasm->len + frame->payload_len > reasm->cap) {
		reasm_reset(reasm);
		return ZLRS_REASM_DROPPED;
	}

	if (frame->payload_len > 0U) {
		memcpy(&reasm->buf[reasm->len], frame->payload, frame->payload_len);
		reasm->len = (uint16_t)(reasm->len + frame->payload_len);
	}
	reasm->last_ms = now_ms;
	reasm->expected_seq = (uint16_t)(frame->seq + 1U);

	if (more) {
		return ZLRS_REASM_NONE;
	}

	reasm->active = false;
	return ZLRS_REASM_DONE;
}

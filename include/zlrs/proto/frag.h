/*
 * Reassembly: strictly in seq order, a gap drops the whole run, see doc/08-protocol.md 2.5.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/limits.h"
#include "zlrs/proto/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint8_t *buf; /* reassembly buffer supplied by the caller */
	uint16_t cap;
	uint16_t len; /* bytes collected so far */
	uint16_t expected_seq;
	uint32_t last_ms;
	bool active;
	/*
	 * Whether a half run expires. On by default, which is right for the best-effort path: a
	 * frame lost there never comes back, so the half message can only be dropped. ARQ turns it
	 * off — it retransmits until the frame arrives, so expiring a run does not recover anything,
	 * it throws away bytes that were already received correctly, and the peer has no way to know.
	 */
	bool expire;
} zlrs_reasm_t;

enum {
	ZLRS_REASM_NONE = 0,    /* buffered but still incomplete */
	ZLRS_REASM_DONE = 1,    /* the whole run is in buf, its length in len */
	ZLRS_REASM_DROPPED = 2, /* gap, timeout or oversize: the run is gone */
};

void zlrs_reasm_init(zlrs_reasm_t *reasm, uint8_t *buf, uint16_t cap);

/* Turn the half-run timeout off (ARQ) or back on (best effort). Call after zlrs_reasm_init(). */
void zlrs_reasm_set_expiry(zlrs_reasm_t *reasm, bool enabled);

/*
 * Deliver one frame. A frame without MORE (or with no run in progress) takes the single-frame
 * path and returns DONE straight away.
 */
int zlrs_reasm_push(zlrs_reasm_t *reasm, const zlrs_frame_t *frame, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

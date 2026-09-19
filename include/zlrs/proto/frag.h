/*
 * Reassembly: strictly in seq order, a gap or a timeout drops the whole run, see
 * docs/design/09-protocol.md 2.5.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/config.h"
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
} zlrs_reasm_t;

enum {
	ZLRS_REASM_NONE = 0,    /* buffered but still incomplete */
	ZLRS_REASM_DONE = 1,    /* the whole run is in buf, its length in len */
	ZLRS_REASM_DROPPED = 2, /* gap, timeout or oversize: the run is gone */
};

void zlrs_reasm_init(zlrs_reasm_t *reasm, uint8_t *buf, uint16_t cap);

/*
 * Deliver one frame. A frame without MORE (or with no run in progress) takes the single-frame
 * path and returns DONE straight away.
 */
int zlrs_reasm_push(zlrs_reasm_t *reasm, const zlrs_frame_t *frame, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

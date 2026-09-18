/*
 * 分片重组：严格按 seq 顺序，出现缺口或超时即丢弃整段，见
 * doc/04-protocol.md 2.5。
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
	uint8_t *buf; /* 调用方提供的重组缓冲 */
	uint16_t cap;
	uint16_t len; /* 已收长度 */
	uint16_t expected_seq;
	uint32_t last_ms;
	bool active;
} zlrs_reasm_t;

enum {
	ZLRS_REASM_NONE = 0,  /* 已缓存，还没收齐 */
	ZLRS_REASM_DONE = 1,  /* 整段在 buf 里，长度见 len */
	ZLRS_REASM_DROPPED = 2, /* 缺口、超时或超长，整段已丢弃 */
};

void zlrs_reasm_init(zlrs_reasm_t *reasm, uint8_t *buf, uint16_t cap);

/*
 * 投递一帧。没有 MORE 的帧（或当前没有未完成的分片）走单帧路径，
 * 直接返回 DONE。
 */
int zlrs_reasm_push(zlrs_reasm_t *reasm, const zlrs_frame_t *frame, uint32_t now_ms);

#ifdef __cplusplus
}
#endif


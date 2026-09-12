/*
 * 分片重组，规则见 docs/03-protocol.md 2.5：
 *   - MORE 置位表示后面还有分片
 *   - 严格按 seq 顺序重组，出现缺口即丢弃整段
 *   - 超过 ZLRS_FRAG_TIMEOUT_MS 未收齐也丢弃整段
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_PROTO_FRAG_H_
#define ZLRS_PROTO_FRAG_H_

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

/* 丢弃未完成的段 */
void zlrs_reasm_reset(zlrs_reasm_t *reasm);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_PROTO_FRAG_H_ */

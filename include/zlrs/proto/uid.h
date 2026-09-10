/*
 * 绑定短语 -> UID。
 *
 * UID = MD5("-DMY_BINDING_PHRASE=\"" + phrase + "\"") 的前 6 字节，
 * 空短语得到全 0 UID（表示未绑定）。
 *
 * 推导格式沿用 ExpressLRS 的习惯，这样用户已有的绑定短语含义不变；
 * 这只是一项 UX 约定，不构成任何兼容性要求。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_PROTO_UID_H_
#define ZLRS_PROTO_UID_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zlrs/proto/config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t state[4];
	uint64_t bit_count;
	uint8_t buffer[64];
	size_t buffer_len;
} zlrs_md5_ctx_t;

void zlrs_md5_init(zlrs_md5_ctx_t *ctx);
void zlrs_md5_update(zlrs_md5_ctx_t *ctx, const void *data, size_t len);
void zlrs_md5_final(zlrs_md5_ctx_t *ctx, uint8_t out[16]);

/*
 * 由绑定短语推导 UID。phrase 为 NULL 或空串时写入全 0 UID。
 */
void zlrs_uid_from_binding_phrase(const char *phrase,
				  uint8_t uid[ZLRS_UID_LEN]);

/* 是否已绑定（UID 后 4 字节非全 0） */
bool zlrs_uid_is_bound(const uint8_t uid[ZLRS_UID_LEN]);

/* UID 后 2 字节，用于同步包里的 UID4/UID5 校验 */
void zlrs_uid_tail(const uint8_t uid[ZLRS_UID_LEN], uint8_t *uid4, uint8_t *uid5);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_PROTO_UID_H_ */

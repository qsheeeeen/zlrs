/*
 * 协议核心的密码学原语：SHA-256、HMAC-SHA256、HKDF-SHA256、
 * PBKDF2-HMAC-SHA256 与 4 字节截断标签。
 *
 * 纯 C、无依赖，主机与设备用同一份实现；算法选型见 docs/02-security.md。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_PROTO_CRYPTO_H_
#define ZLRS_PROTO_CRYPTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zlrs/proto/config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZLRS_SHA256_LEN 32U
#define ZLRS_SHA256_BLOCK_LEN 64U

typedef struct {
	uint32_t state[8];
	uint64_t bit_len;
	uint8_t buf[ZLRS_SHA256_BLOCK_LEN];
	uint16_t buf_len;
} zlrs_sha256_ctx_t;

void zlrs_sha256_init(zlrs_sha256_ctx_t *ctx);
void zlrs_sha256_update(zlrs_sha256_ctx_t *ctx, const void *data, size_t len);
void zlrs_sha256_final(zlrs_sha256_ctx_t *ctx, uint8_t out[ZLRS_SHA256_LEN]);
void zlrs_sha256(const void *data, size_t len, uint8_t out[ZLRS_SHA256_LEN]);

void zlrs_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
		      size_t msg_len, uint8_t out[ZLRS_SHA256_LEN]);

/*
 * HKDF-SHA256。zlrs_kdf() 是协议里 KDF(key, label[, ctx]) 的落地形式：
 * extract 用全零盐，expand 的 info = label || ctx。
 */
void zlrs_hkdf_extract(const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
		       size_t ikm_len, uint8_t prk[ZLRS_SHA256_LEN]);
void zlrs_hkdf_expand(const uint8_t prk[ZLRS_SHA256_LEN], const uint8_t *info,
		      size_t info_len, uint8_t *out, size_t out_len);
void zlrs_kdf(const uint8_t *key, size_t key_len, const char *label,
	      const uint8_t *ctx, size_t ctx_len, uint8_t *out, size_t out_len);

/* 短语 -> 根密钥 */
void zlrs_pbkdf2_sha256(const char *phrase, const uint8_t *salt, size_t salt_len,
			uint32_t iterations, uint8_t out[ZLRS_KEY_LEN]);

/* 每包 tag：HMAC-SHA256 截断到 ZLRS_TAG_LEN 字节 */
void zlrs_tag(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
	      uint8_t out[ZLRS_TAG_LEN]);

/* 定长比较，防时序侧信道 */
bool zlrs_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_PROTO_CRYPTO_H_ */

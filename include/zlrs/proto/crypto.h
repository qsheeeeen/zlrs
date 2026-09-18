/*
 * 密码学原语接口：HKDF-SHA256 派生与每包 4 字节截断标签。标签长度、域分离与
 * KDF 上下文拼接都集中在这里，换后端（PSA / 硬件加速）只换 .c，见
 * lib/proto/crypto_psa.c 与 doc/03-security.md。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zlrs/proto/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KDF(key, label[, ctx])：HKDF-SHA256，空盐 extract，info = label || ctx。
 * 派生值与 doc/03-security.md 第 3 节的派生树一一对应。
 */
void zlrs_kdf(const uint8_t *key, size_t key_len, const char *label,
	      const uint8_t *ctx, size_t ctx_len, uint8_t *out, size_t out_len);

/* 每包 tag：HMAC-SHA256 截断到 ZLRS_TAG_LEN 字节 */
void zlrs_tag(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
	      uint8_t out[ZLRS_TAG_LEN]);

/*
 * 短语 → 根密钥：PBKDF2-HMAC-SHA256。
 * salt 里带 KDF 版本号（见 doc/03-security.md 第 8 节），迭代次数由调用方给，
 * 派生规则固定在设备侧，配置工具不能改。
 */
void zlrs_pbkdf2_sha256(const char *phrase, const uint8_t *salt, size_t salt_len,
			uint32_t iterations, uint8_t out[ZLRS_KEY_LEN]);

/* 与上面同一个规则，但用发布版的固定 salt 与迭代次数（配置模式用） */
#define ZLRS_KDF_V1_SALT "zlrs/kdf/v1"
#define ZLRS_KDF_V1_ITERATIONS 100000U

void zlrs_root_key_from_phrase(const char *phrase, uint8_t out[ZLRS_KEY_LEN]);

/* 定长比较，防时序侧信道 */
bool zlrs_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);

#ifdef __cplusplus
}
#endif


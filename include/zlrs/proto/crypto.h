/*
 * 协议核心用到的密码学原语：HKDF-SHA256 派生与每包 4 字节截断标签。
 *
 * 实现在 lib/proto/crypto.c，底层是 mbedTLS（Zephyr 自带的那份）：
 *   - 上层（lib/link）不直接接触 mbedTLS 类型
 *   - 标签长度、域分离标签、KDF 的上下文拼接方式在这里统一
 * 设备侧与主机侧测试链接同一份 mbedTLS 源码，见 tests/host/Makefile。
 *
 * 算法选型见 docs/02-security.md。
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

/*
 * KDF(key, label[, ctx])：HKDF-SHA256，空盐 extract，info = label || ctx。
 * 派生值与 docs/02-security.md 第 3 节的派生树一一对应。
 */
void zlrs_kdf(const uint8_t *key, size_t key_len, const char *label,
	      const uint8_t *ctx, size_t ctx_len, uint8_t *out, size_t out_len);

/* 每包 tag：HMAC-SHA256 截断到 ZLRS_TAG_LEN 字节 */
void zlrs_tag(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
	      uint8_t out[ZLRS_TAG_LEN]);

/* 定长比较，防时序侧信道 */
bool zlrs_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZLRS_PROTO_CRYPTO_H_ */

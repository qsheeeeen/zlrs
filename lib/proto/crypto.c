/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 密码学原语的 mbedTLS 封装，接口见 zlrs/proto/crypto.h。
 */

#include "zlrs/proto/crypto.h"

#include <string.h>

#include <mbedtls/constant_time.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

#define SHA256_LEN 32U

static const mbedtls_md_info_t *sha256_info(void)
{
	return mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
}

static void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
			size_t msg_len, uint8_t out[SHA256_LEN])
{
	(void)mbedtls_md_hmac(sha256_info(), key, key_len, msg, msg_len, out);
}

static void hkdf_extract(const uint8_t *ikm, size_t ikm_len, uint8_t prk[SHA256_LEN])
{
	/* 空盐，等价于 HashLen 个零字节 */
	(void)mbedtls_hkdf_extract(sha256_info(), NULL, 0U, ikm, ikm_len, prk);
}

static void hkdf_expand(const uint8_t prk[SHA256_LEN], const uint8_t *info,
			size_t info_len, uint8_t *out, size_t out_len)
{
	(void)mbedtls_hkdf_expand(sha256_info(), prk, SHA256_LEN, info, info_len, out,
				  out_len);
}

void zlrs_kdf(const uint8_t *key, size_t key_len, const char *label,
	      const uint8_t *ctx, size_t ctx_len, uint8_t *out, size_t out_len)
{
	uint8_t prk[SHA256_LEN];
	uint8_t info[96];
	const size_t label_len = strlen(label);
	size_t info_len;

	if (label_len + ctx_len > sizeof(info)) {
		memset(out, 0, out_len);
		return;
	}

	memcpy(info, label, label_len);
	info_len = label_len;
	if (ctx_len > 0U && ctx != NULL) {
		memcpy(&info[info_len], ctx, ctx_len);
		info_len += ctx_len;
	}

	hkdf_extract(key, key_len, prk);
	hkdf_expand(prk, info, info_len, out, out_len);

	memset(prk, 0, sizeof(prk));
	memset(info, 0, sizeof(info));
}

void zlrs_tag(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
	      uint8_t out[ZLRS_TAG_LEN])
{
	uint8_t mac[SHA256_LEN];

	hmac_sha256(key, key_len, data, len, mac);
	memcpy(out, mac, ZLRS_TAG_LEN);
	memset(mac, 0, sizeof(mac));
}

bool zlrs_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
	return mbedtls_ct_memcmp(a, b, len) == 0;
}

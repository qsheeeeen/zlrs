/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 密码学实现（设备侧默认）：原语走 Zephyr 的 PSA Crypto。provider 通常是 mbedTLS，
 * 换成带硬件加速的 provider 不用改这里——Zephyr 的 PSA 提供者由 Kconfig 选。
 *
 * 接口与语义见 zlrs/proto/crypto.h，必须和 crypto_soft.c 给出同一串字节：主机侧
 * 用例跑的是那份软件实现（tests/host），设备侧跑的是这里，两边不一致就等于配不出
 * 同一把钥匙。
 *
 * 两处刻意的选择：
 *   - HKDF 用 PSA 的算法。空盐写成 32 个 0：HMAC 会把短 key 补零到分组长，空盐与
 *     HashLen 个 0 是同一个 HMAC key，所以与软件实现一致（主机上逐字节比对过）。
 *   - PBKDF2 用 PSA 的 HMAC 按 RFC 8018 组合，不用 PSA_ALG_PBKDF2_HMAC：不是每个
 *     provider/配置都提供它，而 PBKDF2 只是 HMAC 的循环。
 */

#include <psa/crypto.h>

#include <string.h>

#include "zlrs/proto/crypto.h"

#define SHA256_LEN 32U
#define INFO_MAX 96U

/* 主机侧的 crypto_soft.c 里也有一份（各文件自包含），改动要两边同步 */
void zlrs_root_key_from_phrase(const char *phrase, uint8_t out[ZLRS_KEY_LEN])
{
	static const char salt[] = ZLRS_KDF_V1_SALT;

	zlrs_pbkdf2_sha256(phrase, (const uint8_t *)salt, sizeof(salt) - 1U,
			   ZLRS_KDF_V1_ITERATIONS, out);
}

bool zlrs_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
	uint8_t diff = 0U;

	for (size_t i = 0; i < len; i++) {
		diff |= (uint8_t)(a[i] ^ b[i]);
	}

	return diff == 0U;
}

/* 导一把 HMAC 钥匙，做完一串运算再销毁。PSA 的 key slot 数量随 provider 不同，
 * 所以不常驻：每帧多一次导入的开销，换来的是不依赖 slot 配置。 */
static bool hmac_key_import(const uint8_t *key, size_t key_len, psa_key_id_t *out)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_status_t st;
	bool ok;

	if (psa_crypto_init() != PSA_SUCCESS) {
		return false;
	}

	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_bits(&attr, (size_t)key_len * 8U);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));

	st = psa_import_key(&attr, key, key_len, out);
	psa_reset_key_attributes(&attr);
	ok = (st == PSA_SUCCESS);

	return ok;
}

static bool hmac_key_compute(psa_key_id_t key_id, const uint8_t *msg, size_t len,
			     uint8_t out[SHA256_LEN])
{
	size_t mac_len = 0U;
	const psa_status_t st = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, len,
						out, SHA256_LEN, &mac_len);

	return st == PSA_SUCCESS && mac_len == SHA256_LEN;
}

static bool hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t len,
			uint8_t out[SHA256_LEN])
{
	psa_key_id_t key_id = PSA_KEY_ID_NULL;
	bool ok;

	if (!hmac_key_import(key, key_len, &key_id)) {
		return false;
	}

	ok = hmac_key_compute(key_id, msg, len, out);
	(void)psa_destroy_key(key_id);

	return ok;
}

void zlrs_kdf(const uint8_t *key, size_t key_len, const char *label, const uint8_t *ctx,
	      size_t ctx_len, uint8_t *out, size_t out_len)
{
	uint8_t info[INFO_MAX];
	uint8_t salt[SHA256_LEN] = { 0 };
	const size_t label_len = strlen(label);
	const size_t info_len = label_len + ctx_len;
	psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key_id = PSA_KEY_ID_NULL;
	bool ok = false;

	if (info_len > sizeof(info) || psa_crypto_init() != PSA_SUCCESS) {
		goto out;
	}

	memcpy(info, label, label_len);
	if (ctx_len > 0U && ctx != NULL) {
		memcpy(&info[label_len], ctx, ctx_len);
	}

	psa_set_key_type(&attr, PSA_KEY_TYPE_DERIVE);
	psa_set_key_bits(&attr, (size_t)key_len * 8U);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attr, PSA_ALG_HKDF(PSA_ALG_SHA_256));

	if (psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256)) == PSA_SUCCESS &&
	    psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt,
					   sizeof(salt)) == PSA_SUCCESS &&
	    psa_import_key(&attr, key, key_len, &key_id) == PSA_SUCCESS &&
	    psa_key_derivation_input_key(&op, PSA_KEY_DERIVATION_INPUT_SECRET, key_id) ==
		    PSA_SUCCESS &&
	    psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, info,
					   info_len) == PSA_SUCCESS &&
	    psa_key_derivation_output_bytes(&op, out, out_len) == PSA_SUCCESS) {
		ok = true;
	}

out:
	if (key_id != PSA_KEY_ID_NULL) {
		(void)psa_destroy_key(key_id);
	}
	psa_key_derivation_abort(&op);
	psa_reset_key_attributes(&attr);
	memset(info, 0, sizeof(info));
	if (!ok) {
		/* 失败就输出全零：认证必然过不了（fail closed），不会静默放行 */
		memset(out, 0, out_len);
	}
}

void zlrs_pbkdf2_sha256(const char *phrase, const uint8_t *salt, size_t salt_len,
			uint32_t iterations, uint8_t out[ZLRS_KEY_LEN])
{
	const uint8_t *pw = (const uint8_t *)phrase;
	const size_t pw_len = strlen(phrase);
	uint8_t block[ZLRS_KEY_LEN + 4U];
	uint8_t u[SHA256_LEN];
	uint8_t t[SHA256_LEN];
	psa_key_id_t key_id = PSA_KEY_ID_NULL;
	bool ok;

	if (iterations == 0U || salt_len + 4U > sizeof(block)) {
		memset(out, 0, ZLRS_KEY_LEN);
		return;
	}

	/* RFC 8018：T = U1 ^ U2 ^ ... ^ Uc，输出长度等于 SHA-256 输出，单块即可 */
	memcpy(block, salt, salt_len);
	block[salt_len] = 0U;
	block[salt_len + 1U] = 0U;
	block[salt_len + 2U] = 0U;
	block[salt_len + 3U] = 1U;

	ok = hmac_key_import(pw, pw_len, &key_id) &&
	     hmac_key_compute(key_id, block, salt_len + 4U, u);
	if (ok) {
		memcpy(t, u, sizeof(t));
	}

	for (uint32_t i = 1U; ok && i < iterations; i++) {
		uint8_t next[SHA256_LEN];

		ok = hmac_key_compute(key_id, u, sizeof(u), next);
		if (!ok) {
			break;
		}
		memcpy(u, next, sizeof(u));
		for (uint8_t j = 0U; j < sizeof(t); j++) {
			t[j] ^= u[j];
		}
	}

	if (key_id != PSA_KEY_ID_NULL) {
		(void)psa_destroy_key(key_id);
	}

	if (ok) {
		memcpy(out, t, ZLRS_KEY_LEN);
	} else {
		memset(out, 0, ZLRS_KEY_LEN);
	}

	memset(u, 0, sizeof(u));
	memset(t, 0, sizeof(t));
	memset(block, 0, sizeof(block));
}

void zlrs_tag(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
	      uint8_t out[ZLRS_TAG_LEN])
{
	uint8_t mac[SHA256_LEN];

	if (!hmac_sha256(key, key_len, data, len, mac)) {
		memset(out, 0, ZLRS_TAG_LEN);
		return;
	}

	memcpy(out, mac, ZLRS_TAG_LEN);
	memset(mac, 0, sizeof(mac));
}

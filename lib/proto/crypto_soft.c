/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 密码学实现：纯 C、无依赖，设备与主机侧测试用同一份代码。
 * 固定向量见 tests/host/test_proto.c（HKDF 与 tag 各一组，独立来源）。
 *
 * 设计上这里是可替换的一层：接口见 zlrs/proto/crypto.h。将来要接 Zephyr 的
 * PSA Crypto（比如换用带硬件加速的 provider）时，替换本文件即可。
 */

#include "zlrs/proto/crypto.h"

#define SHA256_LEN 32U
#define SHA256_BLOCK_LEN 64U

typedef struct {
	uint32_t state[8];
	uint64_t bit_len;
	uint8_t buf[SHA256_BLOCK_LEN];
	uint16_t buf_len;
} zlrs_sha256_ctx_t;

#include <string.h>

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32U - (n))))

#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR32(x, 2) ^ ROTR32(x, 13) ^ ROTR32(x, 22))
#define BSIG1(x) (ROTR32(x, 6) ^ ROTR32(x, 11) ^ ROTR32(x, 25))
#define SSIG0(x) (ROTR32(x, 7) ^ ROTR32(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR32(x, 17) ^ ROTR32(x, 19) ^ ((x) >> 10))

static const uint32_t sha256_k[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
	0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
	0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
	0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
	0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
	0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
	0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
	0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void sha256_transform(zlrs_sha256_ctx_t *ctx, const uint8_t *block)
{
	uint32_t w[64];
	uint32_t a = ctx->state[0];
	uint32_t b = ctx->state[1];
	uint32_t c = ctx->state[2];
	uint32_t d = ctx->state[3];
	uint32_t e = ctx->state[4];
	uint32_t f = ctx->state[5];
	uint32_t g = ctx->state[6];
	uint32_t h = ctx->state[7];

	for (uint8_t i = 0; i < 16U; i++) {
		w[i] = ((uint32_t)block[i * 4U] << 24) |
		       ((uint32_t)block[i * 4U + 1U] << 16) |
		       ((uint32_t)block[i * 4U + 2U] << 8) |
		       (uint32_t)block[i * 4U + 3U];
	}
	for (uint8_t i = 16U; i < 64U; i++) {
		w[i] = SSIG1(w[i - 2U]) + w[i - 7U] + SSIG0(w[i - 15U]) + w[i - 16U];
	}

	for (uint8_t i = 0; i < 64U; i++) {
		const uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + sha256_k[i] + w[i];
		const uint32_t t2 = BSIG0(a) + MAJ(a, b, c);

		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

void zlrs_sha256_init(zlrs_sha256_ctx_t *ctx)
{
	ctx->state[0] = 0x6a09e667U;
	ctx->state[1] = 0xbb67ae85U;
	ctx->state[2] = 0x3c6ef372U;
	ctx->state[3] = 0xa54ff53aU;
	ctx->state[4] = 0x510e527fU;
	ctx->state[5] = 0x9b05688cU;
	ctx->state[6] = 0x1f83d9abU;
	ctx->state[7] = 0x5be0cd19U;
	ctx->bit_len = 0;
	ctx->buf_len = 0;
}

void zlrs_sha256_update(zlrs_sha256_ctx_t *ctx, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;

	ctx->bit_len += (uint64_t)len * 8U;

	while (len > 0U) {
		size_t take = SHA256_BLOCK_LEN - ctx->buf_len;

		if (take > len) {
			take = len;
		}
		memcpy(&ctx->buf[ctx->buf_len], p, take);
		ctx->buf_len = (uint16_t)(ctx->buf_len + take);
		p += take;
		len -= take;

		if (ctx->buf_len == SHA256_BLOCK_LEN) {
			sha256_transform(ctx, ctx->buf);
			ctx->buf_len = 0;
		}
	}
}

void zlrs_sha256_final(zlrs_sha256_ctx_t *ctx, uint8_t out[SHA256_LEN])
{
	const uint64_t bits = ctx->bit_len;
	uint8_t pad[SHA256_BLOCK_LEN * 2U];
	size_t pad_len;

	memset(pad, 0, sizeof(pad));
	pad[0] = 0x80U;
	pad_len = (ctx->buf_len < 56U) ? (56U - ctx->buf_len) : (120U - ctx->buf_len);

	/* 长度字段按大端追加在填充之后 */
	for (uint8_t i = 0; i < 8U; i++) {
		pad[pad_len + i] = (uint8_t)(bits >> (56U - 8U * i));
	}
	zlrs_sha256_update(ctx, pad, pad_len + 8U);

	for (uint8_t i = 0; i < 8U; i++) {
		out[i * 4U] = (uint8_t)(ctx->state[i] >> 24);
		out[i * 4U + 1U] = (uint8_t)(ctx->state[i] >> 16);
		out[i * 4U + 2U] = (uint8_t)(ctx->state[i] >> 8);
		out[i * 4U + 3U] = (uint8_t)(ctx->state[i]);
	}

	memset(ctx, 0, sizeof(*ctx));
}

void zlrs_sha256(const void *data, size_t len, uint8_t out[SHA256_LEN])
{
	zlrs_sha256_ctx_t ctx;

	zlrs_sha256_init(&ctx);
	zlrs_sha256_update(&ctx, data, len);
	zlrs_sha256_final(&ctx, out);
}

void zlrs_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
		      size_t msg_len, uint8_t out[SHA256_LEN])
{
	uint8_t k[SHA256_BLOCK_LEN];
	uint8_t pad[SHA256_BLOCK_LEN];
	uint8_t inner[SHA256_LEN];
	zlrs_sha256_ctx_t ctx;

	memset(k, 0, sizeof(k));
	if (key_len > SHA256_BLOCK_LEN) {
		zlrs_sha256(key, key_len, k);
	} else if (key_len > 0U) {
		memcpy(k, key, key_len);
	}

	zlrs_sha256_init(&ctx);
	for (uint8_t i = 0; i < sizeof(k); i++) {
		pad[i] = (uint8_t)(k[i] ^ 0x36U);
	}
	zlrs_sha256_update(&ctx, pad, sizeof(pad));
	zlrs_sha256_update(&ctx, msg, msg_len);
	zlrs_sha256_final(&ctx, inner);

	zlrs_sha256_init(&ctx);
	for (uint8_t i = 0; i < sizeof(k); i++) {
		pad[i] = (uint8_t)(k[i] ^ 0x5cU);
	}
	zlrs_sha256_update(&ctx, pad, sizeof(pad));
	zlrs_sha256_update(&ctx, inner, sizeof(inner));
	zlrs_sha256_final(&ctx, out);

	memset(k, 0, sizeof(k));
	memset(pad, 0, sizeof(pad));
	memset(inner, 0, sizeof(inner));
}

void zlrs_hkdf_extract(const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
		       size_t ikm_len, uint8_t prk[SHA256_LEN])
{
	static const uint8_t zeros[SHA256_LEN] = { 0 };

	if (salt == NULL || salt_len == 0U) {
		salt = zeros;
		salt_len = sizeof(zeros);
	}
	zlrs_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

void zlrs_hkdf_expand(const uint8_t prk[SHA256_LEN], const uint8_t *info,
		      size_t info_len, uint8_t *out, size_t out_len)
{
	uint8_t t[SHA256_LEN];
	uint8_t msg[SHA256_LEN + 96U + 1U];
	size_t t_len = 0;
	size_t done = 0;
	uint8_t counter = 1U;

	if (info_len + 1U > sizeof(msg) - sizeof(t)) {
		memset(out, 0, out_len);
		return;
	}

	while (done < out_len) {
		size_t msg_len = 0;
		size_t take;

		/* T(n) = HMAC(prk, T(n-1) || info || n) */
		if (t_len > 0U) {
			memcpy(msg, t, t_len);
			msg_len += t_len;
		}
		if (info_len > 0U) {
			memcpy(&msg[msg_len], info, info_len);
			msg_len += info_len;
		}
		msg[msg_len] = counter;
		msg_len += 1U;

		zlrs_hmac_sha256(prk, SHA256_LEN, msg, msg_len, t);
		t_len = SHA256_LEN;

		take = out_len - done;
		if (take > t_len) {
			take = t_len;
		}
		memcpy(&out[done], t, take);
		done += take;
		counter++;
	}

	memset(t, 0, sizeof(t));
	memset(msg, 0, sizeof(msg));
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

	zlrs_hkdf_extract(NULL, 0, key, key_len, prk);
	zlrs_hkdf_expand(prk, info, info_len, out, out_len);

	memset(prk, 0, sizeof(prk));
	memset(info, 0, sizeof(info));
}

void zlrs_pbkdf2_sha256(const char *phrase, const uint8_t *salt, size_t salt_len,
			uint32_t iterations, uint8_t out[ZLRS_KEY_LEN])
{
	const uint8_t *pw = (const uint8_t *)phrase;
	const size_t pw_len = strlen(phrase);
	uint8_t block[ZLRS_KEY_LEN + 4U];
	uint8_t u[SHA256_LEN];
	uint8_t t[SHA256_LEN];

	if (iterations == 0U || salt_len + 4U > sizeof(block)) {
		memset(out, 0, ZLRS_KEY_LEN);
		return;
	}

	memcpy(block, salt, salt_len);
	block[salt_len] = 0U;
	block[salt_len + 1U] = 0U;
	block[salt_len + 2U] = 0U;
	block[salt_len + 3U] = 1U;

	/* 输出长度等于 SHA-256 输出，单块即可 */
	zlrs_hmac_sha256(pw, pw_len, block, salt_len + 4U, u);
	memcpy(t, u, sizeof(t));

	for (uint32_t i = 1U; i < iterations; i++) {
		zlrs_hmac_sha256(pw, pw_len, u, sizeof(u), u);
		for (uint8_t j = 0; j < sizeof(t); j++) {
			t[j] ^= u[j];
		}
	}

	memcpy(out, t, ZLRS_KEY_LEN);

	memset(u, 0, sizeof(u));
	memset(t, 0, sizeof(t));
	memset(block, 0, sizeof(block));
}

void zlrs_tag(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
	      uint8_t out[ZLRS_TAG_LEN])
{
	uint8_t mac[SHA256_LEN];

	zlrs_hmac_sha256(key, key_len, data, len, mac);
	memcpy(out, mac, ZLRS_TAG_LEN);
	memset(mac, 0, sizeof(mac));
}

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

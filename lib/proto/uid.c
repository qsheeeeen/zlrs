/*
 * MD5（RFC 1321）。只用于从绑定短语推导 UID，不是安全用途。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/proto/uid.h"

#include <string.h>

#define ZLRS_BIND_KEY "-DMY_BINDING_PHRASE=\""

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32U - (n))))

static const uint32_t md5_k[64] = {
	0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU,
	0xa8304613U, 0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
	0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U,
	0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
	0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U,
	0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
	0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU,
	0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
	0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U,
	0xffeff47dU, 0x85845dd1U, 0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
	0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U
};

static const uint8_t md5_s[64] = {
	7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
	5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static void md5_transform(uint32_t state[4], const uint8_t block[64])
{
	uint32_t m[16];
	uint32_t a = state[0];
	uint32_t b = state[1];
	uint32_t c = state[2];
	uint32_t d = state[3];

	for (uint8_t i = 0; i < 16U; i++) {
		m[i] = (uint32_t)block[i * 4U] | ((uint32_t)block[i * 4U + 1U] << 8) |
		       ((uint32_t)block[i * 4U + 2U] << 16) |
		       ((uint32_t)block[i * 4U + 3U] << 24);
	}

	/*
	 * RFC 1321 的 64 步是四轮串行：第 1..16 步全用 F，第 17..32 步全用 G，
	 * 依此类推。四个轮次的轮换由 a/d/c/b 的赋值顺序体现。
	 */
	for (uint8_t i = 0; i < 64U; i++) {
		uint32_t f;
		uint8_t g;

		if (i < 16U) {
			f = F(b, c, d);
			g = i;
		} else if (i < 32U) {
			f = G(b, c, d);
			g = (uint8_t)((5U * i + 1U) & 15U);
		} else if (i < 48U) {
			f = H(b, c, d);
			g = (uint8_t)((3U * i + 5U) & 15U);
		} else {
			f = I(b, c, d);
			g = (uint8_t)((7U * i) & 15U);
		}

		f = a + f + md5_k[i] + m[g];
		a = d;
		d = c;
		c = b;
		b = b + ROTL32(f, md5_s[i]);
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
}

void zlrs_md5_init(zlrs_md5_ctx_t *ctx)
{
	ctx->state[0] = 0x67452301U;
	ctx->state[1] = 0xefcdab89U;
	ctx->state[2] = 0x98badcfeU;
	ctx->state[3] = 0x10325476U;
	ctx->bit_count = 0;
	ctx->buffer_len = 0;
}

void zlrs_md5_update(zlrs_md5_ctx_t *ctx, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;

	ctx->bit_count += (uint64_t)len * 8U;

	while (len > 0U) {
		size_t take = 64U - ctx->buffer_len;

		if (take > len) {
			take = len;
		}
		memcpy(&ctx->buffer[ctx->buffer_len], p, take);
		ctx->buffer_len += take;
		p += take;
		len -= take;

		if (ctx->buffer_len == 64U) {
			md5_transform(ctx->state, ctx->buffer);
			ctx->buffer_len = 0;
		}
	}
}

void zlrs_md5_final(zlrs_md5_ctx_t *ctx, uint8_t out[16])
{
	uint64_t bits = ctx->bit_count;
	uint8_t pad[72];
	size_t pad_len;

	memset(pad, 0, sizeof(pad));
	pad[0] = 0x80U;
	pad_len = (ctx->buffer_len < 56U) ? (56U - ctx->buffer_len)
					  : (120U - ctx->buffer_len);

	/* 长度字段按小端追加在填充之后 */
	for (uint8_t i = 0; i < 8U; i++) {
		pad[pad_len + i] = (uint8_t)(bits >> (8U * i));
	}

	zlrs_md5_update(ctx, pad, pad_len + 8U);

	for (uint8_t i = 0; i < 4U; i++) {
		out[i * 4U] = (uint8_t)(ctx->state[i]);
		out[i * 4U + 1U] = (uint8_t)(ctx->state[i] >> 8);
		out[i * 4U + 2U] = (uint8_t)(ctx->state[i] >> 16);
		out[i * 4U + 3U] = (uint8_t)(ctx->state[i] >> 24);
	}

	memset(ctx, 0, sizeof(*ctx));
}

void zlrs_uid_from_binding_phrase(const char *phrase, uint8_t uid[ZLRS_UID_LEN])
{
	zlrs_md5_ctx_t md5;
	uint8_t digest[16];

	memset(uid, 0, ZLRS_UID_LEN);
	if (phrase == NULL || phrase[0] == '\0') {
		return;
	}

	zlrs_md5_init(&md5);
	zlrs_md5_update(&md5, ZLRS_BIND_KEY, sizeof(ZLRS_BIND_KEY) - 1U);
	zlrs_md5_update(&md5, phrase, strlen(phrase));
	zlrs_md5_update(&md5, "\"", 1U);
	zlrs_md5_final(&md5, digest);

	memcpy(uid, digest, ZLRS_UID_LEN);
}

bool zlrs_uid_is_bound(const uint8_t uid[ZLRS_UID_LEN])
{
	return (uid[2] != 0U) || (uid[3] != 0U) || (uid[4] != 0U) || (uid[5] != 0U);
}

void zlrs_uid_tail(const uint8_t uid[ZLRS_UID_LEN], uint8_t *uid4, uint8_t *uid5)
{
	*uid4 = uid[4];
	*uid5 = uid[5];
}

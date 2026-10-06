/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Protocol authentication and key derivation. Firmware and host tests share these
 * HMAC, HKDF and PBKDF2 rules; only the SHA-256 operation is supplied separately.
 */

#include "zlrs/proto/crypto.h"
#include "sha256.h"

#include <stdalign.h>
#include <string.h>

#define SHA256_LEN       ZLRS_SHA256_LEN
#define SHA256_BLOCK_LEN 64U
#define INFO_MAX         96U

_Static_assert(ZLRS_KEY_LEN == SHA256_LEN, "The protocol uses 32-byte keys");

struct hmac_key {
	alignas(uint32_t) uint8_t inner[SHA256_BLOCK_LEN];
	alignas(uint32_t) uint8_t outer[SHA256_BLOCK_LEN];
};

static void wipe(void *buf, size_t len)
{
	volatile uint8_t *p = buf;

	while (len-- > 0U) {
		*p++ = 0U;
	}
}

static void hmac_key_init(struct hmac_key *pads, const uint8_t *key, size_t key_len)
{
	uint8_t digest[SHA256_LEN];

	if (key_len > SHA256_BLOCK_LEN) {
		zlrs_sha256(key, key_len, NULL, 0U, digest);
		key = digest;
		key_len = sizeof(digest);
	}
	memset(pads->inner, 0x36, sizeof(pads->inner));
	memset(pads->outer, 0x5c, sizeof(pads->outer));
	for (size_t i = 0U; i < key_len; i++) {
		pads->inner[i] ^= key[i];
		pads->outer[i] ^= key[i];
	}
	wipe(digest, sizeof(digest));
}

static void hmac_compute(const struct hmac_key *pads, const uint8_t *data, size_t len,
			 uint8_t out[SHA256_LEN])
{
	alignas(uint32_t) uint8_t inner[SHA256_LEN];

	zlrs_sha256(pads->inner, sizeof(pads->inner), data, len, inner);
	zlrs_sha256(pads->outer, sizeof(pads->outer), inner, sizeof(inner), out);
	wipe(inner, sizeof(inner));
}

void zlrs_tag(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
	      uint8_t out[ZLRS_TAG_LEN])
{
	struct hmac_key pads;
	uint8_t mac[SHA256_LEN];

	hmac_key_init(&pads, key, key_len);
	hmac_compute(&pads, data, len, mac);
	memcpy(out, mac, ZLRS_TAG_LEN);
	wipe(mac, sizeof(mac));
	wipe(&pads, sizeof(pads));
}

void zlrs_kdf(const uint8_t *key, size_t key_len, const char *label, const uint8_t *ctx,
	      size_t ctx_len, uint8_t *out, size_t out_len)
{
	struct hmac_key pads;
	uint8_t salt[SHA256_LEN] = {0};
	uint8_t prk[SHA256_LEN];
	uint8_t t[SHA256_LEN];
	uint8_t msg[SHA256_LEN + INFO_MAX + 1U];
	const size_t label_len = strlen(label);
	size_t copied = 0U;
	size_t previous = 0U;

	if (label_len > INFO_MAX || ctx_len > INFO_MAX - label_len ||
	    (ctx_len > 0U && ctx == NULL) || out_len > 255U * SHA256_LEN) {
		memset(out, 0, out_len);
		return;
	}
	const size_t info_len = label_len + ctx_len;

	/* Empty salt is equivalent to 32 zero bytes for HMAC-SHA256. */
	hmac_key_init(&pads, salt, sizeof(salt));
	hmac_compute(&pads, key, key_len, prk);
	hmac_key_init(&pads, prk, sizeof(prk));
	for (uint16_t counter = 1U; copied < out_len; counter++) {
		if (previous > 0U) {
			memcpy(msg, t, previous);
		}
		memcpy(msg + previous, label, label_len);
		if (ctx_len > 0U) {
			memcpy(msg + previous + label_len, ctx, ctx_len);
		}
		msg[previous + info_len] = (uint8_t)counter;
		hmac_compute(&pads, msg, previous + info_len + 1U, t);
		const size_t remaining = out_len - copied;
		const size_t take = remaining < sizeof(t) ? remaining : sizeof(t);

		memcpy(out + copied, t, take);
		copied += take;
		previous = sizeof(t);
	}
	wipe(&pads, sizeof(pads));
	wipe(prk, sizeof(prk));
	wipe(t, sizeof(t));
	wipe(msg, sizeof(msg));
}

void zlrs_pbkdf2_sha256(const char *phrase, const uint8_t *salt, size_t salt_len,
			uint32_t iterations, uint8_t out[ZLRS_KEY_LEN])
{
	struct hmac_key pads;
	uint8_t block[ZLRS_KEY_LEN + 4U];
	uint8_t u[SHA256_LEN];
	uint8_t t[SHA256_LEN];

	if (iterations == 0U || salt_len > ZLRS_KEY_LEN) {
		memset(out, 0, ZLRS_KEY_LEN);
		return;
	}
	if (salt_len > 0U) {
		memcpy(block, salt, salt_len);
	}
	block[salt_len] = 0U;
	block[salt_len + 1U] = 0U;
	block[salt_len + 2U] = 0U;
	block[salt_len + 3U] = 1U;
	hmac_key_init(&pads, (const uint8_t *)phrase, strlen(phrase));
	hmac_compute(&pads, block, salt_len + 4U, u);
	memcpy(t, u, sizeof(t));
	for (uint32_t i = 1U; i < iterations; i++) {
		hmac_compute(&pads, u, sizeof(u), u);
		for (size_t j = 0U; j < sizeof(t); j++) {
			t[j] ^= u[j];
		}
	}
	memcpy(out, t, ZLRS_KEY_LEN);
	wipe(&pads, sizeof(pads));
	wipe(block, sizeof(block));
	wipe(u, sizeof(u));
	wipe(t, sizeof(t));
}

void zlrs_root_key_from_phrase(const char *phrase, uint8_t out[ZLRS_KEY_LEN])
{
	static const char salt[] = ZLRS_KDF_V1_SALT;

	zlrs_pbkdf2_sha256(phrase, (const uint8_t *)salt, sizeof(salt) - 1U, ZLRS_KDF_V1_ITERATIONS,
			   out);
}

bool zlrs_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
	uint8_t diff = 0U;

	for (size_t i = 0U; i < len; i++) {
		diff |= (uint8_t)(a[i] ^ b[i]);
	}
	return diff == 0U;
}

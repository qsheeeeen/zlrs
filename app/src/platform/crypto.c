/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Protocol crypto through the Zephyr SHA-256 driver. HMAC, HKDF and PBKDF2 keep
 * the existing protocol derivation rules; the driver owns the hardware and padding.
 */

#include "platform/caps.h"

#if ZLRS_HAS_SHA256

#include <zephyr/crypto/crypto.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <string.h>
#include <stdalign.h>

#include "zlrs/proto/crypto.h"

LOG_MODULE_REGISTER(zlrs_crypto, CONFIG_APP_LOG_LEVEL);

#define SHA256_LEN       32U
#define SHA256_BLOCK_LEN 64U
#define INFO_MAX         96U

static const struct device *const hash_dev = DEVICE_DT_GET(DT_NODELABEL(sha256));

/* Serialize complete sessions and preserve priority inheritance between the shell
 * and RF owner when a lower-priority caller is interrupted between hash chunks. */
K_MUTEX_DEFINE(hash_mutex);

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

/* Feed both spans in order without copying. The driver retains partial words and
 * SHA state across update/compute; freeing releases it for other driver/ROM users. */
static void hash_two(const uint8_t *first, size_t first_len, const uint8_t *second,
		     size_t second_len, uint8_t out[SHA256_LEN])
{
	struct hash_ctx ctx = {.flags = CAP_SEPARATE_IO_BUFS | CAP_SYNC_OPS};
	struct hash_pkt pkt = {.in_buf = first, .in_len = first_len};
	int rc;

	if (!device_is_ready(hash_dev)) {
		LOG_ERR("SHA-256 device unavailable");
		k_panic();
	}
	if (k_mutex_lock(&hash_mutex, K_FOREVER) != 0) {
		k_panic();
	}
	const int64_t lock_started = k_uptime_get();

	do {
		rc = hash_begin_session(hash_dev, &ctx, CRYPTO_HASH_ALGO_SHA256);
		if (rc != -EBUSY || k_uptime_get() - lock_started >= 100) {
			break;
		}
		k_usleep(50);
	} while (true);
	if (rc == 0) {
		rc = hash_update(&ctx, &pkt);
		if (rc == 0) {
			pkt.in_buf = second;
			pkt.in_len = second_len;
			pkt.out_buf = out;
			rc = hash_compute(&ctx, &pkt);
		}
		const int free_rc = hash_free_session(hash_dev, &ctx);

		if (rc == 0) {
			rc = free_rc;
		}
	}
	k_mutex_unlock(&hash_mutex);
	if (rc != 0) {
		/* Never turn a failed driver operation into an authentication tag. */
		LOG_ERR("SHA-256 driver failed: %d", rc);
		k_panic();
	}
}

static void hmac_key_init(struct hmac_key *pads, const uint8_t *key, size_t key_len)
{
	uint8_t digest[SHA256_LEN];

	if (key_len > SHA256_BLOCK_LEN) {
		hash_two(key, key_len, NULL, 0U, digest);
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

	hash_two(pads->inner, sizeof(pads->inner), data, len, inner);
	hash_two(pads->outer, sizeof(pads->outer), inner, sizeof(inner), out);
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
		const size_t take = MIN(out_len - copied, sizeof(t));

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

	BUILD_ASSERT(ZLRS_KEY_LEN == SHA256_LEN);
	if (iterations == 0U || salt_len > ZLRS_KEY_LEN) {
		memset(out, 0, ZLRS_KEY_LEN);
		return;
	}
	if (salt_len > 0U) {
		memcpy(block, salt, salt_len);
	}
	sys_put_be32(1U, block + salt_len);
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

#endif /* ZLRS_HAS_SHA256 */

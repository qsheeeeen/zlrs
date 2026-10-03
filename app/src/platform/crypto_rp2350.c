/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RP2350 protocol crypto backend. The Pico SDK feeds and pads SHA-256 blocks; HMAC,
 * HKDF and PBKDF2 keep exactly the existing protocol derivation rules. CPU writes feed
 * the accelerator without IRQ masking or DMA channel allocation. The SDK's Zephyr
 * build exposes update/padding, so start/result follow its public hardware API.
 */

#include <pico/bootrom/lock.h>
#include <pico/sha256.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <string.h>
#include <stdalign.h>

#include "zlrs/proto/crypto.h"

LOG_MODULE_REGISTER(crypto_rp2350, CONFIG_APP_LOG_LEVEL);

#define SHA256_LEN       32U
#define SHA256_BLOCK_LEN 64U
#define INFO_MAX         96U

/* A mutex lets an interrupted shell check yield to the radio owner safely. The SDK
 * boot lock also coordinates with users outside this backend, including ROM calls. */
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

/* Each call owns the accelerator through its final digest. Input chunks can be
 * unaligned or incomplete blocks; pico_sha256_update retains the trailing bytes. */
static void hash_two(const uint8_t *first, size_t first_len, const uint8_t *second,
		     size_t second_len, uint8_t out[SHA256_LEN])
{
	pico_sha256_state_t state = {0};
	int64_t lock_started;

	if (k_mutex_lock(&hash_mutex, K_FOREVER) != 0) {
		k_panic();
	}
	lock_started = k_uptime_get();
	while (!bootrom_try_acquire_lock(BOOTROM_LOCK_SHA_256)) {
		if (k_uptime_get() - lock_started >= 100) {
			/* Do not turn hardware failure into a zero authentication tag. The
			 * watchdog and MCUboot can recover a failed test image autonomously. */
			LOG_ERR("SHA-256 hardware lock timed out");
			k_panic();
		}
		k_usleep(50);
	}
	state.locked = true;
	sha256_err_not_ready_clear();
	sha256_set_bswap(true);
	sha256_start();
	pico_sha256_update(&state, first, first_len);
	pico_sha256_update(&state, second, second_len);
	pico_sha256_write_padding(&state);
	sha256_wait_valid_blocking();
	if (sha256_err_not_ready()) {
		LOG_ERR("SHA-256 accelerator rejected input");
		k_panic();
	}
	for (size_t i = 0U; i < SHA256_LEN / 4U; i++) {
		sys_put_be32(sha256_hw->sum[i], out + i * 4U);
	}
	bootrom_release_lock(BOOTROM_LOCK_SHA_256);
	k_mutex_unlock(&hash_mutex);
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

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_SHELL)
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include "platform/crypto.h"

/* Large diagnostic inputs belong outside the 2 KiB shell stack. */
static alignas(uint32_t) uint8_t crypto_test_packet[507];
static alignas(uint32_t) uint8_t crypto_test_shifted[510];
K_MUTEX_DEFINE(crypto_test_mutex);

/* Generated independently with Python hashlib/hmac. Complete digests verify padding,
 * chunk boundaries and long-key reduction before the truncated wire tag is checked. */
static const struct {
	uint16_t len;
	const char *want;
} sha_vectors[] = {
	{0U, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
	{3U, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
	{55U, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
	{56U, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
	{63U, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
	{64U, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
	{65U, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
	{255U, "3f8591112c6bbe5c963965954e293108b7208ed2af893e500d859368c654eabe"},
};

static const struct {
	uint16_t key_len;
	uint16_t len;
	const char *want;
} hmac_vectors[] = {
	{32U, 0U, "d38b42096d80f45f826b44a9d5607de72496a415d3f4a1a8c88e3bb9da8dc1cb"},
	{32U, 1U, "e711546e3faad4c7c4aa756bc26cad6abea8241984a0f6b0839c70ca61c4ef88"},
	{32U, 55U, "b478e4cbd63871759702a8a4c9828359869bc9e20d3df429ecd08f5a5d3d9340"},
	{32U, 56U, "e5d1f65e9e9359d05c577b6890044f08c9a1f7969b683f1237ef07db70e5f862"},
	{32U, 63U, "d37a8dadb82b15310342ceabf0de8cb8991ee9bd55dd3e4813e952081cb24bf1"},
	{32U, 64U, "173206781c3b828a0dc2a716fe0ddb5e6e56ec171170952ff6b3f4de44fa18d7"},
	{32U, 65U, "22084084cc171f63dfdd6ca4bcb0c29be8d4ff1cc6b1d0d21e10e2a2a0bfce9c"},
	{32U, 251U, "9f3c65f884ad74347badf93743fbbb6acfe8d1ce807e8217a5fc2d6838d7fcf9"},
	{131U, 200U, "c4b86c4234bc84b889b659e1d14fb2f63a633db5f37618306da3b036564d9ee9"},
};

static int check_vector(const struct shell *sh, unsigned int index, const uint8_t *out, size_t len,
			const char *want)
{
	uint8_t expected[42];

	if (hex2bin(want, len * 2U, expected, sizeof(expected)) != len ||
	    !zlrs_ct_equal(out, expected, len)) {
		shell_error(sh, "crypto vector %u FAIL", index);
		return 1;
	}
	return 0;
}

int zlrs_crypto_self_test(const struct shell *sh)
{
	static uint8_t data_storage[256];
	static uint8_t key_storage[132];
	uint8_t *data = data_storage + 1U;
	uint8_t *key = key_storage + 1U;
	uint8_t ctx[16];
	uint8_t out[42];
	uint8_t tag[ZLRS_TAG_LEN];
	struct hmac_key pads;
	unsigned int count = 0U;
	int failed = 0;
	static char phrase[101];
	uint8_t *const packet_data = crypto_test_packet;
	uint8_t *const shifted_data = crypto_test_shifted;

	if (k_mutex_lock(&crypto_test_mutex, K_NO_WAIT) != 0) {
		return -EBUSY;
	}

	for (size_t i = 0U; i < 255U; i++) {
		data[i] = (uint8_t)i;
	}
	for (size_t i = 0U; i < 131U; i++) {
		key[i] = (uint8_t)i;
	}
	for (size_t i = 0U; i < ARRAY_SIZE(sha_vectors); i++) {
		const size_t len = sha_vectors[i].len;
		const size_t split = len / 2U;
		const uint8_t *input = len == 3U ? (const uint8_t *)"abc" : data;

		hash_two(input, split, input + split, len - split, out);
		failed += check_vector(sh, ++count, out, SHA256_LEN, sha_vectors[i].want);
	}
	for (size_t i = 0U; i < ARRAY_SIZE(hmac_vectors); i++) {
		hmac_key_init(&pads, key, hmac_vectors[i].key_len);
		hmac_compute(&pads, data, hmac_vectors[i].len, out);
		failed += check_vector(sh, ++count, out, SHA256_LEN, hmac_vectors[i].want);
		zlrs_tag(key, hmac_vectors[i].key_len, data, hmac_vectors[i].len, tag);
		failed += check_vector(sh, ++count, tag, sizeof(tag), hmac_vectors[i].want);
	}
	for (size_t i = 0U; i < sizeof(ctx); i++) {
		ctx[i] = (uint8_t)(0x40U + i);
	}
	zlrs_kdf(key, 32U, "zlrs/pair", ctx, sizeof(ctx), out, sizeof(out));
	failed += check_vector(sh, ++count, out, sizeof(out),
			       "1c6418d29272536de8c6d1492587bc52fb374109a5c313e02badd158066e58b7d"
			       "8cd11057bdddc8ec2e4");
	zlrs_pbkdf2_sha256("password", (const uint8_t *)"salt", 4U, 1U, out);
	failed += check_vector(sh, ++count, out, SHA256_LEN,
			       "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
	zlrs_pbkdf2_sha256("password", (const uint8_t *)"salt", 4U, 2U, out);
	failed += check_vector(sh, ++count, out, SHA256_LEN,
			       "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
	memset(phrase, 'P', sizeof(phrase) - 1U);
	phrase[sizeof(phrase) - 1U] = '\0';
	zlrs_pbkdf2_sha256(phrase, key, 32U, 5U, out);
	failed += check_vector(sh, ++count, out, SHA256_LEN,
			       "ba71e3c0299a60aa5620773114321003bf0a2044b09408313015054a177f510e");

	/* The maximum P1 frame hashes 507 bytes. Check every word alignment against
	 * an independent digest before comparing the SDK's word/byte input paths. */
	for (size_t i = 0U; i < sizeof(crypto_test_packet); i++) {
		packet_data[i] = (uint8_t)i;
	}
	hmac_key_init(&pads, key, 32U);
	for (size_t offset = 0U; offset < 4U; offset++) {
		memcpy(shifted_data + offset, packet_data, sizeof(crypto_test_packet));
		hmac_compute(&pads, shifted_data + offset, sizeof(crypto_test_packet), out);
		failed += check_vector(
			sh, ++count, out, SHA256_LEN,
			"5521f6dafa14f363fe04aba4900cf295b95450e82c0462806ba00988ec5e8590");
		zlrs_tag(key, 32U, shifted_data + offset, sizeof(crypto_test_packet), tag);
		failed += check_vector(
			sh, ++count, tag, sizeof(tag),
			"5521f6dafa14f363fe04aba4900cf295b95450e82c0462806ba00988ec5e8590");
	}

	shell_print(sh, "crypto RP2350 SHA-256: %u/%u vectors passed", count - failed, count);
	const uint16_t lengths[] = {251U, 507U};

	for (size_t n = 0U; n < ARRAY_SIZE(lengths); n++) {
		for (size_t offset = 0U; offset < 2U; offset++) {
			uint32_t elapsed = 0U;
			uint32_t worst = 0U;

			memcpy(shifted_data + offset, packet_data, lengths[n]);
			for (unsigned int i = 0U; i < 64U; i++) {
				const uint32_t start = k_cycle_get_32();

				zlrs_tag(key, 32U, shifted_data + offset, lengths[n], tag);
				const uint32_t cycles = k_cycle_get_32() - start;

				elapsed += cycles;
				worst = MAX(worst, cycles);
			}
			shell_print(
				sh,
				"crypto %u-byte tag offset=%u: average=%u us max=%u us (64 calls)",
				lengths[n], (unsigned int)offset, k_cyc_to_us_ceil32(elapsed) / 64U,
				k_cyc_to_us_ceil32(worst));
		}
	}
	wipe(&pads, sizeof(pads));
	k_mutex_unlock(&crypto_test_mutex);
	return failed == 0 ? 0 : -EIO;
}
#endif

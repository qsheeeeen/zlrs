/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#include "zlrs/proto/crypto.h"
#include "test_support.h"
#include "crypto_vectors.h"

#define SHA256_LEN 32U

static alignas(uint32_t) uint8_t data[511];
static alignas(uint32_t) uint8_t key[132];

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	for (size_t i = 0U; i < sizeof(data); i++) {
		data[i] = (uint8_t)i;
	}
	for (size_t i = 0U; i < sizeof(key); i++) {
		key[i] = (uint8_t)i;
	}
}

ZTEST(proto_crypto, test_authentication_tags)
{
	uint8_t tag[ZLRS_TAG_LEN];
	alignas(uint32_t) uint8_t shifted[510];

	for (size_t i = 0U; i < ARRAY_SIZE(hmac_vectors); i++) {
		/* Unaligned key and message exercise the production backend's byte path. */
		for (size_t n = 0U; n < 131U; n++) {
			key[n + 1U] = (uint8_t)n;
		}
		for (size_t n = 0U; n < 255U; n++) {
			data[n + 1U] = (uint8_t)n;
		}
		zlrs_tag(key + 1U, hmac_vectors[i].key_len, data + 1U, hmac_vectors[i].len,
			 tag);
		zlrs_test_check_hex(tag, sizeof(tag), hmac_vectors[i].want);
	}
	for (size_t i = 0U; i < 32U; i++) {
		key[i] = (uint8_t)i;
	}
	for (size_t i = 0U; i < 507U; i++) {
		data[i] = (uint8_t)i;
	}
	for (size_t offset = 0U; offset < 4U; offset++) {
		memcpy(shifted + offset, data, 507U);
		zlrs_tag(key, 32U, shifted + offset, 507U, tag);
		zlrs_test_check_hex(tag, sizeof(tag), "5521f6da");
	}
	printk("HMAC tags: %u vectors passed, including 507 B at offsets 0..3\n",
	       (unsigned int)ARRAY_SIZE(hmac_vectors) + 4U);
}

ZTEST(proto_crypto, test_key_derivation)
{
	uint8_t ctx[16];
	uint8_t out[42];
	char phrase[101];
	uint8_t ikm[22];
	const uint8_t info[] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9};

	memset(ikm, 0x0b, sizeof(ikm));
	zlrs_kdf(ikm, sizeof(ikm), "", info, sizeof(info), out, SHA256_LEN);
	zlrs_test_check_hex(out, SHA256_LEN,
		  "abbafb13f5c1bc489d4203135817956dd521b39e3bd61d1cc85cef884d1f8e2e");
	for (size_t i = 0U; i < sizeof(ctx); i++) {
		ctx[i] = (uint8_t)(0x40U + i);
	}
	zlrs_kdf(key, 32U, "zlrs/pair", ctx, sizeof(ctx), out, sizeof(out));
	zlrs_test_check_hex(out, sizeof(out),
		  "1c6418d29272536de8c6d1492587bc52fb374109a5c313e02badd158066e58b7d"
		  "8cd11057bdddc8ec2e4");
	zlrs_pbkdf2_sha256("password", (const uint8_t *)"salt", 4U, 1U, out);
	zlrs_test_check_hex(out, SHA256_LEN,
		  "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
	zlrs_pbkdf2_sha256("password", (const uint8_t *)"salt", 4U, 2U, out);
	zlrs_test_check_hex(out, SHA256_LEN,
		  "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
	memset(phrase, 'P', sizeof(phrase) - 1U);
	phrase[sizeof(phrase) - 1U] = '\0';
	zlrs_pbkdf2_sha256(phrase, key, 32U, 5U, out);
	zlrs_test_check_hex(out, SHA256_LEN,
		  "ba71e3c0299a60aa5620773114321003bf0a2044b09408313015054a177f510e");
}

ZTEST(proto_crypto, test_production_passphrase)
{
	uint8_t out[SHA256_LEN];
	const int64_t started = k_uptime_get();

	zlrs_root_key_from_phrase("ZLRS hardware KDF timing", out);
	printk("PBKDF2 %u rounds: %lld ms\n", ZLRS_KDF_V1_ITERATIONS,
	       (long long)(k_uptime_get() - started));
	zlrs_test_check_hex(out, sizeof(out),
		  "c435dcd88e863c3e9d0c00458a92cffcda5ad2bb412d4cc5a55066f3b863bb0e");
}

ZTEST(proto_crypto, test_tag_timing)
{
	const uint16_t lengths[] = {251U, 507U};
	alignas(uint32_t) uint8_t shifted[510];
	uint8_t tag[ZLRS_TAG_LEN];

	for (size_t n = 0U; n < ARRAY_SIZE(lengths); n++) {
		for (size_t offset = 0U; offset < 2U; offset++) {
			uint32_t elapsed = 0U;
			uint32_t worst = 0U;

			memcpy(shifted + offset, data, lengths[n]);
			for (unsigned int i = 0U; i < 64U; i++) {
				const uint32_t start = k_cycle_get_32();

				zlrs_tag(key, 32U, shifted + offset, lengths[n], tag);
				const uint32_t cycles = k_cycle_get_32() - start;

				elapsed += cycles;
				worst = MAX(worst, cycles);
			}
			zlrs_test_check_hex(tag, sizeof(tag),
					    lengths[n] == 251U ? "9f3c65f8" : "5521f6da");
			printk("crypto %u-byte tag offset=%u: average=%u us max=%u us (64 calls)\n",
			       lengths[n], (unsigned int)offset,
			       k_cyc_to_us_ceil32(elapsed) / 64U, k_cyc_to_us_ceil32(worst));
		}
	}
}

ZTEST_SUITE(proto_crypto, NULL, NULL, before, NULL, NULL);

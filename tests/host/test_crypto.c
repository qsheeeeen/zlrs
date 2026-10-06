/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test.h"
#include <string.h>

#include "zlrs/proto/crypto.h"

/* ---- KDF and per-frame tag ----------------------------------------------- */

static void test_kdf(void)
{
	uint8_t key[ZLRS_KEY_LEN];
	uint8_t a[ZLRS_SYNC_WORD_LEN];
	uint8_t b[ZLRS_SYNC_WORD_LEN];
	uint8_t tag[ZLRS_TAG_LEN];
	uint8_t ctx[4] = {1U, 2U, 3U, 4U};

	printf("kdf\n");

	memset(key, 0x42, sizeof(key));

	/* Fixed vectors computed independently with Python hashlib/hmac (empty HKDF salt) */
	zlrs_kdf(key, sizeof(key), "zlrs/sync/control", NULL, 0U, a, sizeof(a));
	CHECK(zlrs_test_hex_eq(a, sizeof(a), "c7f3da8b"));
	zlrs_kdf(key, sizeof(key), "zlrs/session", ctx, sizeof(ctx), b, sizeof(b));
	CHECK(zlrs_test_hex_eq(b, sizeof(b), "89e0e3e0"));

	/* Repeatable for the same input; changing label/context gives domain separation. */
	zlrs_kdf(key, sizeof(key), "zlrs/sync/control", NULL, 0U, b, sizeof(b));
	CHECK(memcmp(a, b, sizeof(a)) == 0);
	zlrs_kdf(key, sizeof(key), "zlrs/sync/data", NULL, 0U, b, sizeof(b));
	CHECK(memcmp(a, b, sizeof(a)) != 0);
	zlrs_kdf(key, sizeof(key), "zlrs/session", ctx, sizeof(ctx), a, sizeof(a));
	zlrs_kdf(key, sizeof(key), "zlrs/session", NULL, 0U, b, sizeof(b));
	CHECK(memcmp(a, b, sizeof(a)) != 0);

	/* Per-frame tag: HMAC-SHA256 truncated to 4 bytes */
	zlrs_tag(key, sizeof(key), ctx, sizeof(ctx), tag);
	CHECK(zlrs_test_hex_eq(tag, sizeof(tag), "e1c2dd24"));
}

static void test_phrase_vector(void)
{
	uint8_t key[ZLRS_KEY_LEN];

	printf("crypto: phrase derivation\n");

	/* Vector from Python hashlib.pbkdf2_hmac('sha256', phrase, b'zlrs/kdf/v1', 1e5, 32) */
	zlrs_root_key_from_phrase("correct horse battery staple", key);
	CHECK(zlrs_test_hex_eq(key, sizeof(key),
			       "039690ebe7c3c9f237d80555d083e7a48f781b7a537156a6d0e501f49d60f726"));

	zlrs_root_key_from_phrase("hello", key);
	CHECK(zlrs_test_hex_eq(key, sizeof(key),
			       "7e7363b218016d1f319a6389fa20aaf53d2fe21056ffe30566cc3b874e6073f8"));

	/* The same phrase must derive the same key every time (tool and device each do it once) */
	zlrs_root_key_from_phrase("hello", key);
	CHECK(zlrs_test_hex_eq(key, sizeof(key),
			       "7e7363b218016d1f319a6389fa20aaf53d2fe21056ffe30566cc3b874e6073f8"));
}

void test_crypto(void)
{
	test_kdf();
	test_phrase_vector();
}

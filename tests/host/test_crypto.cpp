/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hex.hpp"
#include <cstdint>
#include <cstdio>
#include <string.h>

#include "zlrs/proto/crypto.h"

/* ---- KDF and per-frame tag ----------------------------------------------- */

TEST(Crypto, KdfDomainSeparation)
{
	uint8_t key[ZLRS_KEY_LEN];
	uint8_t a[ZLRS_SYNC_WORD_LEN];
	uint8_t b[ZLRS_SYNC_WORD_LEN];
	uint8_t tag[ZLRS_TAG_LEN];
	uint8_t ctx[4] = {1U, 2U, 3U, 4U};

	printf("kdf\n");

	memset(key, 0x42, sizeof(key));

	/* Fixed vectors computed independently with Python hashlib/hmac (empty HKDF salt) */
	zlrs_kdf(key, sizeof(key), "zlrs/sync/data", nullptr, 0U, a, sizeof(a));
	EXPECT_TRUE(HexEquals(a, sizeof(a), "0b5e02f8"));
	zlrs_kdf(key, sizeof(key), "zlrs/session", ctx, sizeof(ctx), b, sizeof(b));
	EXPECT_TRUE(HexEquals(b, sizeof(b), "89e0e3e0"));

	/* Repeatable for the same input; changing label/context gives domain separation. */
	zlrs_kdf(key, sizeof(key), "zlrs/sync/data", nullptr, 0U, b, sizeof(b));
	EXPECT_EQ(memcmp(a, b, sizeof(a)), 0);
	zlrs_kdf(key, sizeof(key), "zlrs/auth", nullptr, 0U, b, sizeof(b));
	EXPECT_NE(memcmp(a, b, sizeof(a)), 0);
	zlrs_kdf(key, sizeof(key), "zlrs/session", ctx, sizeof(ctx), a, sizeof(a));
	zlrs_kdf(key, sizeof(key), "zlrs/session", nullptr, 0U, b, sizeof(b));
	EXPECT_NE(memcmp(a, b, sizeof(a)), 0);

	/* Per-frame tag: HMAC-SHA256 truncated to 4 bytes */
	zlrs_tag(key, sizeof(key), ctx, sizeof(ctx), tag);
	EXPECT_TRUE(HexEquals(tag, sizeof(tag), "e1c2dd24"));
}

TEST(Crypto, PhraseVectors)
{
	uint8_t key[ZLRS_KEY_LEN];

	printf("crypto: phrase derivation\n");

	/* Vector from Python hashlib.pbkdf2_hmac('sha256', phrase, b'zlrs/kdf/v1', 1e5, 32) */
	zlrs_root_key_from_phrase("correct horse battery staple", key);
	EXPECT_TRUE(HexEquals(key, sizeof(key),
			      "039690ebe7c3c9f237d80555d083e7a48f781b7a537156a6d0e501f49d60f726"));

	zlrs_root_key_from_phrase("hello", key);
	EXPECT_TRUE(HexEquals(key, sizeof(key),
			      "7e7363b218016d1f319a6389fa20aaf53d2fe21056ffe30566cc3b874e6073f8"));

	/* The same phrase must derive the same key every time (tool and device each do it once) */
	zlrs_root_key_from_phrase("hello", key);
	EXPECT_TRUE(HexEquals(key, sizeof(key),
			      "7e7363b218016d1f319a6389fa20aaf53d2fe21056ffe30566cc3b874e6073f8"));
}

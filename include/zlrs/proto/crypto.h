/*
 * Protocol authentication and key derivation, implemented once in core/proto/crypto.c.
 * SHA-256 device access lives in app/src/platform/sha256.c; host tests replace only
 * that hash operation. See doc/09-security.md for the key tree and wire rules.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zlrs/proto/limits.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KDF(key, label[, ctx]): HKDF-SHA256 with an empty salt and info = label || ctx.
 * The derived values map one to one onto the derivation tree in doc/09-security.md 3.
 */
void zlrs_kdf(const uint8_t *key, size_t key_len, const char *label, const uint8_t *ctx,
	      size_t ctx_len, uint8_t *out, size_t out_len);

/* Per-frame tag: HMAC-SHA256 truncated to ZLRS_TAG_LEN bytes */
void zlrs_tag(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
	      uint8_t out[ZLRS_TAG_LEN]);

/*
 * Passphrase -> root key: PBKDF2-HMAC-SHA256.
 * The salt carries the KDF version (doc/09-security.md 8) and the caller passes the iteration
 * count; the derivation rule is fixed on the device and the management tool cannot change it.
 */
void zlrs_pbkdf2_sha256(const char *phrase, const uint8_t *salt, size_t salt_len,
			uint32_t iterations, uint8_t out[ZLRS_KEY_LEN]);

/* Same rule with the release salt and iteration count, used by management mode */
#define ZLRS_KDF_V1_SALT       "zlrs/kdf/v1"
#define ZLRS_KDF_V1_ITERATIONS 100000U

void zlrs_root_key_from_phrase(const char *phrase, uint8_t out[ZLRS_KEY_LEN]);

/* Constant-time comparison, to avoid timing side channels */
bool zlrs_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);

#ifdef __cplusplus
}
#endif

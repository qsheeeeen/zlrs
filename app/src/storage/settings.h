/*
 * Persistence: root key, device id and pairing record live in NVS (doc/09-security.md 2).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zlrs/proto/limits.h"

int zlrs_storage_settings_init(void);

/*
 * The settings that live in NVS. One entry per record: the table in settings.c is the only place
 * that knows an NVS id and a size, and everything else — the application, the management commands —
 * works with these names and the generic calls below.
 */
enum zlrs_setting {
	ZLRS_SETTING_ROOT_KEY, /* ZLRS_KEY_LEN bytes */
	ZLRS_SETTING_DEV_ID,   /* ZLRS_DEV_ID_LEN bytes */
	ZLRS_SETTING_PAIRING,  /* struct zlrs_storage_settings_pairing_t */
	ZLRS_SETTING_FAILSAFE, /* ZLRS_FAILSAFE_LEN bytes */
	ZLRS_SETTING_BAUD,     /* uint32_t */
	ZLRS_SETTING_REGION,   /* uint8_t */
	ZLRS_SETTING_POWER,    /* int8_t */
	ZLRS_SETTING_ROLE,     /* uint8_t */
	ZLRS_SETTING_PROFILE,  /* uint8_t */
	ZLRS_SETTING_MODE,     /* uint8_t: ZLRS_MODE_* */
	ZLRS_SETTING_COUNT,
};

/*
 * Pairing record: peer identity plus the two pairing nonces (see doc/09-security.md 2, 3).
 * One pairing, one record; there is no other shape.
 */
typedef struct {
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	uint8_t pair_salt[ZLRS_PAIR_SALT_LEN];
} zlrs_storage_settings_pairing_t;

/* Bytes one setting occupies; 0 for an id that is not a setting */
size_t zlrs_storage_settings_size(enum zlrs_setting id);

/*
 * Read and write one record. `len` has to be the size the setting has, so a caller cannot read a
 * region into a baud word: a mismatch is -EINVAL, an unsupported id too.
 * Load returns the bytes read (negative when the record is missing or the read failed).
 */
int zlrs_storage_settings_load(enum zlrs_setting id, void *out, size_t len);
int zlrs_storage_settings_save(enum zlrs_setting id, const void *value, size_t len);

/* Erase one record; a record that is not there already counts as erased */
int zlrs_storage_settings_erase(enum zlrs_setting id);

/* Factory reset: every record the table knows (keys, pairing, failsafe, baud, region, role, ...) */
int zlrs_storage_settings_erase_all(void);

/* A device id is an identity only when it is not all zero (a failed or lazy entropy read) */
bool zlrs_storage_settings_dev_id_valid(const uint8_t id[ZLRS_DEV_ID_LEN]);

/*
 * A fresh device id from the TRNG, or false when that failed (all-zero read, no entropy device).
 * The caller decides to persist it: a new id is a new identity, so a pairing record built on the
 * old one goes with it.
 *
 * The entropy device is read directly on purpose: sys_rand32_get() goes through the CSPRNG, which
 * is seeded lazily and was observed handing back zeros during boot.
 */
bool zlrs_storage_settings_new_dev_id(uint8_t out[ZLRS_DEV_ID_LEN]);

/* Parse 64 hex characters into a root key, returns 0 on success */
int zlrs_storage_settings_parse_hex_key(const char *hex, uint8_t out[ZLRS_KEY_LEN]);

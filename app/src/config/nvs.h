/*
 * Private configuration storage: NVS layout, identity generation and record access.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config/config.h"
#include "zlrs/proto/limits.h"

int zlrs_storage_settings_init(void);

/*
 * Pairing record: peer identity plus the two pairing nonces (see doc/09-security.md 2, 3).
 * One pairing, one record; there is no other shape.
 */
typedef struct {
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	uint8_t pair_salt[ZLRS_PAIR_SALT_LEN];
} zlrs_storage_settings_pairing_t;

/* NVS ids and sizes live only in nvs.c; callers use the configuration key directly. */
size_t zlrs_storage_settings_size(enum zlrs_config_key key);

/*
 * Read and write one record. `len` has to be the size the setting has, so a caller cannot read a
 * region into a baud word: a mismatch is -EINVAL, an unsupported key too.
 * Load returns the bytes read (negative when the record is missing or the read failed).
 */
int zlrs_storage_settings_load(enum zlrs_config_key key, void *out, size_t len);
int zlrs_storage_settings_save(enum zlrs_config_key key, const void *value, size_t len);

/* Erase one record; a record that is not there already counts as erased */
int zlrs_storage_settings_erase(enum zlrs_config_key key);

/* Factory reset: clear the settings partition and initialize its layout marker. */
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

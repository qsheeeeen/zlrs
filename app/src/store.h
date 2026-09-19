/*
 * Persistence: root key, device id and pairing record live in NVS (docs/design/10-security.md 2).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/config.h"

int zlrs_store_init(void);

/*
 * Pairing record: peer identity plus the two pairing nonces (see docs/design/10-security.md 2, 3).
 * One pairing, one record; there is no other shape.
 */
typedef struct {
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	uint8_t pair_salt[ZLRS_PAIR_SALT_LEN];
} zlrs_store_pairing_t;

/* Returns the bytes read; negative when the record is missing or the read failed */
int zlrs_store_load_root_key(uint8_t out[ZLRS_KEY_LEN]);
int zlrs_store_load_dev_id(uint8_t out[ZLRS_DEV_ID_LEN]);
int zlrs_store_load_pairing(zlrs_store_pairing_t *out);
int zlrs_store_load_failsafe(uint8_t out[ZLRS_CFG_FAILSAFE_LEN]);
int zlrs_store_load_baud(uint32_t *out);
int zlrs_store_load_region(uint8_t *out);
int zlrs_store_load_power(int8_t *out);

int zlrs_store_save_root_key(const uint8_t key[ZLRS_KEY_LEN]);
int zlrs_store_save_dev_id(const uint8_t id[ZLRS_DEV_ID_LEN]);
int zlrs_store_save_pairing(const zlrs_store_pairing_t *pairing);
int zlrs_store_save_failsafe(const uint8_t channels[ZLRS_CFG_FAILSAFE_LEN]);
int zlrs_store_save_baud(uint32_t baud);
int zlrs_store_save_region(uint8_t region);
int zlrs_store_save_power(int8_t dbm);

/* Factory reset: wipe every record (keys, pairing, failsafe, baud, region, power) */
int zlrs_store_erase_all(void);

/* Erase the pairing record only (device id and keys stay) */
int zlrs_store_erase_pairing(void);

/* Parse 64 hex characters into a root key, returns 0 on success */
int zlrs_store_parse_hex_key(const char *hex, uint8_t out[ZLRS_KEY_LEN]);

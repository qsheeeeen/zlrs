/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "zlrs/proto/frame.h"
#include "zlrs/region/region.h"

typedef struct {
	zlrs_data_band_t band;
	zlrs_region_id_t region;
	zlrs_role_t role;
	zlrs_profile_t profile;
	uint32_t data_baud;
} zlrs_persist_settings_t;

typedef struct {
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	uint8_t pair_salt[ZLRS_PAIR_SALT_LEN];
} zlrs_persist_pairing_t;

/* Main-thread API. Writes update saved values only after flash succeeds. */
int zlrs_persist_init(void);
const zlrs_persist_settings_t *zlrs_persist_settings(void);
/* Copy a saved value when present; otherwise return false and leave out unchanged. */
bool zlrs_persist_get_device_id(uint8_t out[ZLRS_DEV_ID_LEN]);
bool zlrs_persist_get_root_key(uint8_t out[ZLRS_KEY_LEN]);
bool zlrs_persist_get_pairing(zlrs_persist_pairing_t *out);
int zlrs_persist_save_settings(const zlrs_persist_settings_t *settings);
int zlrs_persist_set_root_key(const uint8_t key[ZLRS_KEY_LEN]);
int zlrs_persist_save_pairing(const zlrs_persist_pairing_t *pairing);
int zlrs_persist_new_identity(void);
int zlrs_persist_erase_pairing(void);
int zlrs_persist_factory_reset(void);

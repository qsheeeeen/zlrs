/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "zlrs/proto/frame.h"
#include "zlrs/region/region.h"

#define ZLRS_POWER_MIN_DBM (-10)
#define ZLRS_POWER_MAX_DBM 22

typedef struct {
	zlrs_data_band_t band;
	zlrs_region_id_t region;
	zlrs_role_t role;
	zlrs_profile_t profile;
	int8_t power_dbm;
	uint32_t data_baud;
} zlrs_persist_settings_t;

typedef struct {
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	uint8_t pair_salt[ZLRS_PAIR_SALT_LEN];
} zlrs_persist_pairing_t;

typedef struct {
	zlrs_persist_settings_t settings;
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t root_key[ZLRS_KEY_LEN];
	zlrs_persist_pairing_t pairing;
	bool identity_valid;
	bool configured;
	bool paired;
} zlrs_persist_state_t;

/* Main-thread API. Writes update the saved object only after flash succeeds. */
int zlrs_persist_init(void);
const zlrs_persist_state_t *zlrs_persist_state(void);
int zlrs_persist_save_settings(const zlrs_persist_settings_t *settings);
int zlrs_persist_set_root_key(const uint8_t key[ZLRS_KEY_LEN]);
int zlrs_persist_save_pairing(const zlrs_persist_pairing_t *pairing);
int zlrs_persist_new_identity(void);
int zlrs_persist_erase_pairing(void);
int zlrs_persist_factory_reset(void);

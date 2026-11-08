/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "proto/frame.h"
#include "region/region.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	zlrs_data_band_t band;
	zlrs_region_id_t region;
	zlrs_role_t role;
	zlrs_profile_t profile;
	uint32_t data_baud;
} zlrs_persist_settings_t;

typedef struct {
	zlrs_persist_settings_t settings;
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t root_key[ZLRS_KEY_LEN];
	zlrs_pairing_t pairing;
	bool identity_valid;
	bool configured;
	bool paired;
} zlrs_persist_state_t;

/* Main-thread API. Writes update saved values only after flash succeeds. */
int zlrs_persist_init(void);
/* Borrowed owner-thread view; flags indicate which credentials are usable.
 * Writes invalidate retained observations. Credential bytes never enter management status. */
const zlrs_persist_state_t *zlrs_persist_state(void);
int zlrs_persist_save_settings(const zlrs_persist_settings_t *settings);
int zlrs_persist_set_root_key(const uint8_t key[ZLRS_KEY_LEN]);
int zlrs_persist_save_pairing(const zlrs_pairing_t *pairing);
int zlrs_persist_new_identity(void);
int zlrs_persist_erase_pairing(void);
/* A destructive attempt retires RAM credentials even on failure. Failed storage stays
 * unavailable until init succeeds; a failed erase can leave records on flash. */
int zlrs_persist_factory_reset(void);

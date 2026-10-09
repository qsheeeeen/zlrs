/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "config/config.h"
#include "zlrs/proto/limits.h"

typedef struct {
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	uint8_t pair_salt[ZLRS_PAIR_SALT_LEN];
} zlrs_config_pairing_t;

/* RF boot dependency only; frontend status never contains these secrets. */
typedef struct {
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t root_key[ZLRS_KEY_LEN];
	zlrs_config_pairing_t pairing;
	bool identity_valid;
	bool configured;
	bool paired;
} zlrs_config_identity_t;

zlrs_config_identity_t zlrs_config_identity(void);
int zlrs_config_save_pairing(const zlrs_config_pairing_t *pairing);

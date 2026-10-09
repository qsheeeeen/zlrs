/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/zbus/zbus.h>

#define ZLRS_CONFIG_POWER_MIN_DBM (-10)
#define ZLRS_CONFIG_POWER_MAX_DBM 22

enum zlrs_config_key {
	ZLRS_CONFIG_MODE,
	ZLRS_CONFIG_DATA_BAUD,
	ZLRS_CONFIG_REGION,
	ZLRS_CONFIG_POWER,
	ZLRS_CONFIG_ROLE,
	ZLRS_CONFIG_PROFILE,
	ZLRS_CONFIG_CONTROL_RATE,
	ZLRS_CONFIG_MODEL,
	ZLRS_CONFIG_MODEL_MATCH,
	ZLRS_CONFIG_TELEMETRY,
	/* Identity records use typed operations rather than zlrs_config_set(). */
	ZLRS_CONFIG_ROOT_KEY,
	ZLRS_CONFIG_DEV_ID,
	ZLRS_CONFIG_PAIRING,
	ZLRS_CONFIG_KEY_COUNT,
};

typedef struct {
	uint32_t data_baud;
	uint32_t control_rate;
	uint8_t mode;
	uint8_t region;
	int8_t power_dbm;
	uint8_t role;
	uint8_t profile;
	uint8_t model;
	bool model_match;
	bool telemetry;
} zlrs_config_values_t;

typedef struct {
	zlrs_config_values_t saved;
	zlrs_config_values_t active;
	uint32_t revision;
	bool reboot_required;
} zlrs_config_state_t;

ZBUS_CHAN_DECLARE(zlrs_config_chan);

int zlrs_config_init(void);
zlrs_config_state_t zlrs_config_state(void);
/* A single validator and apply/persist path for all frontends. */
int zlrs_config_set(enum zlrs_config_key key, int32_t value, bool persist);
bool zlrs_config_profile_allowed(uint8_t profile);
int zlrs_config_set_root_key(const uint8_t *key);
int zlrs_config_new_identity(void);
int zlrs_config_erase_pairing(void);
int zlrs_config_factory_reset(void);
/* Retry a failed state publication without marking it delivered. */
void zlrs_config_poll(void);

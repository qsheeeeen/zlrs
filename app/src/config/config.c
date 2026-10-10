/* SPDX-License-Identifier: Apache-2.0 */
#include "config/config.h"
#include "config/identity.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>

#include "platform/caps.h"
#include "rf/rf.h"
#include "config/nvs.h"
#include "zlrs/mgmt/mgmt.h"
#include "zlrs/proto/frame.h"

ZBUS_CHAN_DEFINE(zlrs_config_chan, zlrs_config_state_t, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

static zlrs_config_state_t state;
static zlrs_config_identity_t identity;
static bool publication_pending;
static bool identity_reset_pending;

bool zlrs_config_profile_allowed(uint8_t profile)
{
	return profile == ZLRS_PROFILE_P1 ||
	       (profile == ZLRS_PROFILE_P0 && IS_ENABLED(CONFIG_ZLRS_APP_DEBUG));
}

static int assign(zlrs_config_values_t *values, enum zlrs_config_key key, int32_t value)
{
	switch (key) {
	case ZLRS_CONFIG_BAND:
		if (value != ZLRS_DATA_BAND_24 && value != ZLRS_DATA_BAND_SUBG) {
			return -EINVAL;
		}
		values->band = value;
		break;
	case ZLRS_CONFIG_DATA_BAUD:
		if (value < 9600 || value > 4000000) {
			return -EINVAL;
		}
		values->data_baud = value;
		break;
	case ZLRS_CONFIG_REGION:
		if (value < 0 || value > UINT8_MAX || zlrs_region_get(value) == NULL) {
			return -EINVAL;
		}
		values->region = value;
		break;
	case ZLRS_CONFIG_POWER:
		if (value < ZLRS_CONFIG_POWER_MIN_DBM || value > ZLRS_CONFIG_POWER_MAX_DBM) {
			return -EINVAL;
		}
		values->power_dbm = value;
		break;
	case ZLRS_CONFIG_ROLE:
		if (value != ZLRS_ROLE_TX && value != ZLRS_ROLE_RX) {
			return -EINVAL;
		}
		values->role = value;
		break;
	case ZLRS_CONFIG_PROFILE:
		if (value < 0 || value > UINT8_MAX || !zlrs_config_profile_allowed(value)) {
			return -EINVAL;
		}
		values->profile = value;
		break;
	default:
		return -EINVAL;
	}
	return zlrs_region_validate(zlrs_region_get(values->region), ZLRS_RF_DESIGN,
				    values->power_dbm) == 0
		       ? 0
		       : -EINVAL;
}

void zlrs_config_poll(void)
{
	if (publication_pending && zbus_chan_pub(&zlrs_config_chan, &state, K_NO_WAIT) == 0) {
		publication_pending = false;
	}
}

static void changed(void)
{
	state.revision++;
	state.reboot_required = identity_reset_pending || state.saved.band != state.active.band ||
				state.saved.region != state.active.region ||
				state.saved.power_dbm != state.active.power_dbm ||
				state.saved.role != state.active.role ||
				state.saved.profile != state.active.profile ||
				state.saved.data_baud != state.active.data_baud;
	publication_pending = true;
	zlrs_config_poll();
}

zlrs_config_state_t zlrs_config_state(void)
{
	return state;
}

zlrs_config_identity_t zlrs_config_identity(void)
{
	return identity;
}

static zlrs_config_values_t defaults(void)
{
	return (zlrs_config_values_t){
		.band = ZLRS_DATA_BAND_24,
		.region = ZLRS_DEFAULT_REGION,
		.power_dbm = CONFIG_ZLRS_APP_TX_POWER_DBM,
		.role = IS_ENABLED(CONFIG_ZLRS_APP_ROLE_TX) ? ZLRS_ROLE_TX : ZLRS_ROLE_RX,
		.profile = IS_ENABLED(CONFIG_ZLRS_APP_SECURITY_PROFILE_P0) ? ZLRS_PROFILE_P0
									   : ZLRS_PROFILE_P1,
	};
}

int zlrs_config_init(void)
{
	const int rc = zlrs_storage_settings_init();
	state.saved = defaults();
	/* Load the RF combination together, then validate it before any frontend sees it. */
	uint8_t region = state.saved.region;
	int8_t power = state.saved.power_dbm;
	(void)zlrs_storage_settings_load(ZLRS_CONFIG_REGION, &region, sizeof(region));
	(void)zlrs_storage_settings_load(ZLRS_CONFIG_POWER, &power, sizeof(power));
	if (power >= ZLRS_CONFIG_POWER_MIN_DBM && power <= ZLRS_CONFIG_POWER_MAX_DBM &&
	    zlrs_region_validate(zlrs_region_get(region), ZLRS_RF_DESIGN, power) == 0) {
		state.saved.region = region;
		state.saved.power_dbm = power;
	}
	for (enum zlrs_config_key key = ZLRS_CONFIG_BAND; key <= ZLRS_CONFIG_PROFILE; key++) {
		if (key == ZLRS_CONFIG_REGION || key == ZLRS_CONFIG_POWER) {
			continue;
		}
		uint32_t wide = 0U;
		uint8_t byte = 0U;
		const size_t size = zlrs_storage_settings_size(key);
		void *out = size == sizeof(wide) ? (void *)&wide : (void *)&byte;
		if (zlrs_storage_settings_load(key, out, size) == (int)size) {
			zlrs_config_values_t candidate = state.saved;
			if (assign(&candidate, key, size == sizeof(wide) ? wide : byte) == 0) {
				state.saved = candidate;
			}
		}
	}
	state.active = state.saved;
	identity.identity_valid =
		zlrs_storage_settings_load(ZLRS_CONFIG_DEV_ID, identity.dev_id,
					   sizeof(identity.dev_id)) == sizeof(identity.dev_id) &&
		zlrs_storage_settings_dev_id_valid(identity.dev_id);
	if (!identity.identity_valid) {
		identity.identity_valid = zlrs_storage_settings_new_dev_id(identity.dev_id);
		if (identity.identity_valid) {
			(void)zlrs_storage_settings_save(ZLRS_CONFIG_DEV_ID, identity.dev_id,
							 sizeof(identity.dev_id));
		}
	}
	identity.configured =
		zlrs_storage_settings_load(ZLRS_CONFIG_ROOT_KEY, identity.root_key,
					   sizeof(identity.root_key)) == sizeof(identity.root_key);
	identity.paired =
		zlrs_storage_settings_load(ZLRS_CONFIG_PAIRING, &identity.pairing,
					   sizeof(identity.pairing)) == sizeof(identity.pairing);
	changed();
	return rc;
}

int zlrs_config_set(enum zlrs_config_key key, int32_t value)
{
	zlrs_config_values_t candidate = state.saved;
	if (assign(&candidate, key, value) != 0) {
		return -EINVAL;
	}
	const uint32_t wide = value;
	const uint8_t byte = value;
	const size_t size = zlrs_storage_settings_size(key);
	zlrs_rf_pause();
	const int rc = zlrs_storage_settings_save(
		key, size == sizeof(wide) ? (const void *)&wide : (const void *)&byte, size);
	zlrs_rf_resume();
	if (rc != 0) {
		return -EIO;
	}
	state.saved = candidate;
	changed();
	return 0;
}

int zlrs_config_set_root_key(const uint8_t *key)
{
	zlrs_rf_pause();
	const int rc = zlrs_storage_settings_save(ZLRS_CONFIG_ROOT_KEY, key, ZLRS_KEY_LEN);
	if (rc == 0) {
		memcpy(identity.root_key, key, ZLRS_KEY_LEN);
		identity.configured = true;
		zlrs_rf_root_key(key);
		changed();
	}
	zlrs_rf_resume();
	return rc == 0 ? 0 : -EIO;
}

int zlrs_config_save_pairing(const zlrs_config_pairing_t *pairing)
{
	if (identity.paired && memcmp(pairing, &identity.pairing, sizeof(*pairing)) == 0) {
		return 0;
	}
	zlrs_rf_pause();
	const int rc = zlrs_storage_settings_save(ZLRS_CONFIG_PAIRING, pairing, sizeof(*pairing));
	zlrs_rf_resume();
	if (rc == 0) {
		identity.pairing = *pairing;
		identity.paired = true;
		changed();
	}
	return rc == 0 ? 0 : -EIO;
}

int zlrs_config_new_identity(void)
{
	uint8_t id[ZLRS_DEV_ID_LEN];
	if (!zlrs_storage_settings_new_dev_id(id)) {
		return -ENODEV;
	}
	zlrs_rf_pause();
	int rc = zlrs_storage_settings_save(ZLRS_CONFIG_DEV_ID, id, sizeof(id));
	if (rc == 0) {
		rc = zlrs_storage_settings_erase(ZLRS_CONFIG_PAIRING);
		memcpy(identity.dev_id, id, sizeof(id));
		identity.identity_valid = true;
		identity.paired = false;
		zlrs_rf_identity(id);
		changed();
	}
	zlrs_rf_resume();
	return rc == 0 ? 0 : -EIO;
}

int zlrs_config_erase_pairing(void)
{
	zlrs_rf_pause();
	const int rc = zlrs_storage_settings_erase(ZLRS_CONFIG_PAIRING);
	zlrs_rf_resume();
	if (rc == 0) {
		identity.paired = false;
		identity_reset_pending = true;
		changed();
	}
	return rc == 0 ? 0 : -EIO;
}

int zlrs_config_factory_reset(void)
{
	zlrs_rf_pause();
	const int rc = zlrs_storage_settings_erase_all();
	zlrs_rf_resume();
	if (rc == 0) {
		state.saved = defaults();
		memset(&identity, 0, sizeof(identity));
		identity_reset_pending = true;
		changed();
	}
	return rc == 0 ? 0 : -EIO;
}

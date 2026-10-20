/* SPDX-License-Identifier: Apache-2.0 */
#include "persist.h"

#include <errno.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

BUILD_ASSERT(IS_ENABLED(CONFIG_NVS), "zlrs keeps keys and pairing in NVS");

LOG_MODULE_REGISTER(zlrs_persist, CONFIG_APP_LOG_LEVEL);

/* Stable layout: explicit byte records, independent of enum/struct representation. */
enum record_id {
	RECORD_LAYOUT = 0,
	RECORD_BAND,
	RECORD_BAUD,
	RECORD_REGION,
	RECORD_ROLE,
	RECORD_PROFILE,
	RECORD_KEY,
	RECORD_ID,
	RECORD_PAIR,
};

#define NVS_LAYOUT      0x5A4C0002U
#define PAIR_BYTES      (ZLRS_DEV_ID_LEN + ZLRS_PAIR_SALT_LEN)
#define NVS_SECTOR_SIZE DT_PROP(DT_GPARENT(DT_NODELABEL(storage_partition)), erase_block_size)

static struct nvs_fs fs = {
	.offset = PARTITION_OFFSET(storage_partition),
	.sector_size = NVS_SECTOR_SIZE,
	.sector_count = PARTITION_SIZE(storage_partition) / NVS_SECTOR_SIZE,
};

/* RAM copy of saved records/defaults; reads do not access flash. */
static struct {
	zlrs_persist_settings_t settings;
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t root_key[ZLRS_KEY_LEN];
	zlrs_persist_pairing_t pairing;
	bool identity_valid;
	bool configured;
	bool paired;
} state;

static bool ready;

static const zlrs_rf_design_t rf_design = {
	.band = DT_PROP(DT_NODELABEL(zlrs_rf), band),
	.low_hz = DT_PROP(DT_NODELABEL(zlrs_rf), low_hz),
	.high_hz = DT_PROP(DT_NODELABEL(zlrs_rf), high_hz),
};

const zlrs_persist_settings_t *zlrs_persist_settings(void)
{
	return &state.settings;
}

bool zlrs_persist_get_device_id(uint8_t out[ZLRS_DEV_ID_LEN])
{
	if (!state.identity_valid) {
		return false;
	}
	memcpy(out, state.dev_id, ZLRS_DEV_ID_LEN);
	return true;
}

bool zlrs_persist_get_root_key(uint8_t out[ZLRS_KEY_LEN])
{
	if (!state.configured) {
		return false;
	}
	memcpy(out, state.root_key, ZLRS_KEY_LEN);
	return true;
}

bool zlrs_persist_get_pairing(zlrs_persist_pairing_t *out)
{
	if (!state.paired) {
		return false;
	}
	*out = state.pairing;
	return true;
}

static zlrs_persist_settings_t default_settings(void)
{
	return (zlrs_persist_settings_t){
		.band = ZLRS_DATA_BAND_AUTO,
		.region = DT_PROP(DT_NODELABEL(zlrs_rf), default_region),
		.role = IS_ENABLED(CONFIG_ZLRS_APP_ROLE_LEADER) ? ZLRS_ROLE_LEADER
								: ZLRS_ROLE_FOLLOWER,
		.profile = IS_ENABLED(CONFIG_ZLRS_APP_SECURITY_PROFILE_P0) ? ZLRS_PROFILE_P0
									   : ZLRS_PROFILE_P1,
		.data_baud = DT_PROP_OR(DT_ALIAS(zlrs_data_uart), current_speed, 115200U),
	};
}

static bool settings_valid(const zlrs_persist_settings_t *s)
{
	return (s->band == ZLRS_DATA_BAND_24 || s->band == ZLRS_DATA_BAND_SUBG ||
		s->band == ZLRS_DATA_BAND_AUTO) &&
	       (s->role == ZLRS_ROLE_LEADER || s->role == ZLRS_ROLE_FOLLOWER) &&
	       (s->profile == ZLRS_PROFILE_P1 ||
		(s->profile == ZLRS_PROFILE_P0 && IS_ENABLED(CONFIG_ZLRS_APP_DEBUG))) &&
	       s->data_baud >= 9600U && s->data_baud <= 4000000U &&
	       zlrs_region_get(s->region) != NULL &&
	       zlrs_region_validate(zlrs_region_get(s->region), &rf_design,
				    zlrs_region_get(s->region)->max_tx_power_dbm) == 0;
}

static int write_record(enum record_id id, const void *bytes, size_t size)
{
	if (!ready) {
		return -EIO;
	}
	const int rc = nvs_write(&fs, id, bytes, size);
	return rc == 0 || rc == (int)size ? 0 : (rc < 0 ? rc : -EIO);
}

static int reset_layout(void)
{
	/* Erase can fail after modifying flash. Retire the RAM identity before attempting it;
	 * only a successful initialization may make surviving records active again. */
	memset(&state, 0, sizeof(state));
	state.settings = default_settings();
	ready = false;
	int rc = nvs_clear(&fs);
	if (rc == 0) {
		rc = nvs_mount(&fs);
	}
	if (rc == 0) {
		uint8_t marker[4];
		sys_put_le32(NVS_LAYOUT, marker);
		ready = true;
		rc = write_record(RECORD_LAYOUT, marker, sizeof(marker));
	}
	ready = rc == 0;
	return rc;
}

static bool id_valid(const uint8_t id[ZLRS_DEV_ID_LEN])
{
	for (size_t i = 0; i < ZLRS_DEV_ID_LEN; i++) {
		if (id[i] != 0U) {
			return true;
		}
	}
	return false;
}

static int new_id(uint8_t id[ZLRS_DEV_ID_LEN])
{
	const struct device *const dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
	uint8_t block[32];
	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	int rc = entropy_get_entropy(dev, block, sizeof(block));
	if (rc == 0) {
		memcpy(id, block, ZLRS_DEV_ID_LEN);
		rc = id_valid(id) ? 0 : -EIO;
		if (rc != 0) {
			LOG_ERR("entropy returned an empty device ID");
		}
	}
	memset(block, 0, sizeof(block));
	return rc;
}

int zlrs_persist_init(void)
{
	memset(&state, 0, sizeof(state));
	state.settings = default_settings();
	ready = false;
	fs.flash_device = PARTITION_DEVICE(storage_partition);
	int rc = nvs_mount(&fs);
	if (rc != 0) {
		const struct flash_area *area;
		LOG_WRN("NVS mount failed (%d), reinitializing storage", rc);
		rc = flash_area_open(PARTITION_ID(storage_partition), &area);
		if (rc == 0) {
			rc = flash_area_flatten(area, 0, area->fa_size);
			flash_area_close(area);
		}
		if (rc == 0) {
			rc = nvs_mount(&fs);
		}
	}
	if (rc != 0) {
		return rc;
	}
	uint8_t bytes[PAIR_BYTES];
	rc = nvs_read(&fs, RECORD_LAYOUT, bytes, 4U);
	if (rc < 0 && rc != -ENOENT) {
		return rc;
	}
	if (rc != 4 || sys_get_le32(bytes) != NVS_LAYOUT) {
		rc = reset_layout();
		if (rc != 0) {
			return rc;
		}
	}
	ready = true;
	/* Load and validate the complete set of stored settings. */
	zlrs_persist_settings_t loaded = state.settings;
	for (enum record_id id = RECORD_BAND; id <= RECORD_PROFILE; id++) {
		const size_t size = id == RECORD_BAUD ? 4U : 1U;
		rc = nvs_read(&fs, id, bytes, size);
		if (rc == -ENOENT) {
			continue;
		}
		if (rc < 0) {
			return rc;
		}
		if (rc != (int)size) {
			LOG_WRN("invalid record length for %u", id);
			continue;
		}
		switch (id) {
		case RECORD_BAND:
			loaded.band = (zlrs_data_band_t)bytes[0];
			break;
		case RECORD_BAUD:
			loaded.data_baud = sys_get_le32(bytes);
			break;
		case RECORD_REGION:
			loaded.region = (zlrs_region_id_t)bytes[0];
			break;
		case RECORD_ROLE:
			loaded.role = (zlrs_role_t)bytes[0];
			break;
		case RECORD_PROFILE:
			loaded.profile = (zlrs_profile_t)bytes[0];
			break;
		default:
			break;
		}
	}
	/* The same rules apply on load and save; malformed fields keep their defaults. */
	zlrs_persist_settings_t candidate = state.settings;
	candidate.region = loaded.region;
	if (settings_valid(&candidate)) {
		state.settings = candidate;
	}
#define LOAD_FIELD(field)                                                                          \
	do {                                                                                       \
		candidate = state.settings;                                                        \
		candidate.field = loaded.field;                                                    \
		if (settings_valid(&candidate)) {                                                  \
			state.settings = candidate;                                                \
		}                                                                                  \
	} while (0)
	LOAD_FIELD(band);
	LOAD_FIELD(data_baud);
	LOAD_FIELD(role);
	LOAD_FIELD(profile);
#undef LOAD_FIELD
	rc = nvs_read(&fs, RECORD_ID, state.dev_id, sizeof(state.dev_id));
	if (rc < 0 && rc != -ENOENT) {
		return rc;
	}
	if (rc != sizeof(state.dev_id) || !id_valid(state.dev_id)) {
		rc = new_id(state.dev_id);
		if (rc == 0) {
			rc = write_record(RECORD_ID, state.dev_id, sizeof(state.dev_id));
		}
		if (rc != 0) {
			memset(state.dev_id, 0, sizeof(state.dev_id));
			return rc;
		}
	}
	state.identity_valid = true;
	rc = nvs_read(&fs, RECORD_KEY, state.root_key, sizeof(state.root_key));
	if (rc < 0 && rc != -ENOENT) {
		return rc;
	}
	state.configured = rc == sizeof(state.root_key);
	rc = nvs_read(&fs, RECORD_PAIR, bytes, sizeof(bytes));
	if (rc < 0 && rc != -ENOENT) {
		return rc;
	}
	state.paired = rc == PAIR_BYTES;
	if (state.paired) {
		memcpy(state.pairing.peer_id, bytes, ZLRS_DEV_ID_LEN);
		memcpy(state.pairing.pair_salt, bytes + ZLRS_DEV_ID_LEN, ZLRS_PAIR_SALT_LEN);
	}
	return 0;
}

int zlrs_persist_save_settings(const zlrs_persist_settings_t *s)
{
	if (!ready) {
		return -EIO;
	}
	if (!settings_valid(s)) {
		return -EINVAL;
	}
	uint8_t bytes[4];
	int rc;
	/* Each setting has its own record. Reflect every successful write, including partial saves. */
#define SAVE_FIELD(field, id)                                                                      \
	do {                                                                                       \
		if (state.settings.field != s->field) {                                            \
			bytes[0] = (uint8_t)s->field;                                              \
			rc = write_record(id, bytes, 1U);                                          \
			if (rc != 0) {                                                             \
				return rc;                                                         \
			}                                                                          \
			state.settings.field = s->field;                                           \
		}                                                                                  \
	} while (0)
	SAVE_FIELD(band, RECORD_BAND);
	if (state.settings.data_baud != s->data_baud) {
		sys_put_le32(s->data_baud, bytes);
		rc = write_record(RECORD_BAUD, bytes, sizeof(bytes));
		if (rc != 0) {
			return rc;
		}
		state.settings.data_baud = s->data_baud;
	}
	SAVE_FIELD(region, RECORD_REGION);
	SAVE_FIELD(role, RECORD_ROLE);
	SAVE_FIELD(profile, RECORD_PROFILE);
#undef SAVE_FIELD
	return 0;
}

int zlrs_persist_set_root_key(const uint8_t key[ZLRS_KEY_LEN])
{
	const int rc = write_record(RECORD_KEY, key, ZLRS_KEY_LEN);
	if (rc == 0) {
		memcpy(state.root_key, key, ZLRS_KEY_LEN);
		state.configured = true;
	}
	return rc;
}

int zlrs_persist_save_pairing(const zlrs_persist_pairing_t *pair)
{
	uint8_t bytes[PAIR_BYTES];
	memcpy(bytes, pair->peer_id, ZLRS_DEV_ID_LEN);
	memcpy(bytes + ZLRS_DEV_ID_LEN, pair->pair_salt, ZLRS_PAIR_SALT_LEN);
	const int rc = write_record(RECORD_PAIR, bytes, sizeof(bytes));
	if (rc == 0) {
		state.pairing = *pair;
		state.paired = true;
	}
	return rc;
}

int zlrs_persist_erase_pairing(void)
{
	if (!ready) {
		return -EIO;
	}
	const int rc = nvs_delete(&fs, RECORD_PAIR);
	if (rc == 0 || rc == -ENOENT) {
		state.paired = false;
		memset(&state.pairing, 0, sizeof(state.pairing));
		return 0;
	}
	return rc;
}

int zlrs_persist_new_identity(void)
{
	uint8_t id[ZLRS_DEV_ID_LEN];
	int rc = new_id(id);
	if (rc != 0) {
		return rc;
	}
	/* Invalidate the old pairing first: a partial failure cannot bind a new ID to an old peer. */
	rc = zlrs_persist_erase_pairing();
	if (rc == 0) {
		rc = write_record(RECORD_ID, id, sizeof(id));
	}
	if (rc == 0) {
		memcpy(state.dev_id, id, sizeof(id));
		state.identity_valid = true;
	}
	return rc;
}

int zlrs_persist_factory_reset(void)
{
	if (!ready) {
		return -EIO;
	}
	return reset_layout();
}

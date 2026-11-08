/* SPDX-License-Identifier: Apache-2.0 */
#include "persist.h"

#include <errno.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

BUILD_ASSERT(IS_ENABLED(CONFIG_SETTINGS_NVS), "zlrs settings use the native NVS backend");

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

#define NVS_LAYOUT    0x5A4C0002U
#define PAIR_BYTES    (ZLRS_DEV_ID_LEN + ZLRS_PAIR_SALT_LEN)
#define RESET_PENDING 0x5A4CFFFEU

/* Settings owns the sole NVS mount and its allocation metadata. */
static struct nvs_fs *storage_fs;
static bool read_failed;

/* RAM copy of saved records/defaults; reads do not access flash. */
static zlrs_persist_state_t state;

static uint32_t present;
static uint32_t schema;

static const struct {
	const char *name;
	void *target;
	uint8_t size;
	uint8_t wire_size;
} records[] = {
	[RECORD_LAYOUT] = {"zlrs/schema", &schema, sizeof(schema), 4},
	[RECORD_BAND] = {"zlrs/band", &state.settings.band, sizeof(state.settings.band), 1},
	[RECORD_BAUD] = {"zlrs/baud", &state.settings.data_baud, sizeof(state.settings.data_baud),
			 4},
	[RECORD_REGION] = {"zlrs/region", &state.settings.region, sizeof(state.settings.region), 1},
	[RECORD_ROLE] = {"zlrs/role", &state.settings.role, sizeof(state.settings.role), 1},
	[RECORD_PROFILE] = {"zlrs/profile", &state.settings.profile, sizeof(state.settings.profile),
			    1},
	[RECORD_KEY] = {"zlrs/key", state.root_key, sizeof(state.root_key), ZLRS_KEY_LEN},
	[RECORD_ID] = {"zlrs/id", state.dev_id, sizeof(state.dev_id), ZLRS_DEV_ID_LEN},
	[RECORD_PAIR] = {"zlrs/pair", &state.pairing, sizeof(state.pairing), PAIR_BYTES},
};

BUILD_ASSERT(sizeof(state.pairing) == PAIR_BYTES, "pairing records contain only byte arrays");

/* Integer records have a stable little-endian wire representation, regardless of enum width. */
static uint32_t scalar_value(const void *data, size_t size)
{
	uint32_t value = 0;
	if (size == 1) {
		return *(const uint8_t *)data;
	}
	if (size == 2) {
		uint16_t half;
		memcpy(&half, data, sizeof(half));
		return half;
	}
	memcpy(&value, data, sizeof(value));
	return value;
}

static int load_setting(const char *name, size_t len, settings_read_cb read_cb, void *arg)
{
	for (size_t i = 0; i < ARRAY_SIZE(records); i++) {
		if (strcmp(name, records[i].name + sizeof("zlrs/") - 1U) != 0) {
			continue;
		}
		uint8_t bytes[ZLRS_KEY_LEN];
		int rc = len == records[i].wire_size ? read_cb(arg, bytes, len) : -EINVAL;
		if (rc < 0 || rc != len) {
			read_failed = true;
			return rc < 0 ? rc : -EIO;
		}
		if (i <= RECORD_PROFILE && records[i].size != 1) {
			const uint32_t value = len == 4 ? sys_get_le32(bytes) : bytes[0];
			if (records[i].size == 2) {
				const uint16_t half = value;
				memcpy(records[i].target, &half, sizeof(half));
			} else {
				memcpy(records[i].target, &value, sizeof(value));
			}
		} else {
			memcpy(records[i].target, bytes, len);
		}
		present |= BIT(i);
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(zlrs, "zlrs", NULL, load_setting, NULL, NULL);

static bool ready;

const zlrs_persist_state_t *zlrs_persist_state(void)
{
	return &state;
}

static const zlrs_persist_settings_t default_settings = {
	.band = ZLRS_DATA_BAND_AUTO,
	.region = DT_PROP(DT_NODELABEL(zlrs_rf), default_region),
	.role = IS_ENABLED(CONFIG_ZLRS_APP_ROLE_LEADER) ? ZLRS_ROLE_LEADER : ZLRS_ROLE_FOLLOWER,
	.profile =
		IS_ENABLED(CONFIG_ZLRS_APP_SECURITY_PROFILE_P0) ? ZLRS_PROFILE_P0 : ZLRS_PROFILE_P1,
	.data_baud = DT_PROP_OR(DT_ALIAS(zlrs_data_uart), current_speed, 115200U),
};

static bool settings_valid(const zlrs_persist_settings_t *s)
{
	return (s->band == ZLRS_DATA_BAND_24 || s->band == ZLRS_DATA_BAND_SUBG ||
		s->band == ZLRS_DATA_BAND_AUTO) &&
	       (s->role == ZLRS_ROLE_LEADER || s->role == ZLRS_ROLE_FOLLOWER) &&
	       (s->profile == ZLRS_PROFILE_P1 ||
		(s->profile == ZLRS_PROFILE_P0 && IS_ENABLED(CONFIG_ZLRS_APP_DEBUG))) &&
	       s->data_baud >= 9600U && s->data_baud <= 4000000U &&
	       zlrs_region_get(s->region) != NULL;
}

static int write_record(enum record_id id, const void *bytes, size_t size)
{
	if (!ready) {
		return -EIO;
	}
	return settings_save_one(records[id].name, bytes, size);
}

static int reset_layout(void)
{
	memset(&state, 0, sizeof(state));
	state.settings = default_settings;
	ready = false;
	/* Mark reset before erasing, so an interrupted erase cannot reactivate credentials. */
	uint8_t marker[4];
	sys_put_le32(RESET_PENDING, marker);
	int rc = settings_save_one(records[RECORD_LAYOUT].name, marker, sizeof(marker));
	if (rc == 0) {
		rc = nvs_clear(storage_fs);
	}
	if (rc == 0) {
		rc = nvs_mount(storage_fs);
	}
	if (rc == 0) {
		/* Native load prunes the erased name IDs and rebuilds its name cache. */
		rc = settings_load_subtree("zlrs");
	}
	if (rc == 0) {
		sys_put_le32(NVS_LAYOUT, marker);
		rc = settings_save_one(records[RECORD_LAYOUT].name, marker, sizeof(marker));
	}
	ready = rc == 0;
	present = 0;
	schema = 0;
	return rc;
}

static int load_records(void)
{
	present = 0;
	schema = 0;
	int rc = settings_subsys_init();
	if (rc != 0) {
		/* No valid backend mount exists yet. Discard unreadable storage and retry. */
		const struct flash_area *area;
		rc = flash_area_open(PARTITION_ID(storage_partition), &area);
		if (rc == 0) {
			rc = flash_area_flatten(area, 0, area->fa_size);
			flash_area_close(area);
		}
		if (rc == 0) {
			rc = settings_subsys_init();
		}
	}
	if (rc != 0) {
		return rc;
	}
	void *storage;
	rc = settings_storage_get(&storage);
	if (rc != 0) {
		return rc;
	}
	storage_fs = storage;
	read_failed = false;
	rc = settings_load_subtree("zlrs");
	if (rc != 0 || read_failed || schema != NVS_LAYOUT) {
		LOG_WRN("settings unreadable or schema missing; reinitializing storage");
		return reset_layout();
	}
	return 0;
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
	state.settings = default_settings;
	ready = false;
	int rc = load_records();
	if (rc != 0) {
		return rc;
	}
	ready = true;
	if (!settings_valid(&state.settings)) {
		rc = reset_layout();
		return rc == 0 ? zlrs_persist_init() : rc;
	}
	if ((present & BIT(RECORD_ID)) == 0 || !id_valid(state.dev_id)) {
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
	state.configured = (present & BIT(RECORD_KEY)) != 0;
	state.paired = (present & BIT(RECORD_PAIR)) != 0;
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
	/* Commit in record order and adopt each successful write, including partial saves. */
	for (enum record_id id = RECORD_BAND; id <= RECORD_PROFILE; id++) {
		const void *value = (const uint8_t *)s + ((const uint8_t *)records[id].target -
							  (const uint8_t *)&state.settings);
		if (memcmp(value, records[id].target, records[id].size) == 0) {
			continue;
		}
		uint8_t bytes[4];
		sys_put_le32(scalar_value(value, records[id].size), bytes);
		int rc = write_record(id, bytes, records[id].wire_size);
		if (rc != 0) {
			return rc;
		}
		memcpy(records[id].target, value, records[id].size);
	}
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

int zlrs_persist_save_pairing(const zlrs_pairing_t *pair)
{
	const int rc = write_record(RECORD_PAIR, pair, sizeof(*pair));
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
	const int rc = settings_delete(records[RECORD_PAIR].name);
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

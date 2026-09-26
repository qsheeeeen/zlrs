/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "storage/settings.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_REGISTER(zlrs_storage_settings, CONFIG_APP_LOG_LEVEL);

/* One row per setting: the only place an NVS id and a size are written down */
static const struct {
	uint16_t nvs_id;
	uint8_t size;
} settings[ZLRS_SETTING_COUNT] = {
	[ZLRS_SETTING_ROOT_KEY] = {1U, ZLRS_KEY_LEN},
	[ZLRS_SETTING_DEV_ID] = {2U, ZLRS_DEV_ID_LEN},
	[ZLRS_SETTING_PAIRING] = {6U, sizeof(zlrs_storage_settings_pairing_t)},
	[ZLRS_SETTING_FAILSAFE] = {4U, ZLRS_CFG_FAILSAFE_LEN},
	[ZLRS_SETTING_BAUD] = {5U, sizeof(uint32_t)},
	[ZLRS_SETTING_REGION] = {7U, sizeof(uint8_t)},
	[ZLRS_SETTING_POWER] = {8U, sizeof(int8_t)},
	[ZLRS_SETTING_ROLE] = {9U, sizeof(uint8_t)},
	[ZLRS_SETTING_PROFILE] = {10U, sizeof(uint8_t)},
};

#define NVS_SECTOR_SIZE 4096U

static struct nvs_fs fs = {
	.offset = PARTITION_OFFSET(storage_partition),
	.sector_size = NVS_SECTOR_SIZE,
	.sector_count = PARTITION_SIZE(storage_partition) / NVS_SECTOR_SIZE,
};

static bool ready;

int zlrs_storage_settings_init(void)
{
	int rc;

	fs.flash_device = PARTITION_DEVICE(storage_partition);
	if (fs.flash_device == NULL) {
		return -1;
	}

	rc = nvs_mount(&fs);
	if (rc != 0) {
		/*
		 * A settings area that cannot be mounted is wiped and mounted again: the device must keep
		 * working with defaults (pairing, role and the rest are re-set from the config port) rather
		 * than refuse to start. It also covers the area holding data from a different layout.
		 */
		const struct flash_area *area;

		LOG_WRN("NVS mount failed (%d), erasing the settings area", rc);
		if (flash_area_open(PARTITION_ID(storage_partition), &area) == 0) {
			(void)flash_area_flatten(area, 0, area->fa_size);
			flash_area_close(area);
			rc = nvs_mount(&fs);
		}
	}
	if (rc != 0) {
		LOG_ERR("NVS mount failed (%d), running without stored settings", rc);
		return rc;
	}

	ready = true;

	return 0;
}

static int read_record(uint16_t id, void *out, size_t len)
{
	if (!ready) {
		return -1;
	}

	return nvs_read(&fs, id, out, len);
}

static int write_record(uint16_t id, const void *data, size_t len)
{
	int rc;

	if (!ready) {
		LOG_ERR("store not ready; write of id %u dropped", id);
		return -1;
	}

	rc = nvs_write(&fs, id, data, len);
	/* Success has two shapes: len bytes written, or 0 when the content was identical and flash
	 * untouched. A sign check alone would call the first a failure and mismatched lengths a success. */
	if (rc == 0 || rc == (ssize_t)len) {
		return 0;
	}

	LOG_ERR("nvs_write(id %u, len %u) returned %d: treating as failure", id, (unsigned int)len,
		rc);

	return (rc < 0) ? (int)rc : -EIO;
}

size_t zlrs_storage_settings_size(enum zlrs_setting id)
{
	if (id >= ZLRS_SETTING_COUNT) {
		return 0U;
	}

	return settings[id].size;
}

int zlrs_storage_settings_load(enum zlrs_setting id, void *out, size_t len)
{
	if (id >= ZLRS_SETTING_COUNT || len != settings[id].size) {
		return -EINVAL;
	}

	return read_record(settings[id].nvs_id, out, len);
}

int zlrs_storage_settings_save(enum zlrs_setting id, const void *value, size_t len)
{
	if (id >= ZLRS_SETTING_COUNT || len != settings[id].size) {
		return -EINVAL;
	}

	return write_record(settings[id].nvs_id, value, len);
}

int zlrs_storage_settings_erase(enum zlrs_setting id)
{
	int rc;

	if (id >= ZLRS_SETTING_COUNT) {
		return -EINVAL;
	}
	if (!ready) {
		return -1;
	}

	rc = nvs_delete(&fs, settings[id].nvs_id);

	/* A missing record counts as success: erasing is what removes it */
	return (rc == 0 || rc == -ENOENT) ? 0 : rc;
}

int zlrs_storage_settings_erase_all(void)
{
	for (size_t i = 0U; i < ZLRS_SETTING_COUNT; i++) {
		const int rc = zlrs_storage_settings_erase((enum zlrs_setting)i);

		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

bool zlrs_storage_settings_dev_id_valid(const uint8_t id[ZLRS_DEV_ID_LEN])
{
	for (size_t i = 0U; i < ZLRS_DEV_ID_LEN; i++) {
		if (id[i] != 0U) {
			return true;
		}
	}

	return false;
}

bool zlrs_storage_settings_new_dev_id(uint8_t out[ZLRS_DEV_ID_LEN])
{
	const struct device *const entropy_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
	uint8_t block[32];
	int rc;

	if (!device_is_ready(entropy_dev)) {
		LOG_ERR("no dev id: entropy device not ready");
		return false;
	}

	/* Read a full block, the size the driver and the self-test are known to handle */
	rc = entropy_get_entropy(entropy_dev, block, sizeof(block));
	if (rc != 0) {
		LOG_ERR("no dev id: entropy read failed (%d)", rc);
		return false;
	}

	memcpy(out, block, ZLRS_DEV_ID_LEN);
	memset(block, 0, sizeof(block));
	if (!zlrs_storage_settings_dev_id_valid(out)) {
		LOG_ERR("no dev id: entropy returned zeros");
		return false;
	}

	return true;
}

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}

	return -1;
}

int zlrs_storage_settings_parse_hex_key(const char *hex, uint8_t out[ZLRS_KEY_LEN])
{
	if (strlen(hex) != ZLRS_KEY_LEN * 2U) {
		return -1;
	}

	for (size_t i = 0; i < ZLRS_KEY_LEN; i++) {
		const int hi = hex_nibble(hex[i * 2U]);
		const int lo = hex_nibble(hex[i * 2U + 1U]);

		if (hi < 0 || lo < 0) {
			return -1;
		}
		out[i] = (uint8_t)((hi << 4) | lo);
	}

	return 0;
}

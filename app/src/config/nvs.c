/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "config/nvs.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_REGISTER(zlrs_storage_settings, CONFIG_APP_LOG_LEVEL);

/* Record 0 identifies the layout; setting records follow the config key order. */
#define NVS_LAYOUT_ID 0U
#define NVS_LAYOUT    0x5A4C0001U

static const uint8_t setting_sizes[ZLRS_CONFIG_KEY_COUNT] = {
	[ZLRS_CONFIG_BAND] = sizeof(uint8_t),
	[ZLRS_CONFIG_DATA_BAUD] = sizeof(uint32_t),
	[ZLRS_CONFIG_REGION] = sizeof(uint8_t),
	[ZLRS_CONFIG_POWER] = sizeof(int8_t),
	[ZLRS_CONFIG_ROLE] = sizeof(uint8_t),
	[ZLRS_CONFIG_PROFILE] = sizeof(uint8_t),
	[ZLRS_CONFIG_ROOT_KEY] = ZLRS_KEY_LEN,
	[ZLRS_CONFIG_DEV_ID] = ZLRS_DEV_ID_LEN,
	[ZLRS_CONFIG_PAIRING] = sizeof(zlrs_storage_settings_pairing_t),
};

#define NVS_SECTOR_SIZE 4096U

static struct nvs_fs fs = {
	.offset = PARTITION_OFFSET(storage_partition),
	.sector_size = NVS_SECTOR_SIZE,
	.sector_count = PARTITION_SIZE(storage_partition) / NVS_SECTOR_SIZE,
};

static bool ready;

static int reset_layout(void)
{
	const uint32_t layout = NVS_LAYOUT;

	ready = false;
	int rc = nvs_clear(&fs);
	if (rc == 0) {
		rc = nvs_mount(&fs);
	}
	if (rc == 0) {
		rc = nvs_write(&fs, NVS_LAYOUT_ID, &layout, sizeof(layout));
		if (rc == sizeof(layout)) {
			rc = 0;
		} else if (rc >= 0) {
			rc = -EIO;
		}
	}
	ready = rc == 0;
	return rc;
}

int zlrs_storage_settings_init(void)
{
	int rc;

	ready = false;
	fs.flash_device = PARTITION_DEVICE(storage_partition);
	if (fs.flash_device == NULL) {
		return -1;
	}

	rc = nvs_mount(&fs);
	if (rc != 0) {
		/*
		 * A settings area that cannot be mounted is wiped and mounted again: the device must keep
		 * working with defaults (pairing, role and the rest are re-set from the management port) rather
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

	uint32_t layout = 0U;
	if (nvs_read(&fs, NVS_LAYOUT_ID, &layout, sizeof(layout)) != sizeof(layout) ||
	    layout != NVS_LAYOUT) {
		LOG_INF("Initializing settings layout");
		return reset_layout();
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

size_t zlrs_storage_settings_size(enum zlrs_config_key key)
{
	if ((unsigned int)key >= ZLRS_CONFIG_KEY_COUNT) {
		return 0U;
	}

	return setting_sizes[key];
}

int zlrs_storage_settings_load(enum zlrs_config_key key, void *out, size_t len)
{
	if ((unsigned int)key >= ZLRS_CONFIG_KEY_COUNT || len != setting_sizes[key]) {
		return -EINVAL;
	}

	return read_record((uint16_t)key + 1U, out, len);
}

int zlrs_storage_settings_save(enum zlrs_config_key key, const void *value, size_t len)
{
	if ((unsigned int)key >= ZLRS_CONFIG_KEY_COUNT || len != setting_sizes[key]) {
		return -EINVAL;
	}

	return write_record((uint16_t)key + 1U, value, len);
}

int zlrs_storage_settings_erase(enum zlrs_config_key key)
{
	int rc;

	if ((unsigned int)key >= ZLRS_CONFIG_KEY_COUNT) {
		return -EINVAL;
	}
	if (!ready) {
		return -1;
	}

	rc = nvs_delete(&fs, (uint16_t)key + 1U);

	/* A missing record counts as success: erasing is what removes it */
	return (rc == 0 || rc == -ENOENT) ? 0 : rc;
}

int zlrs_storage_settings_erase_all(void)
{
	if (!ready) {
		return -EIO;
	}
	return reset_layout();
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

	/* Read a full block, the size the entropy driver supports */
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

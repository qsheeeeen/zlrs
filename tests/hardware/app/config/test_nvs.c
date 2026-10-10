/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>
#include "config/nvs.h"

/* This fixture owns the settings partition. It runs on simulated flash or a reset test board. */
ZTEST(settings, test_layout_and_persistence)
{
	struct nvs_fs inspect = {
		.flash_device = PARTITION_DEVICE(storage_partition),
		.offset = PARTITION_OFFSET(storage_partition),
		.sector_size = 4096U,
		.sector_count = PARTITION_SIZE(storage_partition) / 4096U,
	};
	const uint32_t incompatible = 0x12345678U;
	const uint8_t sentinel = 0xA5U;
	uint8_t value[ZLRS_KEY_LEN];
	uint8_t loaded[ZLRS_KEY_LEN];

	zassert_ok(nvs_mount(&inspect));
	zassert_equal(nvs_write(&inspect, 0U, &incompatible, sizeof(incompatible)),
		      sizeof(incompatible));
	zassert_equal(nvs_write(&inspect, 1U, &sentinel, sizeof(sentinel)), sizeof(sentinel));
	zassert_equal(nvs_write(&inspect, 65000U, &sentinel, sizeof(sentinel)), sizeof(sentinel));
	zassert_ok(zlrs_storage_settings_init());
	zassert_equal(zlrs_storage_settings_load(ZLRS_CONFIG_BAND, loaded, 1U), -ENOENT);
	zassert_ok(nvs_mount(&inspect));
	zassert_equal(nvs_read(&inspect, 65000U, loaded, 1U), -ENOENT);

	for (enum zlrs_config_key key = 0; key < ZLRS_CONFIG_KEY_COUNT; key++) {
		const size_t size = zlrs_storage_settings_size(key);
		zassert_true(size > 0U && size <= sizeof(value));
		memset(value, (unsigned int)key + 17U, size);
		zassert_ok(zlrs_storage_settings_save(key, value, size));
		/* Repeating a write must succeed even when NVS avoids touching flash. */
		zassert_ok(zlrs_storage_settings_save(key, value, size));
	}
	zassert_ok(zlrs_storage_settings_init());
	for (enum zlrs_config_key key = 0; key < ZLRS_CONFIG_KEY_COUNT; key++) {
		const size_t size = zlrs_storage_settings_size(key);
		memset(value, (unsigned int)key + 17U, size);
		zassert_equal(zlrs_storage_settings_load(key, loaded, size), size);
		zassert_mem_equal(value, loaded, size);
	}
	zassert_equal(zlrs_storage_settings_save(ZLRS_CONFIG_BAND, value, 4U), -EINVAL);
	zassert_equal(zlrs_storage_settings_load(ZLRS_CONFIG_KEY_COUNT, loaded, 1U), -EINVAL);
	zassert_equal(zlrs_storage_settings_size((enum zlrs_config_key) - 1), 0U);
	zassert_ok(zlrs_storage_settings_erase(ZLRS_CONFIG_ROOT_KEY));
	zassert_ok(zlrs_storage_settings_erase(ZLRS_CONFIG_ROOT_KEY));
	zassert_equal(zlrs_storage_settings_load(ZLRS_CONFIG_ROOT_KEY, loaded, ZLRS_KEY_LEN),
		      -ENOENT);

	/* Reset must also remove records outside the current setting table. */
	zassert_ok(nvs_mount(&inspect));
	zassert_equal(nvs_write(&inspect, 65000U, &sentinel, sizeof(sentinel)), sizeof(sentinel));
	zassert_ok(zlrs_storage_settings_erase_all());
	zassert_ok(zlrs_storage_settings_init());
	for (enum zlrs_config_key key = 0; key < ZLRS_CONFIG_KEY_COUNT; key++) {
		zassert_equal(
			zlrs_storage_settings_load(key, loaded, zlrs_storage_settings_size(key)),
			-ENOENT);
	}
	zassert_ok(nvs_mount(&inspect));
	zassert_equal(nvs_read(&inspect, 65000U, loaded, 1U), -ENOENT);
	/* The marker survives reset, and the remounted store accepts new writes. */
	uint32_t layout;
	zassert_equal(nvs_read(&inspect, 0U, &layout, sizeof(layout)), sizeof(layout));
	zassert_not_equal(layout, incompatible);
	zassert_ok(zlrs_storage_settings_save(ZLRS_CONFIG_BAND, &sentinel, sizeof(sentinel)));
	zassert_ok(zlrs_storage_settings_init());
	zassert_equal(zlrs_storage_settings_load(ZLRS_CONFIG_BAND, loaded, 1U), 1);
	zassert_equal(loaded[0], sentinel);
	zassert_ok(zlrs_storage_settings_erase_all());
}

ZTEST_SUITE(settings, NULL, NULL, NULL, NULL, NULL);

/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
#include "persist.h"
#include "platform/caps.h"

/* Preserve the entire real partition, including NVS metadata, before destructive fixture cases. */
static uint8_t backup[PARTITION_SIZE(storage_partition)];
static const struct flash_area *area;
static bool fail_write;
ssize_t __real_nvs_write(struct nvs_fs *fs, uint16_t id, const void *data, size_t len);
ssize_t __wrap_nvs_write(struct nvs_fs *fs, uint16_t id, const void *data, size_t len)
{
	if (fail_write) {
		return -EIO;
	}
	return __real_nvs_write(fs, id, data, len);
}
static void *setup(void)
{
	zassert_ok(flash_area_open(PARTITION_ID(storage_partition), &area));
	zassert_ok(flash_area_read(area, 0, backup, sizeof(backup)));
	return NULL;
}
static void teardown(void *unused)
{
	ARG_UNUSED(unused);
	fail_write = false;
	zassert_ok(flash_area_flatten(area, 0, sizeof(backup)));
	zassert_ok(flash_area_write(area, 0, backup, sizeof(backup)));
	flash_area_close(area);
}
static struct nvs_fs inspect = {
	.flash_device = PARTITION_DEVICE(storage_partition),
	.offset = PARTITION_OFFSET(storage_partition),
	.sector_size = DT_PROP(DT_GPARENT(DT_NODELABEL(storage_partition)), erase_block_size),
	.sector_count = PARTITION_SIZE(storage_partition) /
			DT_PROP(DT_GPARENT(DT_NODELABEL(storage_partition)), erase_block_size),
};
ZTEST(settings, test_layout_and_persistence)
{
	zassert_ok(nvs_mount(&inspect));
	const uint8_t incompatible[4] = {0x78, 0x56, 0x34, 0x12};
	const uint8_t sentinel = 0xA5;
	zassert_equal(nvs_write(&inspect, 0, incompatible, 4), 4);
	zassert_equal(nvs_write(&inspect, 65000, &sentinel, 1), 1);
	zassert_ok(zlrs_persist_init());
	zassert_true(zlrs_persist_state()->identity_valid);
	zassert_false(zlrs_persist_state()->configured);
	zassert_false(zlrs_persist_state()->paired);
	zassert_ok(nvs_mount(&inspect));
	uint8_t raw[32];
	zassert_equal(nvs_read(&inspect, 65000, raw, 1), -ENOENT);
	/* Populate the existing 1..9 layout directly, with the original byte format. */
	const uint8_t band = ZLRS_DATA_BAND_SUBG;
	const uint8_t region = ZLRS_DEFAULT_REGION;
	const uint8_t power = (uint8_t)-10;
	const uint8_t role = ZLRS_ROLE_RX;
	const uint8_t profile = ZLRS_PROFILE_P1;
	const uint8_t baud[4] = {0x00, 0x84, 0x03, 0x00}; /* 230400, little endian */
	uint8_t key[32];
	memset(key, 0x42, sizeof(key));
	const uint8_t id[4] = {1, 2, 3, 4};
	uint8_t pair[ZLRS_DEV_ID_LEN + ZLRS_PAIR_SALT_LEN];
	memset(pair, 0x37, sizeof(pair));
	zassert_equal(nvs_write(&inspect, 1, &band, 1), 1);
	zassert_equal(nvs_write(&inspect, 2, baud, 4), 4);
	zassert_equal(nvs_write(&inspect, 3, &region, 1), 1);
	zassert_equal(nvs_write(&inspect, 4, &power, 1), 1);
	zassert_equal(nvs_write(&inspect, 5, &role, 1), 1);
	zassert_equal(nvs_write(&inspect, 6, &profile, 1), 1);
	zassert_equal(nvs_write(&inspect, 7, key, 32), 32);
	zassert_equal(nvs_write(&inspect, 8, id, 4), 4);
	zassert_equal(nvs_write(&inspect, 9, pair, sizeof(pair)), sizeof(pair));
	zassert_ok(zlrs_persist_init());
	const zlrs_persist_state_t *saved = zlrs_persist_state();
	zassert_equal(saved->settings.band, ZLRS_DATA_BAND_SUBG);
	zassert_equal(saved->settings.data_baud, 230400);
	zassert_equal(saved->settings.role, ZLRS_ROLE_RX);
	zassert_true(saved->paired && saved->configured && saved->identity_valid);
	zassert_mem_equal(saved->root_key, key, 32);
	zassert_mem_equal(saved->dev_id, id, 4);
	zassert_mem_equal(saved->pairing.peer_id, pair, 4);
	zlrs_persist_settings_t settings = saved->settings;
	settings.data_baud = 460800;
	zassert_ok(zlrs_persist_save_settings(&settings));
	zassert_ok(zlrs_persist_save_settings(&settings));
	zassert_ok(nvs_mount(&inspect));
	zassert_equal(nvs_read(&inspect, 2, raw, 4), 4);
	zassert_equal(sys_get_le32(raw), 460800);
	fail_write = true;
	settings.data_baud = 115200;
	zassert_equal(zlrs_persist_save_settings(&settings), -EIO);
	zassert_equal(saved->settings.data_baud, 460800);
	uint8_t other_key[32];
	memset(other_key, 0x99, 32);
	zassert_equal(zlrs_persist_set_root_key(other_key), -EIO);
	zassert_mem_equal(saved->root_key, key, 32);
	fail_write = false;
	settings.profile = ZLRS_PROFILE_P2;
	zassert_equal(zlrs_persist_save_settings(&settings), -EINVAL);
	settings = saved->settings;
	settings.region = (zlrs_region_id_t)255;
	zassert_equal(zlrs_persist_save_settings(&settings), -EINVAL);
	settings = saved->settings;
	settings.data_baud = 1;
	zassert_equal(zlrs_persist_save_settings(&settings), -EINVAL);
	zlrs_persist_pairing_t pairing = saved->pairing;
	pairing.peer_id[0] = 0x88;
	zassert_ok(zlrs_persist_save_pairing(&pairing));
	zassert_ok(zlrs_persist_init());
	zassert_equal(saved->pairing.peer_id[0], 0x88);
	zassert_ok(zlrs_persist_new_identity());
	zassert_false(saved->paired);
	zassert_false(memcmp(saved->dev_id, id, 4) == 0);
	zassert_ok(zlrs_persist_erase_pairing());
	zassert_ok(zlrs_persist_erase_pairing());
	zassert_ok(zlrs_persist_factory_reset());
	zassert_false(saved->configured || saved->paired || saved->identity_valid);
	zassert_ok(zlrs_persist_init());
	zassert_true(saved->identity_valid);
	zassert_false(saved->configured || saved->paired);
	zassert_ok(nvs_mount(&inspect));
	zassert_equal(nvs_read(&inspect, 0, raw, 4), 4);
	zassert_equal(sys_get_le32(raw), 0x5A4C0001);
}
ZTEST_SUITE(settings, NULL, setup, NULL, NULL, teardown);

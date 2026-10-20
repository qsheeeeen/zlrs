/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
#include "persist.h"

/* Preserve the entire real partition, including NVS metadata, before destructive fixture cases. */
static uint8_t backup[PARTITION_SIZE(storage_partition)];
static const struct flash_area *area;
static bool fail_write;
static uint16_t fail_record = UINT16_MAX;

enum reset_fault {
	RESET_OK,
	CLEAR_BEFORE,
	CLEAR_AFTER,
	REMOUNT,
	MARKER_WRITE
};
static enum reset_fault reset_fault;
int __real_nvs_clear(struct nvs_fs *fs);
int __real_nvs_mount(struct nvs_fs *fs);

int __wrap_nvs_clear(struct nvs_fs *fs)
{
	if (reset_fault == CLEAR_BEFORE) {
		return -EIO;
	}
	const int rc = __real_nvs_clear(fs);
	return reset_fault == CLEAR_AFTER && rc == 0 ? -EIO : rc;
}

int __wrap_nvs_mount(struct nvs_fs *fs)
{
	return reset_fault == REMOUNT ? -EIO : __real_nvs_mount(fs);
}

ssize_t __real_nvs_write(struct nvs_fs *fs, uint16_t id, const void *data, size_t len);

ssize_t __wrap_nvs_write(struct nvs_fs *fs, uint16_t id, const void *data, size_t len)
{
	if (fail_write || id == fail_record || (reset_fault == MARKER_WRITE && id == 0)) {
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
	reset_fault = RESET_OK;
	zassert_ok(flash_area_flatten(area, 0, sizeof(backup)));
	zassert_ok(flash_area_write(area, 0, backup, sizeof(backup)));
	uint8_t restored[128];
	for (size_t offset = 0; offset < sizeof(backup); offset += sizeof(restored)) {
		const size_t size = MIN(sizeof(restored), sizeof(backup) - offset);
		zassert_ok(flash_area_read(area, offset, restored, size));
		zassert_mem_equal(restored, backup + offset, size, "restored NVS differs at %zu",
				  offset);
	}
	printk("NVS_RESTORE bytes=%zu verified=1\n", sizeof(backup));
	flash_area_close(area);
}
static struct nvs_fs inspect = {
	.flash_device = PARTITION_DEVICE(storage_partition),
	.offset = PARTITION_OFFSET(storage_partition),
	.sector_size = DT_PROP(DT_GPARENT(DT_NODELABEL(storage_partition)), erase_block_size),
	.sector_count = PARTITION_SIZE(storage_partition) /
			DT_PROP(DT_GPARENT(DT_NODELABEL(storage_partition)), erase_block_size),
};

/* Every scenario starts from a fresh partition; suite teardown restores the original bytes. */
static void before(void *unused)
{
	ARG_UNUSED(unused);
	fail_write = false;
	fail_record = UINT16_MAX;
	reset_fault = RESET_OK;
	zassert_ok(flash_area_flatten(area, 0, sizeof(backup)));
	zassert_ok(zlrs_persist_init());
	zassert_ok(nvs_mount(&inspect));
}

static const uint8_t key[ZLRS_KEY_LEN] = {0x42};
static const zlrs_persist_pairing_t pair = {.peer_id = {9, 8, 7, 6},
					    .pair_salt = {1, 2, 3, 4, 5, 6, 7, 8}};

ZTEST(settings, test_incompatible_layout_discards_old_records)
{
	const uint8_t incompatible[4] = {0x01, 0x00, 0x4c, 0x5a};
	const uint8_t sentinel = 0xa5;
	zassert_equal(nvs_write(&inspect, 0, incompatible, 4), 4);
	zassert_equal(nvs_write(&inspect, 65000, &sentinel, 1), 1);
	zassert_ok(zlrs_persist_init());
	zassert_ok(nvs_mount(&inspect));
	uint8_t raw[4];
	zassert_equal(nvs_read(&inspect, 65000, raw, 1), -ENOENT);
	zassert_equal(nvs_read(&inspect, 0, raw, 4), 4);
	zassert_equal(sys_get_le32(raw), 0x5a4c0002);
}

ZTEST(settings, test_missing_values_leave_output_untouched)
{
	uint8_t saved_key[ZLRS_KEY_LEN], saved_id[ZLRS_DEV_ID_LEN];
	zlrs_persist_pairing_t saved_pair;
	uint8_t untouched[ZLRS_KEY_LEN];
	memset(untouched, 0xa5, sizeof(untouched));
	memset(saved_key, 0xa5, sizeof(saved_key));
	memset(&saved_pair, 0xa5, sizeof(saved_pair));
	zassert_true(zlrs_persist_get_device_id(saved_id));
	zassert_false(zlrs_persist_get_root_key(saved_key));
	zassert_false(zlrs_persist_get_pairing(&saved_pair));
	zassert_mem_equal(saved_key, untouched, sizeof(saved_key));
	zassert_mem_equal(&saved_pair, untouched, sizeof(saved_pair));
}

ZTEST(settings, test_independent_byte_records_load)
{
	const uint8_t band = ZLRS_DATA_BAND_SUBG;
	const uint8_t region = DT_PROP(DT_NODELABEL(zlrs_rf), band) == ZLRS_BAND_SUBG_LF
				       ? ZLRS_REGION_CN470
				       : ZLRS_REGION_EU868;
	const uint8_t role = ZLRS_ROLE_FOLLOWER, profile = ZLRS_PROFILE_P1;
	const uint8_t baud[4] = {0x00, 0x84, 0x03, 0x00};
	const uint8_t id[4] = {1, 2, 3, 4};
	uint8_t pairing[ZLRS_DEV_ID_LEN + ZLRS_PAIR_SALT_LEN];
	memset(pairing, 0x37, sizeof(pairing));
	zassert_equal(nvs_write(&inspect, 1, &band, 1), 1);
	zassert_equal(nvs_write(&inspect, 2, baud, 4), 4);
	zassert_equal(nvs_write(&inspect, 3, &region, 1), 1);
	zassert_equal(nvs_write(&inspect, 4, &role, 1), 1);
	zassert_equal(nvs_write(&inspect, 5, &profile, 1), 1);
	zassert_equal(nvs_write(&inspect, 6, key, sizeof(key)), sizeof(key));
	zassert_equal(nvs_write(&inspect, 7, id, sizeof(id)), sizeof(id));
	zassert_equal(nvs_write(&inspect, 8, pairing, sizeof(pairing)), sizeof(pairing));
	zassert_ok(zlrs_persist_init());
	const zlrs_persist_settings_t *saved = zlrs_persist_settings();
	zassert_equal(saved->band, band);
	zassert_equal(saved->data_baud, 230400);
	zassert_equal(saved->role, role);
	zassert_equal(saved->region, region);
	zassert_equal(saved->profile, profile);
	uint8_t saved_key[ZLRS_KEY_LEN], saved_id[ZLRS_DEV_ID_LEN];
	zlrs_persist_pairing_t saved_pair;
	zassert_true(zlrs_persist_get_root_key(saved_key));
	zassert_true(zlrs_persist_get_device_id(saved_id));
	zassert_true(zlrs_persist_get_pairing(&saved_pair));
	zassert_mem_equal(saved_key, key, sizeof(key));
	zassert_mem_equal(saved_id, id, sizeof(id));
	zassert_mem_equal(saved_pair.peer_id, pairing, ZLRS_DEV_ID_LEN);
	zassert_mem_equal(saved_pair.pair_salt, pairing + ZLRS_DEV_ID_LEN, ZLRS_PAIR_SALT_LEN);
}

ZTEST(settings, test_settings_save_and_repeated_save_survive_remount)
{
	zlrs_persist_settings_t settings = *zlrs_persist_settings();
	settings.band = ZLRS_DATA_BAND_AUTO;
	settings.data_baud = 460800;
	zassert_ok(zlrs_persist_save_settings(&settings));
	zassert_ok(zlrs_persist_save_settings(&settings));
	zassert_ok(zlrs_persist_init());
	zassert_equal(zlrs_persist_settings()->band, settings.band);
	zassert_equal(zlrs_persist_settings()->data_baud, settings.data_baud);
	zassert_ok(nvs_mount(&inspect));
	uint8_t raw[4];
	zassert_equal(nvs_read(&inspect, 2, raw, 4), 4);
	zassert_equal(sys_get_le32(raw), 460800);
}

ZTEST(settings, test_partial_settings_save_matches_flash)
{
	const zlrs_persist_settings_t old = *zlrs_persist_settings();
	zlrs_persist_settings_t settings = old;
	settings.band = old.band == ZLRS_DATA_BAND_24 ? ZLRS_DATA_BAND_AUTO : ZLRS_DATA_BAND_24;
	settings.data_baud = old.data_baud == 460800 ? 230400 : 460800;
	fail_record = 2;
	zassert_equal(zlrs_persist_save_settings(&settings), -EIO);
	fail_record = UINT16_MAX;
	zassert_equal(zlrs_persist_settings()->band, settings.band);
	zassert_equal(zlrs_persist_settings()->data_baud, old.data_baud);
	zassert_ok(zlrs_persist_init());
	zassert_equal(zlrs_persist_settings()->band, settings.band);
	zassert_equal(zlrs_persist_settings()->data_baud, old.data_baud);
}

ZTEST(settings, test_failed_key_write_preserves_committed_key)
{
	zassert_ok(zlrs_persist_set_root_key(key));
	uint8_t other[ZLRS_KEY_LEN];
	memset(other, 0x99, sizeof(other));
	fail_write = true;
	const int rc = zlrs_persist_set_root_key(other);
	fail_write = false;
	zassert_equal(rc, -EIO);
	zassert_true(zlrs_persist_get_root_key(other));
	zassert_mem_equal(other, key, sizeof(key));
	zassert_ok(zlrs_persist_init());
	zassert_true(zlrs_persist_get_root_key(other));
	zassert_mem_equal(other, key, sizeof(key));
}

ZTEST(settings, test_failed_pair_write_preserves_committed_pair)
{
	zassert_ok(zlrs_persist_save_pairing(&pair));
	zlrs_persist_pairing_t other = pair;
	other.peer_id[0] ^= 0xff;
	fail_write = true;
	const int rc = zlrs_persist_save_pairing(&other);
	fail_write = false;
	zassert_equal(rc, -EIO);
	zassert_true(zlrs_persist_get_pairing(&other));
	zassert_mem_equal(&other, &pair, sizeof(pair));
	zassert_ok(zlrs_persist_init());
	zassert_true(zlrs_persist_get_pairing(&other));
	zassert_mem_equal(&other, &pair, sizeof(pair));
}

ZTEST(settings, test_invalid_settings_are_rejected)
{
	zlrs_persist_settings_t settings = *zlrs_persist_settings();
	settings.profile = ZLRS_PROFILE_P2;
	zassert_equal(zlrs_persist_save_settings(&settings), -EINVAL);
	settings = *zlrs_persist_settings();
	settings.region = (zlrs_region_id_t)255;
	zassert_equal(zlrs_persist_save_settings(&settings), -EINVAL);
	settings = *zlrs_persist_settings();
	settings.data_baud = 1;
	zassert_equal(zlrs_persist_save_settings(&settings), -EINVAL);
}

ZTEST(settings, test_new_identity_invalidates_pairing)
{
	zassert_ok(zlrs_persist_save_pairing(&pair));
	uint8_t old_id[ZLRS_DEV_ID_LEN], id[ZLRS_DEV_ID_LEN];
	zassert_true(zlrs_persist_get_device_id(old_id));
	zassert_ok(zlrs_persist_new_identity());
	zassert_false(zlrs_persist_get_pairing(&(zlrs_persist_pairing_t){0}));
	zassert_true(zlrs_persist_get_device_id(id));
	zassert_not_equal(memcmp(id, old_id, sizeof(id)), 0);
	zassert_ok(zlrs_persist_init());
	zassert_false(zlrs_persist_get_pairing(&(zlrs_persist_pairing_t){0}));
	zassert_ok(zlrs_persist_erase_pairing());
	zassert_ok(zlrs_persist_erase_pairing());
}

ZTEST(settings, test_failed_identity_write_keeps_old_id_with_pair_removed)
{
	zassert_ok(zlrs_persist_save_pairing(&pair));
	uint8_t old_id[ZLRS_DEV_ID_LEN], id[ZLRS_DEV_ID_LEN];
	zassert_true(zlrs_persist_get_device_id(old_id));
	fail_record = 7;
	const int rc = zlrs_persist_new_identity();
	fail_record = UINT16_MAX;
	zassert_equal(rc, -EIO);
	zassert_false(zlrs_persist_get_pairing(&(zlrs_persist_pairing_t){0}));
	zassert_true(zlrs_persist_get_device_id(id));
	zassert_mem_equal(id, old_id, sizeof(id));
	zassert_ok(zlrs_persist_init());
	zassert_false(zlrs_persist_get_pairing(&(zlrs_persist_pairing_t){0}));
	zassert_true(zlrs_persist_get_device_id(id));
	zassert_mem_equal(id, old_id, sizeof(id));
}

ZTEST(settings, test_factory_reset_and_reinitialize)
{
	zassert_ok(zlrs_persist_set_root_key(key));
	zassert_ok(zlrs_persist_save_pairing(&pair));
	zassert_ok(zlrs_persist_factory_reset());
	uint8_t saved_key[ZLRS_KEY_LEN], saved_id[ZLRS_DEV_ID_LEN];
	zassert_false(zlrs_persist_get_root_key(saved_key));
	zassert_false(zlrs_persist_get_pairing(&(zlrs_persist_pairing_t){0}));
	memset(saved_id, 0xa5, sizeof(saved_id));
	zassert_false(zlrs_persist_get_device_id(saved_id));
	const uint8_t untouched[ZLRS_DEV_ID_LEN] = {0xa5, 0xa5, 0xa5, 0xa5};
	zassert_mem_equal(saved_id, untouched, sizeof(saved_id));
	zassert_ok(zlrs_persist_init());
	zassert_true(zlrs_persist_get_device_id(saved_id));
	zassert_false(zlrs_persist_get_root_key(saved_key));
	zassert_false(zlrs_persist_get_pairing(&(zlrs_persist_pairing_t){0}));
}

static void failed_reset(enum reset_fault fault)
{
	zassert_ok(zlrs_persist_init());
	uint8_t key[ZLRS_KEY_LEN];
	memset(key, 0x42, sizeof(key));
	const zlrs_persist_pairing_t pair = {.peer_id = {9, 8, 7, 6},
					     .pair_salt = {1, 2, 3, 4, 5, 6, 7, 8}};
	zassert_ok(zlrs_persist_set_root_key(key));
	zassert_ok(zlrs_persist_save_pairing(&pair));
	reset_fault = fault;
	const int rc = zlrs_persist_factory_reset();
	reset_fault = RESET_OK;
	zassert_equal(rc, -EIO);
	/* Any destructive attempt invalidates the active identity, even if erase failed midway. */
	uint8_t id[ZLRS_DEV_ID_LEN];
	zlrs_persist_pairing_t saved;
	zassert_false(zlrs_persist_get_root_key(key));
	zassert_false(zlrs_persist_get_device_id(id));
	zassert_false(zlrs_persist_get_pairing(&saved));
	zassert_equal(zlrs_persist_set_root_key(key), -EIO);
	zassert_equal(zlrs_persist_save_settings(zlrs_persist_settings()), -EIO);
	/* A fresh mount is the recovery boundary. A failed erase may leave old flash records. */
	zassert_ok(zlrs_persist_init());
	zassert_true(zlrs_persist_get_device_id(id));
	zassert_equal(zlrs_persist_get_root_key(key), fault == CLEAR_BEFORE);
	zassert_equal(zlrs_persist_get_pairing(&saved), fault == CLEAR_BEFORE);
}

ZTEST(settings, test_reset_clear_failure)
{
	failed_reset(CLEAR_BEFORE);
}

ZTEST(settings, test_reset_partial_clear_failure)
{
	failed_reset(CLEAR_AFTER);
}

ZTEST(settings, test_reset_remount_failure)
{
	failed_reset(REMOUNT);
}

ZTEST(settings, test_reset_marker_failure)
{
	failed_reset(MARKER_WRITE);
}

ZTEST_SUITE(settings, NULL, setup, before, NULL, teardown);

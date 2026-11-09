/* SPDX-License-Identifier: Apache-2.0 */
#include "log.h"
#include <errno.h>
#include <zephyr/fs/fs.h>
#include <zephyr/init.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(zlrs_log_fs));
static bool fail_open;

int __real_flash_area_open(uint8_t id, const struct flash_area **area);

int __wrap_flash_area_open(uint8_t id, const struct flash_area **area)
{
	return fail_open ? -EIO : __real_flash_area_open(id, area);
}

/* Isolate filesystem lifecycle faults from the backend's own open file. */
static int isolate_backend(void)
{
	log_backend_disable(log_backend_get_by_name("log_backend_fs"));
	return 0;
}

SYS_INIT(isolate_backend, POST_KERNEL, 91);

ZTEST(storage_log, test_native_mount_lifecycle)
{
	struct fs_mount_t *mount = &FS_FSTAB_ENTRY(DT_NODELABEL(zlrs_log_fs));
	struct fs_statvfs stat;
	zlrs_log_info_t info;
	const struct flash_area *area;

	/* Mount must exist before the application inventory is called. */
	zassert_ok(fs_statvfs("/lfs", &stat));
	zassert_ok(fs_unmount(mount));
	zassert_ok(flash_area_open(DT_PARTITION_ID(DT_NODELABEL(storage_partition)), &area));
	zassert_ok(flash_area_flatten(area, 0, area->fa_size));
	flash_area_close(area);
	zassert_ok(zlrs_log_info(&info));
	zassert_equal(info.count, 0U);

	const char *paths[] = {"/lfs/log.0003", "/lfs/log.0001"};
	for (size_t i = 0; i < ARRAY_SIZE(paths); i++) {
		struct fs_file_t file;
		fs_file_t_init(&file);
		zassert_ok(fs_open(&file, paths[i], FS_O_CREATE | FS_O_WRITE));
		zassert_equal(fs_write(&file, "abc", i + 1U), i + 1U);
		zassert_ok(fs_close(&file));
	}
	zassert_ok(fs_unmount(mount));
	fail_open = true;
	info.count = 0xaa;
	zassert_equal(zlrs_log_info(&info), -ENODEV);
	zassert_equal(info.count, 0xaa, "failed storage must not masquerade as an empty log");
	fail_open = false;
	zassert_ok(zlrs_log_info(&info));
	zassert_equal(info.count, 2U);
	zassert_equal(info.num[0], 1U);
	zassert_equal(info.size[0], 2U);
	zassert_equal(info.num[1], 3U);
	zassert_equal(info.size[1], 1U);
}

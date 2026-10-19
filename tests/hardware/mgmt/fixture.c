/* SPDX-License-Identifier: Apache-2.0 */
/* Never confirm this image. Real commands/NVS/RF/USB run; recovery restores the NVS snapshot
 * and MCUboot restores the original firmware. Only confirmation and one write are fault-injected. */
#include <errno.h>
#include <stdbool.h>
#include <zephyr/init.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>

static uint8_t backup[PARTITION_SIZE(storage_partition)];
static bool backed_up;

static int snapshot(void)
{
	const struct flash_area *area;
	int rc = flash_area_open(PARTITION_ID(storage_partition), &area);
	if (rc == 0) {
		rc = flash_area_read(area, 0, backup, sizeof(backup));
		flash_area_close(area);
	}
	backed_up = rc == 0;
	return rc;
}

SYS_INIT(snapshot, APPLICATION, 0);

bool __wrap_boot_is_img_confirmed(void)
{
	return true;
}

int __wrap_zlrs_boot_confirm(void)
{
	static bool once;
	if (!once) {
		once = true;
		return -EIO;
	}
	return 0; /* Deliberately never writes image_ok. */
}

ssize_t __real_nvs_write(struct nvs_fs *, uint16_t, const void *, size_t);

ssize_t __wrap_nvs_write(struct nvs_fs *fs, uint16_t id, const void *bytes, size_t len)
{
	static bool once;
	if (id == 4 && len == 1 && !once) {
		once = true;
		return -EIO;
	}
	return __real_nvs_write(fs, id, bytes, len);
}

int __real_zlrs_boot_recovery(void);

int __wrap_zlrs_boot_recovery(void)
{
	if (!backed_up) {
		return -EIO;
	}
	const struct flash_area *area;
	int rc = flash_area_open(PARTITION_ID(storage_partition), &area);
	if (rc == 0) {
		rc = flash_area_flatten(area, 0, sizeof(backup));
		if (rc == 0) {
			rc = flash_area_write(area, 0, backup, sizeof(backup));
		}
		flash_area_close(area);
	}
	return rc == 0 ? __real_zlrs_boot_recovery() : rc;
}

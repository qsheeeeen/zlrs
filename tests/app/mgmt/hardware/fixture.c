/* SPDX-License-Identifier: Apache-2.0 */
/* Never confirm this image. Real commands/NVS/RF/USB run; recovery restores the NVS snapshot
 * and MCUboot restores the original firmware. Only confirmation and one write are fault-injected.
 */
#include <bootutil/bootutil_public.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/settings/settings.h>
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

void __wrap_zlrs_lifecycle_check_rollback(bool fault, uint32_t timeout_ms)
{
	ARG_UNUSED(fault);
	ARG_UNUSED(
		timeout_ms); /* Keep the test image unconfirmed and reachable until restoration. */
}

int __wrap_boot_set_next(const struct flash_area *area, bool active, bool confirm)
{
	ARG_UNUSED(area);
	if (!active || !confirm) {
		return BOOT_EBADIMAGE;
	}
	static bool once;
	if (!once) {
		once = true;
		return BOOT_EFLASH;
	}
	return 0; /* Deliberately never writes image_ok. */
}

int __real_settings_save_one(const char *, const void *, size_t);

int __wrap_settings_save_one(const char *name, const void *bytes, size_t len)
{
	static bool once;
	if (strcmp(name, "zlrs/role") == 0 && len == 1 && !once) {
		once = true;
		return -EIO;
	}
	return __real_settings_save_one(name, bytes, len);
}

int __real_bootmode_set(uint8_t mode);

int __wrap_bootmode_set(uint8_t mode)
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
	return rc == 0 ? __real_bootmode_set(mode) : rc;
}

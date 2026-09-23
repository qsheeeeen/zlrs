/*
 * Log storage: mount the LittleFS the logging subsystem's file backend writes to, and report how
 * the last boot ended. See doc/15-log.md.
 *
 * Nothing here implements logging: the messages themselves go through Zephyr's logging subsystem
 * and its file backend (LOG_BACKEND_FS), which also owns the file rotation that makes the store a
 * ring — when the file cap is reached the oldest log file is overwritten. LittleFS owns power-fail
 * safety and wear leveling on the partition. The only thing that has to happen in the application
 * is the mount, and it has to happen before the log core enables its backends, hence the init
 * priorities below (log core: CONFIG_LOG_CORE_INIT_PRIORITY, raised in prj.conf to run last).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

/*
 * Only boards whose partition table has a log partition get this: the whole file is about that
 * partition, and one without it keeps logging to the console only.
 */
#if !DT_NODE_EXISTS(DT_NODELABEL(log_partition))
LOG_MODULE_REGISTER(zlrs_log_store, CONFIG_APP_LOG_LEVEL);

void zlrs_log_store_absent(void)
{
	LOG_WRN("no log partition on this board, logs are not kept across reboots");
}

int zlrs_log_store_probe(void)
{
	return -ENODEV;
}
#else

LOG_MODULE_REGISTER(zlrs_log_store, CONFIG_APP_LOG_LEVEL);

#define LOG_MOUNT_POINT "/lfs"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(zlrs_log_fs_cfg);

static struct fs_mount_t log_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &zlrs_log_fs_cfg,
	.storage_dev = (void *)FIXED_PARTITION_ID(log_partition),
	.mnt_point = LOG_MOUNT_POINT,
};

static bool log_mounted;

static int log_store_mount(void);

/*
 * Bench entry: run the mount again on demand and report what it returned, so a board with no
 * mounted log store can say why without another reboot (main.c: `zlrs logstore`).
 */
int zlrs_log_store_probe(void)
{
	int rc;

	if (log_mounted) {
		return 0;
	}

	rc = log_store_mount();

	return (int)rc;
}

/* Mount, and on a partition that has never held a filesystem, erase it and mount again */
static int log_store_mount(void)
{
	int rc = fs_mount(&log_mnt);

	if (rc != 0) {
		/*
		 * Never-formatted or corrupt: erase the whole area and mount again, which is the littlefs
		 * recipe from samples/subsys/fs/littlefs. Erasing rather than fs_mkfs() also means a
		 * device whose log store got damaged recovers on its own instead of losing the feature.
		 */
		const struct flash_area *area;

		LOG_WRN("log filesystem unreadable (%d), erasing the partition", rc);
		if (flash_area_open(FIXED_PARTITION_ID(log_partition), &area) == 0) {
			(void)flash_area_flatten(area, 0, area->fa_size);
			flash_area_close(area);
			rc = fs_mount(&log_mnt);
		} else {
			LOG_ERR("cannot open the log partition to erase it");
		}
	}

	if (rc != 0) {
		LOG_ERR("log filesystem mount failed (%d), logs stay on the console only", rc);
		return rc;
	}

	log_mounted = true;

	return 0;
}

/*
 * Runs before the log core starts its backends (POST_KERNEL, and the core is pushed to 90 in
 * prj.conf): the file backend opens its log file when it is enabled, and a file system that is not
 * mounted yet would leave it writing nowhere for the rest of the boot.
 */
static int log_store_init(void)
{
	(void)log_store_mount();

	/* A failed mount must not stop the boot: the rest of the device works, only logs stay local */
	return 0;
}

SYS_INIT(log_store_init, POST_KERNEL, 50);

/* Log core is up by now, so this report lands in the log and therefore in the log file as well */
static int log_store_report(void)
{
	struct fs_statvfs stat = { 0 };

	if (log_mounted && fs_statvfs(LOG_MOUNT_POINT, &stat) == 0) {
		LOG_INF("log store: %s, %u KiB of %u KiB free", LOG_MOUNT_POINT,
			(unsigned int)((stat.f_bfree * stat.f_frsize) / 1024U),
			(unsigned int)((stat.f_blocks * stat.f_frsize) / 1024U));
	} else {
		LOG_WRN("log store: no file system, logs are not kept across reboots");
	}

#if defined(CONFIG_HWINFO)
	{
		uint32_t cause = 0U;

		if (hwinfo_get_reset_cause(&cause) == 0) {
			/*
			 * One line per boot: with the log files in flash this is what says whether the
			 * device restarted because someone asked it to or because something went wrong.
			 */
			LOG_INF("boot: reset cause%s%s%s%s%s",
				((cause & RESET_POR) != 0U) ? " power-on" : "",
				((cause & RESET_SOFTWARE) != 0U) ? " software" : "",
				((cause & RESET_WATCHDOG) != 0U) ? " watchdog" : "",
				((cause & RESET_PIN) != 0U) ? " pin" : "",
				((cause & RESET_DEBUG) != 0U) ? " debug" : "");
			(void)hwinfo_clear_reset_cause();
		}
	}
#endif

	return 0;
}

SYS_INIT(log_store_report, POST_KERNEL, 99);

#endif /* log_partition */

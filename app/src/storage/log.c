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

#include <stdio.h>
#include <string.h>

#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

#include "zlrs/mgmt/mgmt.h"

/*
 * Only boards whose partition table has a log partition get this: the whole file is about that
 * partition, and one without it keeps logging to the console only.
 */
#if !DT_NODE_EXISTS(DT_NODELABEL(log_partition))
LOG_MODULE_REGISTER(zlrs_storage_log, CONFIG_APP_LOG_LEVEL);

void zlrs_storage_log_absent(void)
{
	LOG_WRN("no log partition on this board, logs are not kept across reboots");
}

int zlrs_storage_log_probe(void)
{
	return -ENODEV;
}

int zlrs_storage_log_info(zlrs_mgmt_log_info_t *out)
{
	ARG_UNUSED(out);

	return ZLRS_MGMT_E_STATE;
}

int zlrs_storage_log_read(uint8_t index, uint32_t offset, uint8_t len, uint8_t *out, uint8_t *out_len)
{
	ARG_UNUSED(index);
	ARG_UNUSED(offset);
	ARG_UNUSED(len);
	ARG_UNUSED(out);
	ARG_UNUSED(out_len);

	return ZLRS_MGMT_E_STATE;
}
#else

LOG_MODULE_REGISTER(zlrs_storage_log, CONFIG_APP_LOG_LEVEL);

#define LOG_MOUNT_POINT "/lfs"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(zlrs_log_fs_cfg);

static struct fs_mount_t log_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &zlrs_log_fs_cfg,
	.storage_dev = (void *)PARTITION_ID(log_partition),
	.mnt_point = LOG_MOUNT_POINT,
};

static bool log_mounted;

static int log_store_mount(void);

/* The ring, smallest file number first: that is the order the host exports them in */
struct log_file {
	uint16_t num;
	uint32_t size;
};

static struct log_file log_files[ZLRS_MGMT_LOG_FILES_MAX];
static uint8_t log_file_count;

/* List the store, smallest file number first: the backend names files `<prefix><0000>` upward and
 * overwrites the oldest, so numeric order is the order they were written in. */
static int log_store_scan(void)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	uint8_t found = 0U;
	int rc;

	log_file_count = 0U;

	if (!log_mounted) {
		rc = log_store_mount();
		if (rc != 0) {
			return ZLRS_MGMT_E_STATE;
		}
	}

	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, LOG_MOUNT_POINT);
	if (rc != 0) {
		return ZLRS_MGMT_E_STATE;
	}

	for (;;) {
		rc = fs_readdir(&dir, &ent);
		if (rc != 0 || ent.name[0] == '\0') {
			break;
		}
		if (ent.type != FS_DIR_ENTRY_FILE) {
			continue;
		}

		/* The backend's names are the prefix plus four digits; anything else is not ours */
		const char *digits = ent.name + strlen(CONFIG_LOG_BACKEND_FS_FILE_PREFIX);
		uint16_t num = 0U;
		uint8_t n = 0U;

		if (strncmp(ent.name, CONFIG_LOG_BACKEND_FS_FILE_PREFIX,
			    strlen(CONFIG_LOG_BACKEND_FS_FILE_PREFIX)) != 0) {
			continue;
		}
		while (digits[n] >= '0' && digits[n] <= '9' && n < 5U) {
			num = (uint16_t)((num * 10U) + (uint16_t)(digits[n] - '0'));
			n++;
		}
		if (n == 0U) {
			continue;
		}

		if (found < ZLRS_MGMT_LOG_FILES_MAX) {
			/* Insert sorted: at most a handful of files, an insertion sort is plenty */
			uint8_t at = found;

			while (at > 0U && log_files[at - 1U].num > num) {
				log_files[at] = log_files[at - 1U];
				at--;
			}
			log_files[at].num = num;
			log_files[at].size = (uint32_t)ent.size;
			found++;
		}
	}

	(void)fs_closedir(&dir);
	log_file_count = found;

	/*
	 * One line per scan: an export that finds nothing is a different bug from one that finds the
	 * files but cannot read them, and this is the line that tells the two apart on a serial console.
	 */
	if (found == 0U) {
		LOG_WRN("log store: no files under %s", LOG_MOUNT_POINT);
	} else {
		LOG_INF("log store: %u file(s), newest log.%04u", found, log_files[found - 1U].num);
	}

	return ZLRS_MGMT_OK;
}

/* What the export needs: how many files, which is newest, and each size. No store on this board is
 * E_STATE, not an empty list — "nothing logged yet" and "no store here" are different answers. */
int zlrs_storage_log_info(zlrs_mgmt_log_info_t *out)
{
	int rc;

	if (out == NULL) {
		return ZLRS_MGMT_E_PARAM;
	}

	rc = log_store_scan();
	if (rc != ZLRS_MGMT_OK) {
		return rc;
	}

	memset(out, 0, sizeof(*out));
	out->count = log_file_count;
	out->newest = (log_file_count > 0U) ? (uint8_t)(log_file_count - 1U) : 0U;

	for (uint8_t i = 0U; i < log_file_count; i++) {
		out->num[i] = log_files[i].num;
		out->size[i] = log_files[i].size;
	}

	return ZLRS_MGMT_OK;
}

/* One chunk of one file. The caller (management protocol) bounds len; a short tail returns short. */
int zlrs_storage_log_read(uint8_t index, uint32_t offset, uint8_t len, uint8_t *out, uint8_t *out_len)
{
	char path[sizeof(LOG_MOUNT_POINT) + 16U];
	struct fs_file_t file;
	ssize_t got;
	int rc;

	if (out == NULL || out_len == NULL || index >= log_file_count || len == 0U) {
		return ZLRS_MGMT_E_PARAM;
	}

	(void)snprintf(path, sizeof(path), "%s/%s%04u", LOG_MOUNT_POINT,
		       CONFIG_LOG_BACKEND_FS_FILE_PREFIX, log_files[index].num);

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc != 0) {
		return ZLRS_MGMT_E_STATE;
	}

	rc = fs_seek(&file, (off_t)offset, FS_SEEK_SET);
	if (rc != 0) {
		(void)fs_close(&file);
		return ZLRS_MGMT_E_PARAM;
	}

	got = fs_read(&file, out, len);
	(void)fs_close(&file);
	if (got < 0) {
		return ZLRS_MGMT_E_STATE;
	}

	*out_len = (uint8_t)got;

	return ZLRS_MGMT_OK;
}

/*
 * Bench entry: run the mount again on demand and report what it returned, so a board with no
 * mounted log store can say why without another reboot (main.c: `zlrs logstore`).
 */
int zlrs_storage_log_probe(void)
{
	int rc;

	if (log_mounted) {
		return 0;
	}

	rc = log_store_mount();

	return (int)rc;
}

/* Retry before erasing: a reset during a write leaves LittleFS work to do on the next mount, and
 * that must not cost the log of the boot that went wrong. */
static int log_store_mount(void)
{
	int rc = -EIO;

	for (uint8_t attempt = 0U; attempt < 3U; attempt++) {
		rc = fs_mount(&log_mnt);
		if (rc == 0) {
			log_mounted = true;
			return 0;
		}

		LOG_WRN("log filesystem mount failed (%d), attempt %u", rc, attempt + 1U);
		k_msleep(50);
	}

	if (rc != 0) {
		/*
		 * Never-formatted or corrupt: erase the whole area and mount again, which is the littlefs
		 * recipe from samples/subsys/fs/littlefs. Erasing rather than fs_mkfs() also means a
		 * device whose log store got damaged recovers on its own instead of losing the feature.
		 */
		const struct flash_area *area;

		LOG_WRN("log filesystem unreadable (%d), erasing the partition", rc);
		if (flash_area_open(PARTITION_ID(log_partition), &area) == 0) {
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
 * After LittleFS registers itself (CONFIG_FILE_SYSTEM_INIT_PRIORITY, 99): mounting earlier fails
 * with "file system type not found" and the file backend then skips every record.
 */
BUILD_ASSERT(CONFIG_FILE_SYSTEM_INIT_PRIORITY < 100,
	     "the log store mounts at 100: raise it if the file system layer moves later");

static int log_store_init(void)
{
	(void)log_store_mount();

	/* A failed mount must not stop the boot: the rest of the device works, only logs stay local */
	return 0;
}

SYS_INIT(log_store_init, POST_KERNEL, 100);

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

SYS_INIT(log_store_report, POST_KERNEL, 101);

#endif /* log_partition */

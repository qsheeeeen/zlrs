/* SPDX-License-Identifier: Apache-2.0 */
/* Zephyr owns the log filesystem mount and file rotation. This module inventories
 * saved files and reports reset information after filesystem initialization. */

#include <zephyr/drivers/uart.h>
#include <zephyr/fs/fs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>

#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

#include "log.h"
#include <errno.h>

LOG_MODULE_REGISTER(zlrs_log, CONFIG_APP_LOG_LEVEL);

#if defined(CONFIG_LOG_BACKEND_UART)
void zlrs_log_console_sync(void)
{
	const struct log_backend *backend = log_backend_get_by_name("log_backend_uart");
	bool connected = true;

#if DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart)
	uint32_t dtr = 0U;
	const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	connected = uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr != 0U;
#endif
	if (backend != NULL && connected != log_backend_is_active(backend)) {
		if (connected) {
			log_backend_enable(backend, backend->cb->ctx, CONFIG_LOG_MAX_LEVEL);
		} else {
			log_backend_disable(backend);
		}
	}
}
#endif

/* The native file backend numbers files with exactly four decimal digits. */
int zlrs_log_file_id(const char *name)
{
	const size_t prefix = sizeof(CONFIG_LOG_BACKEND_FS_FILE_PREFIX) - 1U;
	if (strncmp(name, CONFIG_LOG_BACKEND_FS_FILE_PREFIX, prefix) != 0) {
		return -EINVAL;
	}
	const char *digits = name + prefix;
	if (strlen(digits) != 4U || strspn(digits, "0123456789") != 4U) {
		return -EINVAL;
	}
	return (int)strtoul(digits, NULL, 10);
}

/*
 * A board fstab entry owns log storage; without one, logging stays on the console.
 */
#if !DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(zlrs_log_fs))

int zlrs_log_info(zlrs_log_info_t *out)
{
	ARG_UNUSED(out);

	return -ENODEV;
}

#else

BUILD_ASSERT(IS_ENABLED(CONFIG_FS_LITTLEFS_FSTAB_AUTOMOUNT),
	     "log storage requires the board's native filesystem automount");
FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(zlrs_log_fs));

/* List the backend files by numeric ID. */
int zlrs_log_info(zlrs_log_info_t *out)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	zlrs_log_info_t info = {0};
	int rc;

	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, CONFIG_LOG_BACKEND_FS_DIR);
	if (rc != 0) {
		/* Retry a failed boot mount at an explicit storage boundary. */
		const int mount_rc = fs_mount(&FS_FSTAB_ENTRY(DT_NODELABEL(zlrs_log_fs)));
		if (mount_rc != 0) {
			return mount_rc == -EBUSY ? rc : mount_rc;
		}
		rc = fs_opendir(&dir, CONFIG_LOG_BACKEND_FS_DIR);
		if (rc != 0) {
			return rc;
		}
	}

	for (;;) {
		rc = fs_readdir(&dir, &ent);
		if (rc != 0 || ent.name[0] == '\0') {
			break;
		}
		if (ent.type != FS_DIR_ENTRY_FILE) {
			continue;
		}

		const int num = zlrs_log_file_id(ent.name);
		if (num < 0) {
			continue;
		}

		if (info.count < CONFIG_LOG_BACKEND_FS_FILES_LIMIT) {
			/* Insert sorted: at most a handful of files, an insertion sort is plenty */
			uint16_t at = info.count;

			while (at > 0U && info.num[at - 1U] > num) {
				info.num[at] = info.num[at - 1U];
				info.size[at] = info.size[at - 1U];
				at--;
			}
			info.num[at] = num;
			info.size[at] = (uint32_t)ent.size;
			info.count++;
		}
	}

	const int close_rc = fs_closedir(&dir);
	if (rc != 0 || close_rc != 0) {
		return rc != 0 ? rc : close_rc;
	}
	*out = info;

	/*
	 * One line per scan: an export that finds nothing is a different bug from one that finds the
	 * files but cannot read them, and this is the line that tells the two apart on a serial console.
	 */
	if (info.count == 0U) {
		LOG_WRN("log store: no files under %s", CONFIG_LOG_BACKEND_FS_DIR);
	} else {
		LOG_INF("log store: %u file(s), newest log.%04u", info.count,
			info.num[info.count - 1U]);
	}

	return 0;
}

BUILD_ASSERT(CONFIG_FILE_SYSTEM_INIT_PRIORITY < 101,
	     "the log report must run after native filesystem initialization");

/* Log core is up by now, so this report lands in the log and therefore in the log file as well */
static int log_store_report(void)
{
	struct fs_statvfs stat = {0};

	if (fs_statvfs(CONFIG_LOG_BACKEND_FS_DIR, &stat) == 0) {
		LOG_INF("log store: %s, %u KiB of %u KiB free", CONFIG_LOG_BACKEND_FS_DIR,
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

#endif /* zlrs_log_fs */

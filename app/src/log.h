/*
 * Log store: the flash ring the file logging backend writes to (app/src/log.c,
 * doc/06-firmware.md). Used by management log export.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

/* Apply the native UART backend's host-readiness policy before processing logs. */
#if defined(CONFIG_LOG_BACKEND_UART)
void zlrs_log_console_sync(void);
#else
static inline void zlrs_log_console_sync(void)
{
}
#endif

typedef struct {
	uint16_t count;
	uint16_t num[CONFIG_LOG_BACKEND_FS_FILES_LIMIT];
	uint32_t size[CONFIG_LOG_BACKEND_FS_FILES_LIMIT];
} zlrs_log_info_t;

/* Parse one native backend filename, or return negative errno. */
int zlrs_log_file_id(const char *name);

/* What the store holds: file count and per-file size, sorted by file number */
int zlrs_log_info(zlrs_log_info_t *out);

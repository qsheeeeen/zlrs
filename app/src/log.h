/*
 * Log store: the flash ring the file logging backend writes to (app/src/log.c,
 * doc/06-firmware.md). Used by management log export.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#define ZLRS_LOG_FILES_MAX 8U

/* Apply the native UART backend's host-readiness policy before processing logs. */
#if defined(CONFIG_LOG_BACKEND_UART)
void zlrs_log_console_sync(void);
#else
static inline void zlrs_log_console_sync(void) {}
#endif

typedef struct {
	uint8_t count;
	uint16_t num[ZLRS_LOG_FILES_MAX];
	uint32_t size[ZLRS_LOG_FILES_MAX];
} zlrs_log_info_t;

/* What the store holds: file count and per-file size, sorted by file number */
int zlrs_log_info(zlrs_log_info_t *out);

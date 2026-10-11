/*
 * Log store: the flash ring the file logging backend writes to (app/src/log.c,
 * doc/07-firmware.md). Used by management log export.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#define ZLRS_LOG_FILES_MAX 8U

typedef struct {
	uint8_t count;
	uint8_t newest;
	uint16_t num[ZLRS_LOG_FILES_MAX];
	uint32_t size[ZLRS_LOG_FILES_MAX];
} zlrs_log_info_t;

/* What the store holds: file count, newest index and per-file size */
int zlrs_log_info(zlrs_log_info_t *out);

/* One chunk of one file, oldest first by index; short reads at the end of a file are normal */
int zlrs_log_read(uint8_t index, uint32_t offset, uint8_t len, uint8_t *out, uint8_t *out_len);

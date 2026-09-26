/*
 * Log store: the flash ring the file logging backend writes to (app/src/storage/log.c,
 * doc/08-firmware.md 7). Used by the shell's `zlrs logstore` and the config protocol's export.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "zlrs/config/config.h"

/* Mount on demand and report the error code; 0 when the store is usable */
int zlrs_storage_log_probe(void);

/* What the store holds: file count, newest index and per-file size */
int zlrs_storage_log_info(zlrs_cfg_log_info_t *out);

/* One chunk of one file, oldest first by index; short reads at the end of a file are normal */
int zlrs_storage_log_read(uint8_t index, uint32_t offset, uint8_t len, uint8_t *out, uint8_t *out_len);

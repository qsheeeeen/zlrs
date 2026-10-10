/*
 * Log store: the flash ring the file logging backend writes to (app/src/storage/log.c,
 * doc/07-firmware.md). Used by management log export.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "zlrs/mgmt/mgmt.h"

/* What the store holds: file count, newest index and per-file size */
int zlrs_storage_log_info(zlrs_mgmt_log_info_t *out);

/* One chunk of one file, oldest first by index; short reads at the end of a file are normal */
int zlrs_storage_log_read(uint8_t index, uint32_t offset, uint8_t len, uint8_t *out,
			  uint8_t *out_len);

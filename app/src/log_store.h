/*
 * Log store: the flash ring the logging subsystem's file backend writes to
 * (app/src/log_store.c, doc/08-firmware.md 7).
 *
 * The application uses it in two places: the shell's `zlrs logstore` (mount on demand, for bench)
 * and the config protocol's log export (what the host dumps to a directory).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "zlrs/config/config.h"

/* Mount on demand and report the error code; 0 when the store is usable */
int zlrs_log_store_probe(void);

/* What the store holds: file count, newest index and per-file size */
int zlrs_log_store_info(zlrs_cfg_log_info_t *out);

/* One chunk of one file, oldest first by index; short reads at the end of a file are normal */
int zlrs_log_store_read(uint8_t index, uint32_t offset, uint8_t len, uint8_t *out, uint8_t *out_len);

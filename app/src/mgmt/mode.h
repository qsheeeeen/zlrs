/*
 * Management mode: the session the management protocol runs in, and the device commands it carries.
 *
 * The host enters it through the dedicated management CDC; the protocol — framing, command
 * dispatch — is core/mgmt and pure C, so every command lands here as a callback. This file is
 * where a command becomes a device action: an NVS write, a pairing, a reboot into recovery
 * (doc/07-firmware.md 2, doc/09-security.md 9).
 *
 * The session works on the application's live state, handed in as a context instead of an
 * indirection table: the commands persist values (they take effect on the next boot), so the
 * context carries copies of what is live now plus the buffers a few commands write into.
 *
 * Naming: `<dir>/<file>.c` carries `zlrs_<dir>_<file>_*()`, so this is `zlrs_mgmt_mode_*()`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/link/link.h"

#if defined(CONFIG_ZLRS_APP_DEBUG)
typedef int (*zlrs_mgmt_debug_report_fn)(uint8_t report_id, uint16_t offset, uint8_t *out,
					 uint8_t capacity, uint16_t *total_len, uint8_t *written);
#endif

/* What the commands act on: the application's live state, handed in when a session opens */
typedef struct {
	zlrs_link_t *control; /* the link the commands act on (keys, pairing, failsafe) */
	zlrs_link_t *data;    /* a fresh device id goes to both links */
	zlrs_link_t *active;  /* pairing and status use this link */
	uint8_t mode;         /* live ZLRS_MODE_*; writes take effect after reboot */
	uint8_t *failsafe;    /* the application's failsafe payload (ZLRS_FAILSAFE_LEN bytes) */
	uint8_t *device_id;   /* the identity, replaced by NEW_DEV_ID (ZLRS_DEV_ID_LEN bytes) */
	uint8_t region_id;    /* the live region: a command persists, it does not switch */
	int8_t tx_power_dbm; /* the live TX power: what a region or power command is checked against */
#if defined(CONFIG_ZLRS_APP_DEBUG)
	zlrs_mgmt_debug_report_fn debug_report;
#endif
} zlrs_mgmt_ctx_t;

/*
 * Run a blocking management session until the idle timeout or an EXIT command.
 * A pending host ENTER is acknowledged after session setup and synchronous log flushing.
 * Returns 0 = EXIT seen, 1 = timeout, -1 = this board has no management port.
 */
int zlrs_mgmt_mode_run(const zlrs_mgmt_ctx_t *ctx);

/*
 * The host confirmed this image (see the upgrade flow in doc/07-firmware.md 6).
 * It is the only verdict on whether this image stays: without a confirm after an upgrade,
 * the next reset lets MCUboot roll back.
 */
bool zlrs_mgmt_mode_image_confirmed(void);

/* May this image run that profile? P0 is a debug-image move, P2 is not implemented (07-firmware 4) */
bool zlrs_mgmt_mode_profile_allowed(uint8_t profile);

/* Dedicated management CDC, initialized and polled directly by the application. */
int zlrs_mgmt_init(void);
/* Detect ENTER without acknowledging it; mode_run() replies when the session is ready. */
bool zlrs_mgmt_poll_enter(void);

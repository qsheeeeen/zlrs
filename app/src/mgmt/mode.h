/*
 * Management mode: the session the management protocol runs in, and the device commands it carries.
 *
 * The host enters it through the dedicated management CDC; the protocol — framing, command
 * dispatch — is core/mgmt and pure C, so every command lands here as a callback. This file is
 * where a command becomes a device action: an NVS write, a pairing, a reboot into recovery
 * (doc/07-firmware.md 2, doc/09-security.md 9).
 *
 * Call on the RF owner after it grants a maintenance window. Configuration validates and
 * persists settings; RF applies device actions. This frontend owns the session and replies.
 *
 * Naming: `<dir>/<file>.c` carries `zlrs_<dir>_<file>_*()`, so this is `zlrs_mgmt_mode_*()`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/zbus/zbus.h>

struct zlrs_mode {
	bool session;
};
ZBUS_CHAN_DECLARE(zlrs_mode_chan);

/*
 * Run a blocking management session until the idle timeout or an EXIT command.
 * A pending host ENTER is acknowledged after session setup and synchronous log flushing.
 * Returns 0 = EXIT seen, 1 = timeout, -1 = this board has no management port.
 */
int zlrs_mgmt_mode_run(void);

/*
 * The host confirmed this image (see the upgrade flow in doc/07-firmware.md 6).
 * It is the only verdict on whether this image stays: without a confirm after an upgrade,
 * the next reset lets MCUboot roll back.
 */
bool zlrs_mgmt_mode_image_confirmed(void);

/* Dedicated management CDC, initialized and polled directly by the application. */
int zlrs_mgmt_init(void);
/* Detect ENTER without acknowledging it; mode_run() replies when the session is ready. */
bool zlrs_mgmt_poll_enter(void);
/* May be called by a shell thread; the RF owner consumes the request in poll_enter(). */
void zlrs_mgmt_request_session(void);

/*
 * Management port: the dedicated CDC the management protocol runs on (doc/08-firmware.md 1).
 *
 * The port is what the main loop polls outside a session (an enter command is how a session
 * starts) and what the session reads and writes inside one (mgmt/mode.c). The protocol itself —
 * framing, commands — is core/mgmt and pure C; nothing here knows about it beyond the one
 * command that starts a session.
 *
 * Naming: `<dir>/<file>.c` carries `zlrs_<dir>_<file>_*()`, so this is `zlrs_mgmt_port_*()`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* One read out of the RX ring: a session drains the port in chunks of this size */
#define ZLRS_MGMT_CHUNK 32U

/* Look the port up; -1 when this board has no management CDC, later calls stay safe */
int zlrs_mgmt_port_init(void);

/* Is the port usable (false when the alias is missing or the device is not ready) */
bool zlrs_mgmt_port_ready(void);

/*
 * Poll the port from the main loop without blocking: an enter-management-mode command returns true
 * (the ACK is out), anything else answers E_STATE.
 */
bool zlrs_mgmt_port_poll_enter(void);

/* Session side (mgmt/mode.c): move bytes over the port, 0 when it is not up */
uint16_t zlrs_mgmt_port_read(uint8_t *buf, uint16_t len);
uint16_t zlrs_mgmt_port_write(const uint8_t *buf, uint16_t len);

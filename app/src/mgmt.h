/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
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
int zlrs_mgmt_run(void);

/* Dedicated management CDC, initialized and polled directly by the application. */
int zlrs_mgmt_init(void);
/* Detect ENTER without acknowledging it; run() replies when the session is ready. */
bool zlrs_mgmt_poll_enter(void);
/* May be called by a shell thread; the RF owner consumes the request in poll_enter(). */
void zlrs_mgmt_request_session(void);

/* Execute a parsed command on the owner thread in a maintenance window. */
uint8_t zlrs_mgmt_command(uint8_t cmd, const uint8_t *payload, uint8_t len, uint8_t *out,
			  size_t cap);

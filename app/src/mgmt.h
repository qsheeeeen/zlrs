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

/* Dedicated management CDC, serviced directly by the application owner. */
int zlrs_mgmt_init(void);
/* Process idle CDC input, including one online query per frame, and session requests;
 * true asks the owner to run a session.
 * run() acknowledges host ENTER when ready; RECOVERY may reboot here. */
bool zlrs_mgmt_process_idle(void);
/* May be called by a shell thread; the application owner consumes it in process_idle(). */
void zlrs_mgmt_request_session(void);

/* Execute a parsed command on the owner thread in a maintenance window. */
uint8_t zlrs_mgmt_command(uint8_t cmd, const uint8_t *payload, uint8_t len, uint8_t *out,
			  size_t cap);

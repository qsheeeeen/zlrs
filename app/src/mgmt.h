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

/* Native SMP worker hands product operations to the RF owner at a frame boundary.
 * ENTER completes only after the maintenance window and log flush are ready. */
bool zlrs_mgmt_process_idle(void);
int zlrs_mgmt_run(void);

#include "mgmt_types.h"
#include "log.h"

struct zlrs_mgmt_reply {
	zlrs_mgmt_status_t status;
	uint32_t uptime_ms;
	uint32_t hardware_ms;
	const char *report;
	zlrs_log_info_t logs;
};

/* Synchronous, serialized request; call from a preemptible service thread. */
int zlrs_mgmt_request(uint32_t op, const uint8_t *value, size_t len, struct zlrs_mgmt_reply *reply);

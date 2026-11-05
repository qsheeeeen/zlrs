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

/* host -> device */
#define ZLRS_MGMT_CMD_ENTER         0x00U /* enter management mode (the formal entry) */
#define ZLRS_MGMT_CMD_STATUS        0x01U /* read status */
#define ZLRS_MGMT_CMD_PAIR          0x02U /* start pairing */
#define ZLRS_MGMT_CMD_ERASE_PAIR    0x03U /* erase the pairing record */
#define ZLRS_MGMT_CMD_SET_KEY_HEX   0x04U /* root key as 64 hex characters */
#define ZLRS_MGMT_CMD_SET_PHRASE    0x05U /* passphrase, derived into a root key on the device */
#define ZLRS_MGMT_CMD_SET_BAUD      0x08U /* 4-byte little-endian baud rate */
#define ZLRS_MGMT_CMD_SET_REGION    0x09U /* 1-byte region id (0..3) */
#define ZLRS_MGMT_CMD_SET_ROLE      0x0AU /* 1-byte role (0 = Leader, 1 = Follower) */
#define ZLRS_MGMT_CMD_SET_PROFILE   0x0BU /* 1-byte profile (0 = P0, 1 = P1, 2 = P2) */
#define ZLRS_MGMT_CMD_SET_BAND      0x0CU /* 0 = 2.4 GHz, 1 = sub-G, 2 = auto; next boot */
#define ZLRS_MGMT_CMD_NEW_DEV_ID    0x0DU /* make a fresh device id (drops the pairing record) */
#define ZLRS_MGMT_CMD_EXIT          0x0EU /* leave management mode */
#define ZLRS_MGMT_CMD_FACTORY_RESET 0x0FU /* wipe NVS */
#define ZLRS_MGMT_CMD_LOG_INFO      0x12U /* what the log store holds */
#define ZLRS_MGMT_CMD_DEBUG_REPORT  0x14U /* debug image only: one report id byte */
#define ZLRS_MGMT_CMD_OBSERVE       0x15U /* timestamped status without pausing RF */

#include "log.h"
#include "rf.h"

struct zlrs_mgmt_reply {
	zlrs_rf_status_t status;
	uint32_t uptime_ms;
	uint32_t data_baud;
	zlrs_region_id_t region;
	const char *report;
	zlrs_log_info_t logs;
};

/* Synchronous, serialized request; call from a preemptible service thread. */
int zlrs_mgmt_request(uint32_t op, const uint8_t *value, size_t len, struct zlrs_mgmt_reply *reply);

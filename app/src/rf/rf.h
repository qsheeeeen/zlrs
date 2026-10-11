/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/zbus/zbus.h>

#include "zlrs/proto/limits.h"
#include "zlrs/link/state.h"
#include "persist.h"

typedef struct {
	zlrs_role_t role;
	zlrs_profile_t profile;
	zlrs_data_band_t data_band;
	zlrs_region_id_t region;
	zlrs_link_state_t state;
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	bool configured;
	bool paired;
	bool session_valid;
	bool paused;
	bool link_lost;
	bool fault;
	uint32_t data_drop_bytes;
	uint8_t lq_percent;
	int8_t tx_power_dbm;
	int16_t rssi_dbm;
	int8_t snr_db;
	uint32_t frame_period_us;
} zlrs_rf_status_t;

/* Public facts only: no keys or mutable link pointers. */
ZBUS_CHAN_DECLARE(zlrs_rf_chan);

/* These functions execute on the RF owner thread, including frontend callbacks. */
/* Boot once: 0 only when RF is ready, negative errno on failure. */
int zlrs_rf_init(const zlrs_persist_state_t *saved);
/* Start once after successful init. Each frame waits for its absolute boundary and returns. */
void zlrs_rf_start(void);
void zlrs_rf_frame(void);
/* Also publishes an initialization fault while main keeps management reachable. */
void zlrs_rf_publish_state(void);
bool zlrs_rf_pairing(uint8_t peer_id[ZLRS_DEV_ID_LEN], uint8_t pair_salt[ZLRS_PAIR_SALT_LEN]);
zlrs_rf_status_t zlrs_rf_status(void);
int zlrs_rf_pair_start(void);
int zlrs_rf_pair_cancel(void);
/* Apply credentials in a maintenance window; ordinary settings wait for reboot. */
void zlrs_rf_credentials(const zlrs_persist_state_t *saved);
/* Safe boundary: cancel radio launch and invalidate preloaded data. */
void zlrs_rf_pause(void);
void zlrs_rf_resume(void);
/* Captured only while paused at a management boundary. */
void zlrs_rf_log_snapshot(void);
void zlrs_rf_management_return(void);
#if defined(CONFIG_ZLRS_APP_DEBUG)
/* Debug shell receives a copy; all link/configuration APIs above remain owner-thread only. */
void zlrs_rf_trace(char *out, size_t capacity);
int zlrs_rf_debug_report(uint8_t id, uint16_t offset, uint8_t *out, uint8_t capacity,
			 uint16_t *total, uint8_t *written);
#endif

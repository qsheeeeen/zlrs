/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/zbus/zbus.h>

#include "zlrs/proto/frame.h"
#include "zlrs/link/state.h"
#include "zlrs/region/region.h"
#include "platform/lr2021.h"

typedef struct {
	zlrs_role_t role;
	zlrs_profile_t profile;
	zlrs_data_band_t data_band;
	zlrs_data_band_t band_mode;
	bool band_acquiring;
	bool band_planned;
	uint32_t band_switches;
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
	int16_t peer_rssi_dbm;
	uint8_t peer_lq_percent;
	int8_t power_floor_dbm;
	int8_t power_ceiling_dbm;
	uint32_t tx_frames;
	uint32_t rx_frames;
	uint32_t rx_crc_errors;
	uint32_t rx_bad_tag;
	uint32_t tx_retries;
	uint32_t missed_slots;
	uint32_t radio_fail;
	uint32_t frequency_hz;
	uint16_t hop_index;
	zlrs_lr2021_status_t radio;
} zlrs_rf_status_t;

/* Public facts only: no keys or mutable link pointers. */
ZBUS_CHAN_DECLARE(zlrs_rf_chan);

/* These functions execute on the RF owner thread, including frontend callbacks. */
/* Boot once from saved settings/credentials: 0 when ready, negative errno on failure. */
int zlrs_rf_init(void);
/* Start once after successful init. Each frame waits for its absolute boundary and returns. */
void zlrs_rf_start(void);
void zlrs_rf_frame(void);
/* Also publishes an initialization fault while main keeps management reachable. */
void zlrs_rf_publish_state(void);
bool zlrs_rf_pairing(uint8_t peer_id[ZLRS_DEV_ID_LEN], uint8_t pair_salt[ZLRS_PAIR_SALT_LEN]);
zlrs_rf_status_t zlrs_rf_status(void);
/* Fresh chip measurements only at a paused management boundary; publishes the
 * snapshot for shell/UI readers without SPI work on their threads. */
void zlrs_rf_status_refresh(void);
int zlrs_rf_pair_start(void);
int zlrs_rf_pair_cancel(void);
/* Reload saved credentials in a maintenance window; ordinary settings wait for reboot. */
void zlrs_rf_credentials(void);
/* Safe boundary: cancel radio launch and invalidate preloaded data. */
void zlrs_rf_pause(void);
void zlrs_rf_resume(void);
/* Captured only while paused at a management boundary. */
void zlrs_rf_log_snapshot(void);

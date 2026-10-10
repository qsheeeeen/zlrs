/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/zbus/zbus.h>

#include "zlrs/proto/limits.h"
#include "zlrs/link/state.h"

/* Copied facts. Link storage and keys never leave the RF owner. */
struct zlrs_link_status {
	bool role_tx;
	bool configured;
	bool paired;
	uint8_t state;
	uint8_t lq_percent;
};

struct zlrs_alerts {
	bool link_lost;
	bool fault;
	uint32_t data_drop_bytes;
};

ZBUS_CHAN_DECLARE(zlrs_link_chan, zlrs_alert_chan);

typedef struct {
	uint8_t role;
	uint8_t profile;
	uint8_t data_band;
	uint8_t region;
	uint8_t state;
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	bool configured;
	bool paired;
	bool session_valid;
	bool paused;
	uint8_t lq_percent;
	int8_t tx_power_dbm;
	int16_t rssi_dbm;
	int8_t snr_db;
	uint32_t frame_period_us;
} zlrs_rf_status_t;

/* These functions execute on the RF owner thread, including frontend callbacks. */
/* Boot once: 0 only when RF is ready, negative errno on failure. */
int zlrs_rf_init(void);
/* Application policy runs at RF owner boundaries, including the fault/recovery loop. */
typedef struct {
	/* Return true if a management session consumed this frame. */
	bool (*before_frame)(bool fault);
	void (*after_frame)(uint32_t now_ms);
} zlrs_rf_owner_hooks_t;
int zlrs_rf_run(const zlrs_rf_owner_hooks_t *hooks);
zlrs_rf_status_t zlrs_rf_status(void);
int zlrs_rf_pair_start(void);
int zlrs_rf_pair_cancel(void);
void zlrs_rf_identity(const uint8_t id[ZLRS_DEV_ID_LEN]);
void zlrs_rf_root_key(const uint8_t key[ZLRS_KEY_LEN]);
/* Nestable safe boundary: cancel radio launch and invalidate preloaded data. */
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

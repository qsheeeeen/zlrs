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
	bool failsafe;
	bool fault;
	uint32_t data_drop_bytes;
};

ZBUS_CHAN_DECLARE(zlrs_link_chan, zlrs_alert_chan);

typedef struct {
	uint8_t role;
	uint8_t profile;
	uint8_t mode;
	uint8_t region;
	uint8_t state;
	uint8_t control_state;
	uint8_t dev_id[ZLRS_DEV_ID_LEN];
	uint8_t peer_id[ZLRS_DEV_ID_LEN];
	bool configured;
	bool paired;
	bool session_valid;
	bool control_enabled;
	bool paused;
	uint8_t lq_percent;
	uint8_t control_lq;
	int8_t tx_power_dbm;
	int8_t peer_power_dbm;
	int16_t rssi_dbm;
	int8_t snr_db;
	uint16_t peer_timeout_ms;
	int16_t rssi_reference_dbm;
	uint32_t control_rate_hz;
	uint32_t frame_period_us;
} zlrs_rf_status_t;

/* These functions execute on the RF owner thread, including frontend callbacks. */
int zlrs_rf_init(void);
int zlrs_rf_run(void);
zlrs_rf_status_t zlrs_rf_status(void);
void zlrs_rf_control_input(const uint8_t *payload, uint8_t len);
void zlrs_rf_telemetry(const uint8_t *payload, uint8_t len);
bool zlrs_rf_send_service(const uint8_t *frame, uint8_t len);
int zlrs_rf_pair_start(void);
int zlrs_rf_pair_cancel(void);
void zlrs_rf_model(uint8_t model, bool match);
void zlrs_rf_identity(const uint8_t id[ZLRS_DEV_ID_LEN]);
void zlrs_rf_root_key(const uint8_t key[ZLRS_KEY_LEN]);
/* Nestable safe boundary: cancel radio launch and invalidate preloaded/stale control. */
void zlrs_rf_pause(void);
void zlrs_rf_resume(void);
void zlrs_rf_request_reboot(uint32_t delay_ms);
void zlrs_rf_cancel_reboot(void);
#if defined(CONFIG_ZLRS_APP_DEBUG)
/* Debug shell receives a copy; all link/configuration APIs above remain owner-thread only. */
void zlrs_rf_trace(char *out, size_t capacity);
int zlrs_rf_debug_report(uint8_t id, uint16_t offset, uint8_t *out, uint8_t capacity,
			 uint16_t *total, uint8_t *written);
#endif

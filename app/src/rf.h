/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/zbus/zbus.h>

#include "link/link.h"
#include "link/state.h"
#include "platform/lr2021.h"
#include "power.h"
#include "proto/frame.h"
#include "region/region.h"

/* RF owner costs, including hash reservation in TX finalization. */
typedef struct {
	zlrs_timing_t tx_finalize_us;
	zlrs_timing_t tx_prepare_us;
	zlrs_timing_t rx_accept_us;
	uint32_t hash_busy_tx, hash_busy_rx;
} zlrs_rf_work_t;

typedef struct {
	uint32_t wins;
	uint32_t hits;
	uint32_t tx;
	uint32_t span_ms;
	zlrs_link_state_t state;
} zlrs_rf_rate_sample_t;

typedef struct {
	uint32_t count;
	uint64_t total_us;
	uint64_t max_us;
	uint32_t skipped_after_maintenance;
	uint32_t skipped_without_maintenance;
} zlrs_rf_maintenance_t;

typedef struct {
	const zlrs_link_status_t *link;
	bool links_ready, radio_ready, ever_connected;
	zlrs_lr2021_status_t radio;
	int16_t temperature_centi_c;
	uint32_t hardware_sampled_ms;
	zlrs_data_band_t band_mode;
	uint32_t band_switches;
	zlrs_rf_rate_sample_t rate;
	const zlrs_region_t *region; /* immutable region table */
	uint32_t frame_period_us;
	uint32_t slot_prepare_us;
	uint32_t skipped_frames;
	uint32_t missed_slots;
	zlrs_rf_work_t work;
	uint32_t prepare_wait_skipped;
	int32_t phase_err_us;
	int32_t phase_last_us;
	uint32_t phase_hits;
	uint32_t late_prepare_entry_us;
	uint32_t late_prepare_build_us;
	uint32_t late_prepare_spi_us;
	bool power_fixed;
	zlrs_power_t power;
	zlrs_rf_maintenance_t maintenance;
	bool paused;
	uint64_t pause_started_cycles;
} zlrs_rf_state_t;

/* Read-only facts are valid on the RF owner thread until its next mutation. */
const zlrs_rf_state_t *zlrs_rf_state(void);

/* Owned inputs for UI and asynchronous debug request validation. */
typedef struct {
	zlrs_role_t role;
	zlrs_data_band_t data_band;
	zlrs_data_band_t band_mode;
	zlrs_phy_t data_phy;
	bool band_acquiring;
	bool band_planned;
	zlrs_link_state_t state;
	bool session_valid;
	bool link_lost;
	bool fault;
	uint32_t data_drop_bytes;
	uint8_t lq_percent;
	uint32_t frame_period_us;
	int8_t power_floor_dbm;
	int8_t power_ceiling_dbm;
} zlrs_rf_event_t;

/* Public facts only: no keys or mutable link pointers. */
ZBUS_CHAN_DECLARE(zlrs_rf_chan);

/* These functions execute on the RF owner thread, including frontend callbacks. */
/* Boot once from saved settings/credentials: 0 when ready, negative errno on failure. */
int zlrs_rf_init(void);
/* Start once after successful init. Each frame waits for its absolute boundary and returns. */
void zlrs_rf_start(void);
void zlrs_rf_frame(void);
/* Also publishes an initialization fault while main keeps management reachable. */
void zlrs_rf_publish_state(bool force);
/* Fresh chip measurements only at a paused management boundary; publishes the
 * snapshot for management/UI readers without SPI work on their threads. */
void zlrs_rf_status_refresh(void);
/* Zero means pairing is active; -EAGAIN means retry when a full nonce is ready. */
int zlrs_rf_pair_start(void);
int zlrs_rf_pair_cancel(void);
/* Reload saved credentials in a maintenance window; ordinary settings wait for reboot. */
void zlrs_rf_credentials(void);
/* Safe boundary: cancel radio launch and close the pending transaction. */
void zlrs_rf_pause(void);
void zlrs_rf_resume(void);
/* Captured only while paused at a management boundary. */
void zlrs_rf_log_snapshot(void);

#if defined(CONFIG_ZLRS_APP_DEBUG)
/* Any thread: enqueue a volatile debug override for the RF owner. */
int zlrs_rf_power_request(bool automatic, int dbm);
/* Leader only: negotiate one volatile 2.4 GHz profile change for bench tests. */
int zlrs_rf_phy_request(unsigned int phy);
#endif

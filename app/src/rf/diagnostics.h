/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "zlrs/link/link.h"
#include "zlrs/region/region.h"

/* RF-internal copied observations, captured only on the owner thread. No keys/buffers. */
typedef struct {
	zlrs_link_cfg_t config;
	zlrs_link_info_t info;
	zlrs_link_stats_t stats;
	zlrs_link_diagnostics_t diagnostics;
	bool active;
} zlrs_rf_link_snapshot_t;

typedef struct {
	uint32_t wins;
	uint32_t hits;
	uint32_t tx;
	uint32_t span_ms;
	uint8_t state;
} zlrs_rf_rate_sample_t;

typedef struct {
	zlrs_rf_link_snapshot_t links[2];
	zlrs_rf_rate_sample_t rate[2];
	zlrs_mod_cfg_t data_mod;
	const zlrs_region_t *region; /* immutable region table */
	uint32_t frame_period_us;
	uint32_t slot_budget_us;
	uint32_t slot_prepare_us;
	uint16_t data_mtu;
	uint32_t skipped_frames;
	uint32_t missed_slots;
	uint32_t prepare_wait_skipped;
	uint32_t prepare_wait_late_us_max;
	uint32_t prepare_irq_late_us_max;
	uint32_t prepare_wake_us_max;
	int32_t phase_err_us;
	int32_t phase_last_us;
	uint32_t phase_hits;
	uint32_t late_prepare_entry_us;
	uint32_t late_prepare_build_us;
	uint32_t late_prepare_spi_us;
	uint32_t serial_pump_us_last;
	uint32_t protocol_build_us_last;
	uint32_t packet_build_us_last;
	uint32_t packet_build_us_max;
	int8_t tx_power_dbm;
} zlrs_rf_diagnostics_t;

/* Called by management diagnostics at a safe boundary; no work on the RF hot path. */
void zlrs_rf_diagnostics_capture(zlrs_rf_diagnostics_t *out);

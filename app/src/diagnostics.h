/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "power.h"
#include "zlrs/link/link.h"
#include "zlrs/region/region.h"

/* RF owner costs, including hash reservation in TX finalization. */
typedef struct {
	uint32_t tx_finalize_us_last, tx_finalize_us_max;
	uint32_t tx_prepare_us_last, tx_prepare_us_max;
	uint32_t rx_accept_us_last, rx_accept_us_max;
	uint32_t hash_busy_tx, hash_busy_rx;
} zlrs_rf_work_t;

/* RF-internal copied observations, captured only on the owner thread. No keys/buffers. */
typedef struct {
	zlrs_link_cfg_t config;
	zlrs_link_info_t info;
	zlrs_link_stats_t stats;
	zlrs_link_diagnostics_t diagnostics;
} zlrs_rf_link_snapshot_t;

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
	zlrs_rf_link_snapshot_t link;
	zlrs_rf_rate_sample_t rate;
	zlrs_mod_cfg_t data_mod;
	const zlrs_region_t *region; /* immutable region table */
	uint32_t frame_period_us;
	uint32_t slot_budget_us;
	uint32_t slot_prepare_us;
	uint16_t data_mtu;
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
} zlrs_rf_diagnostics_t;

/* Called by management diagnostics at a safe boundary; no work on the RF hot path. */
void zlrs_rf_diagnostics_capture(zlrs_rf_diagnostics_t *out);

/* Diagnostic IDs belong to the application; management converts their wire representation. */
typedef enum {
	ZLRS_DIAGNOSTIC_LINK = 0,
	ZLRS_DIAGNOSTIC_RATE,
	ZLRS_DIAGNOSTIC_RADIO,
	ZLRS_DIAGNOSTIC_POWER,
} zlrs_diagnostic_report_t;

/* Snapshot and render on the owner thread; native SMP carries the complete text. */
int zlrs_diagnostics_report(zlrs_diagnostic_report_t report, const char **text);

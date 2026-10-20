/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "power.h"
#include "timing.h"
#include "zlrs/link/link.h"
#include "zlrs/region/region.h"

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

typedef enum {
	ZLRS_RF_TIMING_PREPARE_IRQ,
	ZLRS_RF_TIMING_OWNER_WAKE,
	ZLRS_RF_TIMING_SERIAL,
	ZLRS_RF_TIMING_PROTOCOL,
	ZLRS_RF_TIMING_PACKET,
	ZLRS_RF_TIMING_RADIO_PREPARE,
	ZLRS_RF_TIMING_RX_PROTOCOL,
	ZLRS_RF_TIMING_RX_FINISH,
	ZLRS_RF_TIMING_COUNT,
} zlrs_rf_timing_phase_t;

typedef struct {
	zlrs_timing_histogram_t phase[ZLRS_RF_TIMING_COUNT];
	uint32_t epoch;
	uint32_t started_ms;
} zlrs_rf_timings_t;

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
	uint32_t rx_finish_us_last;
	uint32_t rx_finish_us_max;
	uint32_t rx_protocol_us_last;
	uint32_t rx_protocol_us_max;
	bool power_fixed;
	zlrs_power_t power;
	zlrs_rf_timings_t timings;
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

/* Begin a snapshot with offset 0, then read its chunks on the owner thread.
 * Returns 0, -EINVAL for an invalid report/offset/capacity, or -ENOSPC for report overflow.
 */
int zlrs_diagnostics_read(zlrs_diagnostic_report_t report, uint16_t offset, uint8_t *out,
			  uint8_t capacity, uint16_t *total, uint8_t *written);

/*
 * Region layer: turns "where is this used" into a channel plan, power ceiling and duty
 * cap (doc/07-protocol.md). Validation at boot and limiting before TX are separate.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "zlrs/proto/fhss.h"
#include "zlrs/proto/mod.h"

#ifdef __cplusplus
extern "C" {
#endif

/* sub-G hardware: LF covers 410-510 MHz, HF covers 850-930 MHz (two SKUs) */
typedef enum {
	ZLRS_BAND_SUBG_LF = 0,
	ZLRS_BAND_SUBG_HF = 1,
} zlrs_band_t;

typedef struct {
	const char *name;
	zlrs_fhss_domain_id_t domain;
	zlrs_band_t band;
	uint32_t low_hz; /* band edge, used to validate the channel plan */
	uint32_t high_hz;
	int8_t max_tx_power_dbm;
	uint8_t duty_percent;    /* 0 = unlimited */
	uint32_t duty_window_ms; /* duty accounting window */
	uint32_t max_rate_bps;   /* rate ceiling implied by the band width */
} zlrs_region_t;

/*
 * The radio design a region has to fit: which sub-G hardware this is plus the
 * frequency range that hardware can actually reach. The board declares it
 * (devicetree node zlrs_rf), the region table declares the rules, and
 * validation checks one against the other instead of matching a board name.
 */
typedef struct {
	zlrs_band_t band;
	uint32_t low_hz; /* coverage of the radio design */
	uint32_t high_hz;
} zlrs_rf_design_t;

typedef enum {
	ZLRS_REGION_FCC915 = 0,
	ZLRS_REGION_EU868,
	ZLRS_REGION_CN470,
	ZLRS_REGION_CN433,
	ZLRS_REGION_COUNT
} zlrs_region_id_t;

typedef enum {
	ZLRS_DATA_BAND_24 = 0,
	ZLRS_DATA_BAND_SUBG = 1,
	ZLRS_DATA_BAND_AUTO = 2 /* policy choice; active band remains 0 or 1 */
} zlrs_data_band_t;

const zlrs_region_t *zlrs_region_get(zlrs_region_id_t id);

/*
 * Check that region, radio design, channel plan and TX power agree with each other: the
 * design has to be the right band and cover the region's band edges, every channel has to
 * fit the band, power has to respect the ceiling. Returns 0 when fine, negative otherwise
 * (the value only feeds the log). -5 = wrong band, -6 = design does not cover the region.
 */
int zlrs_region_validate(const zlrs_region_t *region, const zlrs_rf_design_t *rf,
			 int8_t tx_power_dbm);

/* Rate ceiling from the band width: clamp the configured value, return what is usable */
uint32_t zlrs_region_clamp_rate(const zlrs_region_t *region, uint32_t rate_bps);

/* Duty accounting: airtime accumulates per window and TX is denied past the cap */
typedef struct {
	uint32_t window_ms;
	uint32_t window_start_ms;
	uint32_t used_ms;
	uint8_t limit_percent;
	bool limited;
	uint32_t denied; /* denied transmissions, kept for observability */
} zlrs_duty_t;

void zlrs_duty_init(zlrs_duty_t *duty, const zlrs_region_t *region, uint32_t now_ms);

/* Ask for airtime_ms of transmit budget: true books it, false counts a denial */
bool zlrs_duty_reserve(zlrs_duty_t *duty, uint32_t airtime_ms, uint32_t now_ms);

/* Remaining airtime in this window (ms); UINT32_MAX when unlimited (may roll the window) */
uint32_t zlrs_duty_remaining_ms(zlrs_duty_t *duty, uint32_t now_ms);

/* Estimate airtime from frame length and rate (us, rounded up), used for duty and planning */
uint32_t zlrs_airtime_us(uint16_t frame_len, uint32_t rate_bps);

/* Same estimate in milliseconds (rounded up), used by the duty accounting */
uint32_t zlrs_airtime_ms(uint16_t frame_len, uint32_t rate_bps);

/*
 * How many frames per second this region allows at the given frame length and rate.
 * UINT32_MAX when the duty cycle is not limited.
 */
uint32_t zlrs_region_max_frames_per_s(const zlrs_region_t *region, uint32_t rate_bps,
				      uint16_t frame_len);

#ifdef __cplusplus
}
#endif

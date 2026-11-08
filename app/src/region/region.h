/*
 * Region layer: turns "where is this used" into a channel plan, power ceiling and duty
 * cap (doc/07-protocol.md). Selection is restricted to the board-defined region set.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/dt-bindings/zlrs/region.h>

#include "proto/mod.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const char *name;
	uint32_t start_hz;
	uint32_t step_hz;
	uint8_t channel_count;
	uint32_t low_hz; /* band edge, used to validate the channel plan */
	uint32_t high_hz;
	int8_t max_tx_power_dbm;
	uint8_t duty_percent;    /* 0 = unlimited */
	uint32_t duty_window_ms; /* duty accounting window */
	uint32_t max_rate_bps;   /* rate ceiling implied by the band width */
} zlrs_region_t;

typedef uint8_t zlrs_region_id_t;

extern const zlrs_region_t zlrs_regions[ZLRS_REGION_COUNT];

typedef enum {
	ZLRS_DATA_BAND_24 = 0,
	ZLRS_DATA_BAND_SUBG = 1,
	ZLRS_DATA_BAND_AUTO = 2 /* policy choice; active band remains 0 or 1 */
} zlrs_data_band_t;

/* Lookup succeeds only for a known region enabled by this board's devicetree. */
const zlrs_region_t *zlrs_region_get(zlrs_region_id_t id);
uint32_t zlrs_region_supported_mask(void);

/* Rate ceiling from the band width: clamp the configured value, return what is usable */
uint32_t zlrs_region_clamp_rate(const zlrs_region_t *region, uint32_t rate_bps);

/* Duty accounting: airtime accumulates per window and TX is denied past the cap */
typedef struct {
	uint32_t window_ms;
	uint32_t window_start_ms;
	uint32_t used_ms;
	uint32_t budget_ms;
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

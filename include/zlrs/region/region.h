/*
 * Region layer: turns "where is this used" into a channel plan, power ceiling and duty
 * cap (docs/design/09-protocol.md 5.2). Validation at boot and limiting before TX are separate.
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

typedef struct {
	const char *name;
	uint8_t domain;  /* ZLRS_DOMAIN_* */
	uint8_t band;    /* ZLRS_BAND_*: which sub-G hardware this region needs */
	uint32_t low_hz; /* band edge, used to validate the channel plan */
	uint32_t high_hz;
	int8_t max_tx_power_dbm;
	uint8_t duty_percent;    /* 0 = unlimited */
	uint32_t duty_window_ms; /* duty accounting window */
	uint32_t max_rate_bps;   /* rate ceiling implied by the band width */
} zlrs_region_t;

/* sub-G hardware: LF covers 410-510 MHz, HF covers 850-930 MHz (two SKUs) */
typedef enum {
	ZLRS_BAND_SUBG_LF = 0,
	ZLRS_BAND_SUBG_HF = 1,
} zlrs_band_t;

enum {
	ZLRS_REGION_FCC915 = 0,
	ZLRS_REGION_EU868,
	ZLRS_REGION_CN470,
	ZLRS_REGION_CN433,
	ZLRS_REGION_COUNT
};

const zlrs_region_t *zlrs_region_get(uint8_t id);

/*
 * Check that region, local sub-G hardware, channel plan and TX power agree with each other:
 * the band has to match the board, every channel has to fit the band, power has to respect
 * the ceiling. Returns 0 when fine, negative otherwise (the value only feeds the log).
 */
int zlrs_region_validate(const zlrs_region_t *region, uint8_t band, int8_t tx_power_dbm);

/* Rate ceiling from the band width: clamp the configured value, return what is usable */
uint32_t zlrs_region_clamp_rate(const zlrs_region_t *region, uint32_t rate_bps);

/* Modulation goes through the same gate: FLRC clamps the rate, LoRa clamps the bandwidth */
zlrs_mod_cfg_t zlrs_region_clamp_mod(const zlrs_region_t *region, zlrs_mod_cfg_t mod);

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

/* Modulation-aware: equivalent for FLRC, LoRa uses the model in core/proto/mod.c */
uint32_t zlrs_airtime_mod_us(const zlrs_mod_cfg_t *mod, uint16_t frame_len);
uint32_t zlrs_airtime_mod_ms(const zlrs_mod_cfg_t *mod, uint16_t frame_len);

/*
 * How many frames per second this region allows at the given frame length and rate.
 * UINT32_MAX when the duty cycle is not limited.
 */
uint32_t zlrs_region_max_frames_per_s(const zlrs_region_t *region, uint32_t rate_bps,
				      uint16_t frame_len);

/*
 * Regulatory ceiling for the sub-band mix (docs/design/09-protocol.md 5.1): how many of the 4 slots
 * may land on sub-G. Regions without a duty limit (duty_percent = 0) return 3 = 100%; the
 * others are folded as window length x duty into the four levels (0/1/2/4 slots) and get the
 * highest one that still fits.
 *   window_ms            - total length of the 4 slots
 *   subg_slot_airtime_us - airtime of one sub-G slot
 */
uint8_t zlrs_region_mix_level(const zlrs_region_t *region, uint32_t window_ms,
			      uint32_t subg_slot_airtime_us);

/* Same for a modulation (a LoRa frame is far longer than FLRC, so duty bites harder) */
uint32_t zlrs_region_max_frames_per_s_mod(const zlrs_region_t *region, const zlrs_mod_cfg_t *mod,
					  uint16_t frame_len);

/*
 * The control link's time plan: frame rate, frame period, per-slot budget and the telemetry
 * and data thinning factors. Two things bound the rate: the regional duty cap, and a frame
 * having to fit one RC frame out plus one telemetry frame in.
 */
typedef struct {
	uint32_t rate_hz; /* effective control frame rate */
	uint32_t frame_period_us;
	uint32_t slot_budget_us; /* wait budget per slot */
	uint8_t tlm_every_n;     /* telemetry every n frames */
	uint8_t data_every_n;    /* data window every n frames */
	bool clamped_by_duty;    /* the regional duty cap lowered the rate */
	bool clamped_by_airtime; /* airtime lowered the rate */
} zlrs_control_plan_t;

/*
 * Inputs to the control time plan. Grouped rather than positional because the modulation and
 * the two frame lengths plus the other link's reservation all change with hardware and region.
 */
typedef struct {
	const zlrs_region_t *region;
	const zlrs_mod_cfg_t *mod; /* control link modulation */
	uint32_t want_hz;          /* configured tier */
	uint16_t frame_len;        /* RC frame bytes, header and tag included */
	uint16_t tlm_len;          /* telemetry frame bytes; 0 = use frame_len */
	/*
	 * Reservation for the other link: give it its modulation and frame length and the plan
	 * amortises it per frame through data_every_n (see zlrs_control_plan_t).
	 * NULL = no reservation (control only).
	 */
	const zlrs_mod_cfg_t *other_mod;
	uint16_t other_frame_len;
} zlrs_plan_req_t;

void zlrs_region_control_plan(const zlrs_plan_req_t *req, zlrs_control_plan_t *out);

#ifdef __cplusplus
}
#endif

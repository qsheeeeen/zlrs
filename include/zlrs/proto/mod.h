/*
 * Modulation tiers and the airtime model, see docs/design/07-architecture.md 3. With LoRa "how long
 * a frame takes" is no longer a division: SF, bandwidth, coding rate and preamble decide it.
 * The superframe, slot budgets, ARQ timeouts and duty accounting all take their time from here.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	ZLRS_MOD_FLRC = 0,
	ZLRS_MOD_LORA = 1,
} zlrs_modulation_t;

/*
 * A full tier description. Members are ordered largest first so device and host can compare
 * with the same sizeof, see zlrs_mod_equal().
 */
typedef struct {
	uint32_t rate_bps;  /* FLRC: raw rate tier; unused (0) for LoRa */
	uint32_t bw_hz;     /* LoRa: bandwidth (125k / 250k / 500k); unused for FLRC */
	uint8_t modulation; /* ZLRS_MOD_* */
	uint8_t sf;         /* LoRa: spreading factor 5..12 */
	uint8_t cr;         /* LoRa: coding rate, 1..4 means 4/5..4/8 */
	uint8_t preamble;   /* LoRa: preamble symbols */
	uint8_t sync_byte;  /* LoRa: 1-byte sync word (top bit is the private bit) */
	uint8_t reserved[3];
} zlrs_mod_cfg_t;

/* Are two tiers identical (rate adaptation uses it to decide whether to change) */
bool zlrs_mod_equal(const zlrs_mod_cfg_t *a, const zlrs_mod_cfg_t *b);

/* FLRC tier: only the rate means anything */
zlrs_mod_cfg_t zlrs_mod_flrc(uint32_t rate_bps);

/*
 * LoRa tier: SF, bandwidth, coding rate, preamble symbols. The sync word derives from key
 * material that only exists after pairing, so pass 0 here and let the link layer fill it in.
 */
zlrs_mod_cfg_t zlrs_mod_lora(uint8_t sf, uint32_t bw_hz, uint8_t cr, uint8_t preamble);

/*
 * Airtime of one frame (us, rounded up).
 *   FLRC: payload plus 4 bytes (preamble/sync/length) at the raw rate
 *   LoRa: (n + 4.25) preamble symbols plus payload symbols, per Semtech's LoRa time formula
 * Returns 0 for invalid parameters (zero rate, zero bandwidth, out-of-range SF).
 */
uint32_t zlrs_mod_airtime_us(const zlrs_mod_cfg_t *mod, uint16_t frame_len);

/* Same estimate in milliseconds (rounded up), used by the duty accounting */
uint32_t zlrs_mod_airtime_ms(const zlrs_mod_cfg_t *mod, uint16_t frame_len);

#ifdef __cplusplus
}
#endif

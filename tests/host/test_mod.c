/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modulation tiers and the airtime model (core/proto/mod.c).
 *
 * Where the expected values come from:
 *   - FLRC and zlrs_airtime_us() in core/region must agree (one model, two call sites)
 *   - LoRa timing by hand from Semtech's formula: Ts = 2^SF/BW, preamble (n + 4.25) symbols,
 *     payload symbols n_payload = 8 + ceil((8*PL - 4*SF + 28 + 16) / (4*(SF - 2*DE))) * (CR+4)
 */

#include "test.h"

#include "zlrs/proto/mod.h"
#include "zlrs/region/region.h"

#include "test.h"

#include "zlrs/proto/mod.h"
#include "zlrs/region/region.h"

/* FLRC: point-by-point agreement with the region layer's model */
static void test_mod_flrc_matches_region(void)
{
	static const uint32_t rates[] = {2600000U, 1300000U, 650000U, 325000U};
	static const uint16_t lens[] = {7U, 31U, 128U, 255U};
	const zlrs_mod_cfg_t no_rate = zlrs_mod_flrc(0U);

	printf("mod: flrc airtime matches region model\n");

	for (uint8_t i = 0U; i < sizeof(rates) / sizeof(rates[0]); i++) {
		const zlrs_mod_cfg_t mod = zlrs_mod_flrc(rates[i]);

		for (uint8_t j = 0U; j < sizeof(lens) / sizeof(lens[0]); j++) {
			CHECK(zlrs_mod_airtime_us(&mod, lens[j]) ==
			      zlrs_airtime_us(lens[j], rates[i]));
			CHECK(zlrs_mod_airtime_ms(&mod, lens[j]) ==
			      zlrs_airtime_ms(lens[j], rates[i]));
		}
	}

	/* No rate (0) cannot be computed: return 0 instead of dividing by zero */
	CHECK(zlrs_mod_airtime_us(&no_rate, 31U) == 0U);
}

/*
 * SF7 / BW125 / CR 4/5 / 8 preamble symbols:
 *   Ts = 128 / 125000 = 1.024 ms
 *   31 B payload: ceil((248 - 28 + 44) / 28) = ceil(9.43) = 10 -> 10 * 5 + 8 = 58 symbols
 *   72.2 ms? No: preamble (8 + 4.25) * 1.024 = 12.544 ms, payload 58 * 1.024 = 59.392 ms,
 *   total 71.936 ms
 *   7 B heartbeat: ceil((56 - 28 + 44) / 28) = 3 -> 3 * 5 + 8 = 23 symbols = 23.552 ms,
 *   total 36.096 ms
 */
static void test_mod_lora_reference(void)
{
	const zlrs_mod_cfg_t sf7 = zlrs_mod_lora(7U, 125000U, 1U, 8U);
	const zlrs_mod_cfg_t sf12 = zlrs_mod_lora(12U, 125000U, 1U, 8U);
	const zlrs_mod_cfg_t sf7_250 = zlrs_mod_lora(7U, 250000U, 1U, 8U);
	const zlrs_mod_cfg_t sf7_cr8 = zlrs_mod_lora(7U, 125000U, 4U, 8U);
	const zlrs_mod_cfg_t no_bw = zlrs_mod_lora(7U, 0U, 1U, 8U);
	const zlrs_mod_cfg_t bad_sf = zlrs_mod_lora(13U, 125000U, 1U, 8U);
	uint32_t rc_us;
	uint32_t tlm_us;

	printf("mod: lora airtime reference values\n");

	rc_us = zlrs_mod_airtime_us(&sf7, 31U);
	tlm_us = zlrs_mod_airtime_us(&sf7, 7U);
	CHECK(rc_us == 71936U);
	CHECK(tlm_us == 36096U);
	CHECK(zlrs_mod_airtime_ms(&sf7, 31U) == 72U);

	/* Double the bandwidth halves the time (integer rounding allows a 1 us difference) */
	CHECK(zlrs_mod_airtime_us(&sf7_250, 31U) == rc_us / 2U);

	/* CR 4/8 is slower than 4/5 but never slower than a full preamble: monotonic is enough */
	CHECK(zlrs_mod_airtime_us(&sf7_cr8, 31U) > rc_us);

	/* Higher SF is slower: SF12 is an order of magnitude behind SF7 */
	CHECK(zlrs_mod_airtime_us(&sf12, 31U) > 10U * rc_us);

	/* Longer payloads take longer (one smooth curve, no knees) */
	CHECK(zlrs_mod_airtime_us(&sf7, 64U) > zlrs_mod_airtime_us(&sf7, 32U));

	/* Invalid parameters give 0 rather than a nonsense number */
	CHECK(zlrs_mod_airtime_us(&no_bw, 31U) == 0U);
	CHECK(zlrs_mod_airtime_us(&bad_sf, 31U) == 0U);
	CHECK(zlrs_mod_airtime_us(NULL, 31U) == 0U);
}

/*
 * The 10 Hz LoRa tier has to fit one RC frame out plus one telemetry frame in: the quantitative
 * version of "the superframe period follows the slower link" (doc/07-architecture.md 3).
 */
static void test_mod_lora_control_budget(void)
{
	const zlrs_mod_cfg_t sf7 = zlrs_mod_lora(7U, 125000U, 1U, 8U);
	const uint32_t rc_us = zlrs_mod_airtime_us(&sf7, 31U);
	const uint32_t tlm_us = zlrs_mod_airtime_us(&sf7, 7U);
	const uint32_t period_us = 100000U; /* 10 Hz */

	printf("mod: lora control-link budget\n");

	/* A 100 ms period fits a 72 ms RC frame but not 72 + 36 */
	CHECK(rc_us < period_us);
	CHECK(rc_us + tlm_us > period_us);
	/* So telemetry thins out at 10 Hz: once every 2 frames just fits */
	CHECK(rc_us + tlm_us / 2U < period_us);
}

void test_mod(void)
{
	test_mod_flrc_matches_region();
	test_mod_lora_reference();
	test_mod_lora_control_budget();
}

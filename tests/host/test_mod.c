/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modulation tiers and the airtime model (core/proto/mod.c).
 *
 * Where the expected values come from:
 *   - FLRC and zlrs_airtime_us() in core/region must agree (one model, two call sites)
 */

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

void test_mod(void)
{
	test_mod_flrc_matches_region();

	/* Independent LR20xx airtime vectors: BW1MHz, CR4/5, explicit, CRC, preamble12. */
	static const struct {
		uint16_t len;
		uint32_t sf6, sf7;
	} cases[] = {{12U, 3279U, 5663U},
		     {24U, 4559U, 8223U},
		     {128U, 15759U, 27423U},
		     {255U, 29199U, 50463U}};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const zlrs_mod_cfg_t sf6 = zlrs_mod_lora(6U), sf7 = zlrs_mod_lora(7U);
		CHECK(zlrs_mod_airtime_us(&sf6, cases[i].len) == cases[i].sf6);
		CHECK(zlrs_mod_airtime_us(&sf7, cases[i].len) == cases[i].sf7);
		CHECK(zlrs_mod_airtime_us(&sf7, 256U) == 0U);
	}
	for (unsigned int phy = 0; phy < ZLRS_PHY_COUNT; phy++) {
		const zlrs_mod_cfg_t mod = zlrs_mod_phy(phy);
		CHECK(zlrs_mod_phy_id(&mod) == phy);
	}
}

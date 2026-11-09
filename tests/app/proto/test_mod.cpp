/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modulation tiers and the airtime model (app/src/proto/mod.c).
 *
 * Expected airtimes are independent numerical vectors for each PHY.
 */

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <zephyr/ztest.h>

#include "proto/mod.h"
#include "region/region.h"

/* Independent FLRC airtimes in us for 7, 31, 128 and 255 wire bytes. */
ZTEST(Modulation, test_FlrcAirtimeVectors)
{
	static const uint16_t lengths[] = {7U, 31U, 128U, 255U};

	static const struct {
		zlrs_phy_t phy;
		uint32_t expected[4];
	} vectors[] = {
		{ZLRS_PHY_FLRC_2600, {79U, 177U, 575U, 1096U}},
		{ZLRS_PHY_FLRC_1300, {157U, 354U, 1149U, 2191U}},
		{ZLRS_PHY_FLRC_650, {313U, 707U, 2297U, 4382U}},
		{ZLRS_PHY_FLRC_325, {625U, 1413U, 4594U, 8764U}},
	};

	for (const auto &vector : vectors) {
		for (size_t i = 0; i < ARRAY_SIZE(lengths); i++) {
			zexpect_equal(zlrs_phy_airtime_us(vector.phy, lengths[i]),
				      vector.expected[i]);
		}
	}
	zexpect_equal(zlrs_phy_for_rate(0U), ZLRS_PHY_COUNT);
	zexpect_equal(zlrs_phy_airtime_us(ZLRS_PHY_COUNT, 31U), 0U);
}

ZTEST(Modulation, test_LoRaVectorsAndProfileIds)
{

	/* Independent LR20xx airtime vectors: BW1MHz, CR4/5, explicit, CRC, preamble12. */
	static const struct {
		uint16_t len;
		uint32_t sf6, sf7;
	} cases[] = {{12U, 3279U, 5663U},
		     {24U, 4559U, 8223U},
		     {128U, 15759U, 27423U},
		     {255U, 29199U, 50463U}};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const zlrs_phy_t sf6 = ZLRS_PHY_LORA_SF6, sf7 = ZLRS_PHY_LORA_SF7;
		zexpect_equal(zlrs_phy_airtime_us(sf6, cases[i].len), cases[i].sf6);
		zexpect_equal(zlrs_phy_airtime_us(sf7, cases[i].len), cases[i].sf7);
		zexpect_equal(zlrs_phy_airtime_us(sf7, 256U), 0U);
	}
	static const char *const names[] = {"FLRC 2.6 Mbps",   "FLRC 650 kbps",   "FLRC 325 kbps",
					    "LoRa SF6/BW1MHz", "LoRa SF7/BW1MHz", "FLRC 1.3 Mbps"};
	for (unsigned int phy = 0; phy < ZLRS_PHY_COUNT; phy++) {
		zexpect_str_equal(zlrs_phy_name(static_cast<zlrs_phy_t>(phy)), names[phy]);
	}
	zexpect_str_equal(zlrs_phy_name(ZLRS_PHY_COUNT), "unknown");
}

ZTEST_SUITE(Modulation, NULL, NULL, NULL, NULL, NULL);

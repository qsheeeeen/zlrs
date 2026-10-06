/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test.h"
#include <string.h>

#include "zlrs/proto/fhss.h"

/* ---- hop tables ---------------------------------------------------------- */

void test_fhss(void)
{
	uint16_t seen[16];
	zlrs_fhss_t fhss;
	zlrs_fhss_t again;
	uint32_t first;
	uint32_t next;

	printf("fhss\n");

	/* Two tables (main 2.4G plus sub-G) from one seed, identical on both ends */
	{
		zlrs_fhss_t dual;
		zlrs_fhss_t dual2;

		zlrs_fhss_init_dual(&dual, ZLRS_DOMAIN_ISM2400, ZLRS_DOMAIN_CN433, 0x5a5a1234U);
		zlrs_fhss_init_dual(&dual2, ZLRS_DOMAIN_ISM2400, ZLRS_DOMAIN_CN433, 0x5a5a1234U);
		CHECK(zlrs_fhss_sub_ready(&dual));
		CHECK(dual.sub_count == (ZLRS_FHSS_SEQUENCE_LEN / 5U) * 5U);
		CHECK(zlrs_fhss_sub_current_hz(&dual) == zlrs_fhss_sub_current_hz(&dual2));
		CHECK(zlrs_fhss_sub_current_hz(&dual) >= 433050000U);
		CHECK(zlrs_fhss_sub_current_hz(&dual) <= 434790000U);
		CHECK(zlrs_fhss_sub_next_hz(&dual) == zlrs_fhss_sub_current_hz(&dual));
		zlrs_fhss_set_sub_index(&dual, 3U);
		CHECK(zlrs_fhss_sub_current_hz(&dual) == zlrs_fhss_sub_current_hz(&dual2) ||
		      dual.sub_index == 3U);

		/* Four mix levels map to how many of every 4 slots land on sub-G */
		CHECK(zlrs_fhss_mix_slots(0U) == 0U);
		CHECK(zlrs_fhss_mix_slots(1U) == 1U);
		CHECK(zlrs_fhss_mix_slots(2U) == 2U);
		CHECK(zlrs_fhss_mix_slots(3U) == 4U);

		/* Without a sub-band the second table is unusable */
		zlrs_fhss_init(&dual, ZLRS_DOMAIN_ISM2400, 0x1U);
		CHECK(!zlrs_fhss_sub_ready(&dual));
		CHECK(zlrs_fhss_sub_current_hz(&dual) == 0U);
	}

	/* EU868: 13 channels, 256 / 13 = 19 blocks */
	zlrs_fhss_init(&fhss, ZLRS_DOMAIN_EU868, 0x1234abcdU);
	CHECK(fhss.domain->channel_count == 13U);
	CHECK(fhss.sequence_count == 247U);
	CHECK(fhss.sync_channel == 6U);

	memset(seen, 0, sizeof(seen));
	for (uint16_t i = 0; i < fhss.sequence_count; i++) {
		CHECK(fhss.sequence[i] < 13U);
		seen[fhss.sequence[i]]++;
		if ((i % 13U) == 0U) {
			CHECK(fhss.sequence[i] ==
			      fhss.sync_channel); /* slot 0 of every block is the sync channel */
		}
	}
	for (uint8_t ch = 0; ch < 13U; ch++) {
		CHECK(seen[ch] == 19U); /* every channel used equally often */
	}

	/* One seed gives one table on both ends; a new seed gives a new table */
	zlrs_fhss_init(&again, ZLRS_DOMAIN_EU868, 0x1234abcdU);
	CHECK(memcmp(fhss.sequence, again.sequence, fhss.sequence_count) == 0);
	zlrs_fhss_init(&again, ZLRS_DOMAIN_EU868, 0x1234abceU);
	CHECK(memcmp(fhss.sequence, again.sequence, fhss.sequence_count) != 0);

	zlrs_fhss_init(&fhss, ZLRS_DOMAIN_ISM2400, 0x1234abcdU);
	CHECK(fhss.sequence_count == 237U); /* 256 / 79 = 3 blocks */
	CHECK(fhss.domain->channel_count == 79U);
	CHECK(fhss.sequence[fhss.index] == fhss.sync_channel);

	first = zlrs_fhss_current_hz(&fhss);
	CHECK(first == fhss.domain->start_hz + (uint32_t)fhss.sync_channel * fhss.domain->step_hz);
	next = zlrs_fhss_next_hz(&fhss);
	CHECK(next != first);
	CHECK(fhss.index == 1U);
	zlrs_fhss_set_index(&fhss, 0U);
	CHECK(zlrs_fhss_current_hz(&fhss) == first);
}

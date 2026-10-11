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
	/* Each retained single-domain plan is reproducible, balanced and wraps at its own size. */
	for (zlrs_fhss_domain_id_t domain = ZLRS_DOMAIN_FCC915; domain < ZLRS_DOMAIN_COUNT;
	     ++domain) {
		uint16_t counts[96] = {0};
		zlrs_fhss_init(&fhss, domain, 0x5a5a1234U);
		zlrs_fhss_init(&again, domain, 0x5a5a1234U);
		CHECK(memcmp(fhss.sequence, again.sequence, fhss.sequence_count) == 0);
		first = zlrs_fhss_current_hz(&fhss);
		for (uint16_t i = 0U; i < fhss.sequence_count; ++i) {
			CHECK(fhss.sequence[i] < fhss.domain->channel_count);
			counts[fhss.sequence[i]]++;
			(void)zlrs_fhss_next_hz(&fhss);
		}
		CHECK(fhss.index == 0U);
		CHECK(zlrs_fhss_current_hz(&fhss) == first);
		for (uint8_t channel = 0U; channel < fhss.domain->channel_count; ++channel) {
			CHECK(counts[channel] == fhss.sequence_count / fhss.domain->channel_count);
		}
	}
}

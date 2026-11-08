/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string.h>
#include <zephyr/ztest.h>

#include "proto/fhss.h"

/* ---- hop tables ---------------------------------------------------------- */

ZTEST(Fhss, test_BalancedDeterministicHopTables)
{
	uint16_t seen[16];
	zlrs_fhss_t fhss;
	zlrs_fhss_t again;
	uint32_t first;
	uint32_t next;

	printf("fhss\n");

	/* EU868: 13 channels, 256 / 13 = 19 blocks */
	zlrs_fhss_init(&fhss, ZLRS_REGION_EU868, 0x1234abcdU);
	zexpect_equal(fhss.region->channel_count, 13U);
	zexpect_equal(fhss.sequence_count, 247U);
	zexpect_equal(fhss.sync_channel, 6U);

	memset(seen, 0, sizeof(seen));
	for (uint16_t i = 0; i < fhss.sequence_count; i++) {
		zexpect_true((fhss.sequence[i]) < (13U));
		seen[fhss.sequence[i]]++;
		if ((i % 13U) == 0U) {
			zexpect_equal(
				fhss.sequence[i],
				fhss.sync_channel); /* slot 0 of every block is the sync channel */
		}
	}
	for (uint8_t ch = 0; ch < 13U; ch++) {
		zexpect_equal(seen[ch], 19U); /* every channel used equally often */
	}

	/* One seed gives one table on both ends; a new seed gives a new table */
	zlrs_fhss_init(&again, ZLRS_REGION_EU868, 0x1234abcdU);
	zexpect_equal(memcmp(fhss.sequence, again.sequence, fhss.sequence_count), 0);
	zlrs_fhss_init(&again, ZLRS_REGION_EU868, 0x1234abceU);
	zexpect_not_equal(memcmp(fhss.sequence, again.sequence, fhss.sequence_count), 0);

	zlrs_fhss_init(&fhss, ZLRS_REGION_ISM2400, 0x1234abcdU);
	zexpect_equal(fhss.sequence_count, 237U); /* 256 / 79 = 3 blocks */
	zexpect_equal(fhss.region->channel_count, 79U);
	zexpect_equal(fhss.sequence[fhss.index], fhss.sync_channel);

	first = zlrs_fhss_current_hz(&fhss);
	zexpect_equal(first,
		      fhss.region->start_hz + (uint32_t)fhss.sync_channel * fhss.region->step_hz);
	zlrs_fhss_set_index(&fhss, fhss.index + 1U);
	next = zlrs_fhss_current_hz(&fhss);
	zexpect_not_equal(next, first);
	zexpect_equal(fhss.index, 1U);
	zlrs_fhss_set_index(&fhss, 0U);
	zexpect_equal(zlrs_fhss_current_hz(&fhss), first);
	/* Each retained single-domain plan is reproducible, balanced and wraps at its own size. */
	for (int domain = ZLRS_REGION_FCC915; domain < ZLRS_REGION_COUNT; ++domain) {
		uint16_t counts[96] = {0};
		zlrs_fhss_init(&fhss, static_cast<zlrs_region_id_t>(domain), 0x5a5a1234U);
		zlrs_fhss_init(&again, static_cast<zlrs_region_id_t>(domain), 0x5a5a1234U);
		zexpect_equal(memcmp(fhss.sequence, again.sequence, fhss.sequence_count), 0);
		first = zlrs_fhss_current_hz(&fhss);
		for (uint16_t i = 0U; i < fhss.sequence_count; ++i) {
			zexpect_true((fhss.sequence[i]) < (fhss.region->channel_count));
			counts[fhss.sequence[i]]++;
			zlrs_fhss_set_index(&fhss, fhss.index + 1U);
		}
		zexpect_equal(fhss.index, 0U);
		zexpect_equal(zlrs_fhss_current_hz(&fhss), first);
		for (uint8_t channel = 0U; channel < fhss.region->channel_count; ++channel) {
			zexpect_equal(counts[channel],
				      fhss.sequence_count / fhss.region->channel_count);
		}
	}
}

ZTEST_SUITE(Fhss, NULL, NULL, NULL, NULL, NULL);

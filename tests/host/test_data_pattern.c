/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test.h"
#include "../hardware/app/link/data_pattern.h"

void test_data_pattern(void)
{
	const uint32_t offsets[] = {0U,     1U,     2U,     3U,     252U,      253U,
				    254U,   255U,   256U,   257U,   65532U,    65533U,
				    65534U, 65535U, 65536U, 65537U, 0xfffffcU, UINT32_MAX - 3U};
	const uint16_t lengths[] = {0U, 1U, 2U, 3U, 4U, 31U, 64U, 511U, 512U, 1025U};
	uint8_t actual[1032];
	uint8_t expected[1025];

	printf("debug: bulk benchmark patterns and exact bad-byte counts\n");
	for (uint8_t leader = 0U; leader < 2U; leader++) {
		for (uint8_t align = 0U; align < 4U; align++) {
			for (size_t o = 0U; o < sizeof(offsets) / sizeof(offsets[0]); o++) {
				for (size_t l = 0U; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
					const uint16_t len = lengths[l];
					uint8_t *data = actual + align;

					memset(actual, 0xcc, sizeof(actual));
					for (uint16_t i = 0U; i < len; i++) {
						const uint32_t offset = offsets[o] + i;

						expected[i] = (uint8_t)(offset ^ (offset >> 8) ^
									(offset >> 16) ^
									(leader ? 0x5aU : 0xa5U));
					}
					zlrs_bench_pattern_fill(data, len, offsets[o], leader);
					CHECK(memcmp(data, expected, len) == 0);
					CHECK(data[len] == 0xccU);
					CHECK(zlrs_bench_pattern_bad(data, len, offsets[o],
								     leader) == 0U);
					CHECK(zlrs_bench_pattern_bad(data, len, offsets[o],
								     !leader) == len);
					if (len > 0U) {
						data[0] ^= 1U;
						CHECK(zlrs_bench_pattern_bad(data, len, offsets[o],
									     leader) == 1U);
					}
					if (len > 1U) {
						data[len - 1U] ^= 0x80U;
						CHECK(zlrs_bench_pattern_bad(data, len, offsets[o],
									     leader) == 2U);
					}
				}
			}
		}
	}
	zlrs_bench_pattern_fill(NULL, 0U, 0U, true);
	CHECK(zlrs_bench_pattern_bad(NULL, 0U, 0U, true) == 0U);
}

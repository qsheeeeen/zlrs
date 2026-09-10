/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/proto/crc.h"

#define ZLRS_CRC_TABLE_LEN 256U

void zlrs_crc_init(zlrs_crc_t *crc, uint8_t bits, uint16_t poly)
{
	uint16_t highbit;
	uint16_t value;

	if (crc->bits == bits && crc->poly == poly) {
		return;
	}

	crc->poly = poly;
	crc->bits = bits;
	crc->bitmask = (uint16_t)((1U << bits) - 1U);
	highbit = (uint16_t)(1U << (bits - 1U));

	for (uint16_t i = 0; i < ZLRS_CRC_TABLE_LEN; i++) {
		value = (uint16_t)(i << (bits - 8U));
		for (uint8_t j = 0; j < 8U; j++) {
			value = (uint16_t)((value << 1) ^
					   ((value & highbit) ? poly : 0U));
		}
		crc->tab[i] = value;
	}
}

uint16_t zlrs_crc_calc(zlrs_crc_t *crc, const uint8_t *data, uint8_t len,
		       uint16_t seed)
{
	while (len--) {
		seed = (uint16_t)((seed << 8) ^
			crc->tab[((seed >> (crc->bits - 8U)) ^ (uint16_t)*data++) & 0x00FFU]);
	}

	return (uint16_t)(seed & crc->bitmask);
}

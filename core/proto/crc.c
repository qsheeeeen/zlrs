/*
 * CRC8 (DVB-S2), see include/zlrs/proto/crc.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zlrs/proto/crc.h"

uint8_t zlrs_crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = 0U;

	for (size_t i = 0U; i < len; i++) {
		crc ^= data[i];
		for (uint8_t bit = 0U; bit < 8U; bit++) {
			crc = (uint8_t)((crc & 0x80U) != 0U ? (uint8_t)((crc << 1) ^ 0xD5U)
							    : (uint8_t)(crc << 1));
		}
	}

	return crc;
}

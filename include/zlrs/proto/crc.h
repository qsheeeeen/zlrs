/*
 * CRC8 for the serial-side tools (DVB-S2, polynomial 0xD5). CRSF and config frames use it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard check value: the CRC of "123456789" is 0xBC */
uint8_t zlrs_crc8(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

/* Internal wire helpers: explicit bytes work on unaligned buffers on every host. */
static inline uint16_t zlrs_get_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t zlrs_get_le32(const uint8_t *p)
{
	return (uint32_t)zlrs_get_le16(p) | ((uint32_t)zlrs_get_le16(p + 2) << 16);
}

static inline void zlrs_put_le16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static inline void zlrs_put_le32(uint8_t *p, uint32_t value)
{
	zlrs_put_le16(p, (uint16_t)value);
	zlrs_put_le16(p + 2, (uint16_t)(value >> 16));
}

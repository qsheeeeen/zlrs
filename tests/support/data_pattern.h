/* Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline uint8_t zlrs_bench_pattern_byte(uint32_t offset, bool sender_leader)
{
	return (uint8_t)(offset ^ (offset >> 8) ^ (offset >> 16) ^ (sender_leader ? 0x5aU : 0xa5U));
}

/* Call at a four-byte stream boundary. Each lane stays below 256, so adding the
 * replicated low byte produces four consecutive bytes without cross-lane carry. */
static inline uint32_t zlrs_bench_pattern_word(uint32_t offset, bool sender_leader)
{
	const uint8_t flip = (offset >> 8) ^ (offset >> 16) ^ (sender_leader ? 0x5aU : 0xa5U);

	return (0x03020100U + (offset & 0xffU) * 0x01010101U) ^ (uint32_t)flip * 0x01010101U;
}

static inline void zlrs_bench_pattern_fill(uint8_t *out, uint16_t len, uint32_t offset,
					   bool sender_leader)
{
	while (len > 0U && (offset & 3U) != 0U) {
		*out++ = zlrs_bench_pattern_byte(offset++, sender_leader);
		len--;
	}
	while (len >= 4U) {
		const uint32_t word = zlrs_bench_pattern_word(offset, sender_leader);
		const uint8_t bytes[4] = {word, word >> 8, word >> 16, word >> 24};

		memcpy(out, bytes, sizeof(bytes));
		out += 4U;
		offset += 4U;
		len -= 4U;
	}
	while (len-- > 0U) {
		*out++ = zlrs_bench_pattern_byte(offset++, sender_leader);
	}
}

static inline uint16_t zlrs_bench_pattern_bad(const uint8_t *data, uint16_t len, uint32_t offset,
					      bool sender_leader)
{
	uint16_t bad = 0U;

	while (len > 0U && (offset & 3U) != 0U) {
		bad += *data++ != zlrs_bench_pattern_byte(offset++, sender_leader);
		len--;
	}
	while (len >= 4U) {
		const uint32_t actual = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
					((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
		const uint32_t difference = actual ^ zlrs_bench_pattern_word(offset, sender_leader);

		if (difference != 0U) {
			bad += (difference & 0xffU) != 0U;
			bad += (difference & 0xff00U) != 0U;
			bad += (difference & 0xff0000U) != 0U;
			bad += (difference & 0xff000000U) != 0U;
		}
		data += 4U;
		offset += 4U;
		len -= 4U;
	}
	while (len-- > 0U) {
		bad += *data++ != zlrs_bench_pattern_byte(offset++, sender_leader);
	}
	return bad;
}

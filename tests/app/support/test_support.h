/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

static inline void zlrs_test_check_hex(const uint8_t *out, size_t len, const char *want)
{
	uint8_t expected[42];

	zassert_true(len <= sizeof(expected));
	zassert_equal(hex2bin(want, len * 2U, expected, sizeof(expected)), len);
	zassert_mem_equal(out, expected, len);
}

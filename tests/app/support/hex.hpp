/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zephyr/ztest.h>
#include <cctype>
#include <cstdint>
#include <string>

inline bool HexEquals(const uint8_t *data, size_t len, const char *hex)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string actual;
	actual.reserve(len * 2U);
	for (size_t i = 0; i < len; i++) {
		actual += digits[data[i] >> 4];
		actual += digits[data[i] & 0x0fU];
	}
	std::string expected(hex);
	for (char &c : expected) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (actual == expected) {
		return true;
	}
	TC_PRINT("actual hex: %s; expected: %s\n", actual.c_str(), expected.c_str());
	return false;
}

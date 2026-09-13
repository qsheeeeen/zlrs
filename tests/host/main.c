/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 协议核心与链路层的主机侧测试。两者都不依赖 Zephyr，可以直接在本机跑：
 *
 *   make -C tests/host run
 *
 * 期望值来源：SHA-256 / HMAC / HKDF / PBKDF2 用 RFC 向量，CRC 用标准向量，
 * 链路行为用双节点仿真（模拟射频，不接硬件）。
 */

#include "test.h"

unsigned int zlrs_test_checks;
unsigned int zlrs_test_failures;

int zlrs_test_hex_eq(const uint8_t *data, size_t len, const char *hex)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 0; i < len; i++) {
		char hi = hex[i * 2U];
		char lo = hex[i * 2U + 1U];

		if (hi >= 'A' && hi <= 'F') {
			hi = (char)(hi - 'A' + 'a');
		}
		if (lo >= 'A' && lo <= 'F') {
			lo = (char)(lo - 'A' + 'a');
		}
		if (hi != digits[data[i] >> 4] || lo != digits[data[i] & 0x0FU]) {
			printf("  hex mismatch at byte %zu: got %02x want %02x\n", i, data[i],
			       (uint8_t)((uint8_t)(hi >= 'a' ? hi - 'a' + 10 : hi - '0') << 4) |
				       (uint8_t)(lo >= 'a' ? lo - 'a' + 10 : lo - '0'));
			return 0;
		}
	}

	return len > 0U || hex[0] == '\0';
}

int main(void)
{
	printf("zlrs host tests\n");
	test_proto();
	test_link();
	test_crsf();
	test_config();
	test_region();
	test_led();
	test_power();
	printf("%u checks, %u failure(s)\n", zlrs_test_checks, zlrs_test_failures);

	return zlrs_test_failures == 0U ? 0 : 1;
}

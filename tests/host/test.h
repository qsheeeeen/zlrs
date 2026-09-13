/*
 * 主机侧测试的小工具。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_HOST_TEST_H_
#define ZLRS_HOST_TEST_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern unsigned int zlrs_test_checks;
extern unsigned int zlrs_test_failures;

#define CHECK(cond)                                                                 \
	do {                                                                        \
		zlrs_test_checks++;                                                 \
		if (!(cond)) {                                                      \
			zlrs_test_failures++;                                       \
			printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
		}                                                                   \
	} while (0)

/* 与十六进制串比较，大小写不敏感 */
int zlrs_test_hex_eq(const uint8_t *data, size_t len, const char *hex);

void test_proto(void);
void test_link(void);
void test_crsf(void);
void test_config(void);
void test_region(void);
void test_led(void);

#endif /* ZLRS_HOST_TEST_H_ */

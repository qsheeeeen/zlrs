/*
 * Small helpers for the host-side tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern unsigned int zlrs_test_checks;
extern unsigned int zlrs_test_failures;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		zlrs_test_checks++;                                                                \
		if (!(cond)) {                                                                     \
			zlrs_test_failures++;                                                      \
			printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                   \
		}                                                                                  \
	} while (0)

/* Compare against a hex string, case-insensitive */
int zlrs_test_hex_eq(const uint8_t *data, size_t len, const char *hex);

void test_crypto(void);
void test_frame(void);
void test_fhss(void);
void test_data_pattern(void);
void test_radio(void);
void test_link(void);
void test_mgmt(void);
void test_region(void);
void test_mod(void);
void test_fuzz(void);

void test_power(void);

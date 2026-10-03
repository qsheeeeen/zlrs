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

void test_proto(void);
void test_slot_clock(void);
void test_bench_pattern(void);
void test_radio_fifo(void);
void test_link(void);
void test_crsf(void);
void test_mgmt(void);
void test_region(void);
void test_mod(void);
void test_led(void);
void test_power(void);
void test_fuzz(void);

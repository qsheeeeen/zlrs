/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "link/link.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum zlrs_test_op {
	TEST_RADIO_STATUS,
	TEST_RADIO_START,
	TEST_CHANNELS,
	TEST_ENTROPY_STATUS,
	TEST_ENTROPY_STALL,
	TEST_SAVE_STATUS,
	TEST_SAVE_FAULT,
	TEST_DATA_STATUS,
	TEST_DATA_ARM,
	TEST_DATA_START,
	TEST_DATA_STOP,
	TEST_PHY_STATUS,
	TEST_PHY_PIN,
	TEST_PHY_REQUEST,
	TEST_RESTORE,
	TEST_RF_STATUS,
};

struct zlrs_test_report {
	char text[7168];
	size_t used;
	bool truncated;
};

void zlrs_test_print(struct zlrs_test_report *report, const char *format, ...)
	__attribute__((format(printf, 2, 3)));
int zlrs_test_radio_control(struct zlrs_test_report *report, enum zlrs_test_op op, uint32_t arg);
int zlrs_test_data_control(struct zlrs_test_report *report, enum zlrs_test_op op, uint32_t arg);
int zlrs_test_phy_control(struct zlrs_test_report *report, enum zlrs_test_op op, uint32_t arg);

/* Test-only fixture access; all live mutations occur on the actual RF owner. */
void zlrs_test_radio_apply(zlrs_link_t *link);
void zlrs_test_pairing_save_fault_apply(void);
int zlrs_test_pairing_save_fault_control(struct zlrs_test_report *report, enum zlrs_test_op op,
					 uint32_t arg);

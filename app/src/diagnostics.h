/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
/* Diagnostic IDs belong to the application; management converts their wire representation. */
typedef enum {
	ZLRS_DIAGNOSTIC_LINK = 0,
	ZLRS_DIAGNOSTIC_RATE,
	ZLRS_DIAGNOSTIC_RADIO,
	ZLRS_DIAGNOSTIC_POWER,
} zlrs_diagnostic_report_t;

/* Snapshot and render on the owner thread; native SMP carries the complete text. */
int zlrs_diagnostics_report(zlrs_diagnostic_report_t report, const char **text);

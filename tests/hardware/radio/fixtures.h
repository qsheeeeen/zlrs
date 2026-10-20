/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "zlrs/link/link.h"
#include <stddef.h>

struct shell;

/* Test-only fixture access; all live mutations occur on the actual RF owner. */
void zlrs_test_radio_apply_window_request(zlrs_link_t *link);
void zlrs_test_pairing_save_fault_apply(void);
int zlrs_test_pairing_save_fault_command(const struct shell *sh, size_t argc, char **argv);

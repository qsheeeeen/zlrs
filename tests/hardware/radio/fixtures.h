/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "zlrs/link/link.h"

/* Test-only fixture access; all live mutations occur on the actual RF owner. */
void zlrs_test_radio_poll(zlrs_link_t *link);

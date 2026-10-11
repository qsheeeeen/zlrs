/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

/* Called on the application owner; confirmation writes flash in a maintenance window. */
int zlrs_boot_confirm(void);
void zlrs_boot_poll(bool fault);
/* Returns a negative errno if recovery cannot be entered; success reboots. */
int zlrs_boot_recovery(void);

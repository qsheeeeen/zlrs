/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

/* Called on the application owner; confirmation writes flash in a maintenance window. */
int zlrs_boot_confirm(void);
/* Reboot an unconfirmed image on startup fault or host confirmation timeout. */
void zlrs_boot_check_rollback(bool fault);
/* Returns a negative errno if recovery cannot be entered; success reboots. */
int zlrs_boot_recovery(void);

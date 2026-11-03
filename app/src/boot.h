/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

/* Reboot an unconfirmed image on startup fault or host confirmation timeout. */
void zlrs_boot_check_rollback(bool fault);
